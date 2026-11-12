# Architecture

How a frame gets from the wire to user space, which context each piece of code runs in, how the shared data is protected, and the order things are set up and torn down. File names refer to `kernel/` unless a path is given.

## The path of one frame

```
CANH / CANL
    |
SN65HVD230 transceiver
    |   CRX to P9_24, CTX from P9_26 (pin mux set by dts/can_analyzer.dts)
DCAN1 controller, 481d0000.can
    |   interrupt, then NAPI poll in the c_can driver
netif_receive_skb()
    |
af_can: can_receive()          rcu_read_lock()
    |   walks the receive lists of can1
    +--> analyzer_rx()         data frames (id 0, mask 0)
    +--> analyzer_rx_err()     error frames (mask CAN_ERR_FLAG | CAN_ERR_MASK)
                               rcu_read_unlock(), then the skb is freed
```

Inside `analyzer_rx()` (can_analyzer_rx.c), in softirq context:

```
enable is 0                   -> return, nothing counted
t = ktime_get_ns()
frames_seen++
analyzer_stats_frame()        totals, bits for the load estimate, per-ID count
analyzer_filter_accept()      under RCU, no lock
    dropped by a rule         -> frames_filtered++, return
copy the frame out of the skb
spin_lock(rx_lock), kfifo_put(), spin_unlock(rx_lock)
    stored                    -> wake_up_interruptible(&analyzer_wq)
    buffer full               -> frames_dropped++
```

And on the reading side, in process context:

```
read()
    analyzer_rx_pop()         spin_lock_bh(rx_lock), kfifo_get()
    nothing there             -> O_NONBLOCK ? -EAGAIN
                                 : wait_event_interruptible(analyzer_wq, !empty)
    format one line, copy_to_user()
```

## Execution contexts

| Code | Context | What it may do |
|-|-|-|
| `analyzer_rx()`, `analyzer_rx_err()` | softirq (NET_RX) | no sleeping: spinlocks, `GFP_ATOMIC`, no mutex |
| `analyzer_stats_frame()`, `analyzer_stats_error()`, `analyzer_nl_error_event()` | softirq, called from the above | same rules |
| `rate_tick()` (hrtimer) | hard interrupt | arithmetic on counters only, no locks |
| `read()`, `poll()`, `ioctl()` on the device | process | may sleep, uses `spin_lock_bh()` against the softirq |
| netlink handlers | process | `GFP_KERNEL` allocations |
| sysfs `enable` and `frame_count` | process | `READ_ONCE()` and `WRITE_ONCE()` |
| `analyzer_init()`, `analyzer_exit()` | process (insmod, rmmod) | may sleep, calls `synchronize_rcu()` |

## The frame buffer

The buffer is a kfifo of `struct analyzer_record`: a `u64` timestamp and a `struct can_frame`. `kfifo_alloc()` rounds the size up to a power of 2, so the default of 256 stays 256 and `fifo_size=3` became 4 on the board.

When the kfifo is full, `kfifo_put()` returns 0 and stores nothing. The new frame is dropped and `frames_dropped` counts it. The buffer never overwrites the oldest frame. On the board, six frames into a buffer of four with no reader kept the first four (0x201 to 0x204) and dropped the last two.

The frame is copied out of the skb because the skb belongs to af_can, which frees it once the receive callbacks return.

A kfifo is only safe without a lock when there is exactly one producer and one consumer. Here the producer is the softirq and the consumers are however many processes read the device, so `rx_lock` covers every access. The softirq takes it with `spin_lock()`. Process context takes it with `spin_lock_bh()`: if a reader held the lock and the softirq ran on the same CPU and tried to take it, it would spin forever, and `_bh` keeps the softirq off that CPU while the lock is held. With several readers each frame goes to exactly one of them.

Readers sleep on the wait queue `analyzer_wq`. `read()` sleeps in `wait_event_interruptible()` until the buffer is not empty, and a signal ends the sleep, so Ctrl+C works. `poll()` puts the caller on the same queue with `poll_wait()` (which does not sleep) and reports `EPOLLIN | EPOLLRDNORM` when a frame is waiting. The softirq wakes the queue after every frame it stores.

Each `read()` returns one line, `t=<ns> id=0x<id> len=<n> data=<bytes>`, with 3 hex digits for a standard ID and 8 for an extended one. The line is built in a 96 byte buffer, so a `read()` asking for less than 96 bytes gets `EINVAL`.

## Filter rules

A rule (can_analyzer_filter.c) holds an ID, a mask and an action. Before it is stored, `normalize()` adds `CAN_EFF_FLAG` to the mask, cuts the mask down to 11 or 29 ID bits, and keeps `id & mask`. So the extended flag is always compared, a standard rule never matches an extended frame, and an ID with bits outside its range is refused with `EINVAL`.

There are two kinds of rule, and they are stored differently:

- An exact rule has every ID bit in its mask. It matches one ID, so it goes in a hash table keyed by that ID (`DEFINE_HASHTABLE(exact_rules, 6)`, 64 buckets) and one lookup finds it.
- A masked rule covers many IDs, so hashing the frame's ID can't find it. Masked rules sit in a list and are checked in the order they were added.

`analyzer_filter_accept()` checks the hash table first, then the list, and the first match decides. With no match the frame is accepted.

The receive path only reads the rules, under `rcu_read_lock()` with the `_rcu` iterators, and never takes a lock. Adding and deleting happen in process context (ioctl or netlink) under `rules_lock`. A plain `spin_lock()` is enough there, because the softirq never takes `rules_lock`. A delete unlinks the rule with `hash_del_rcu()` or `list_del_rcu()` under the lock, drops the lock, and frees it with `kfree_rcu()`, which waits until every reader that might still be looking at the rule has finished. `synchronize_rcu()` is never called with the spinlock held, because it sleeps.

Errors: more than 64 rules gives `ENOSPC`, a duplicate `EEXIST`, deleting a rule that doesn't exist `ENOENT`, and an action other than accept or drop `EINVAL`.

## Statistics

can_analyzer_stats.c keeps the totals in atomics: `frames_total`, `bits_total` and `errors_total` as `atomic64_t`, and one `atomic_t` per error class. Every data frame is counted before filtering.

Per-ID counts live in a second hash table (`DEFINE_HASHTABLE(id_stats, 8)`, 256 buckets) under `id_lock`, which the softirq takes with `spin_lock()` and `/proc` with `spin_lock_bh()`. A new ID is allocated in the softirq, so with `GFP_ATOMIC`. The table stops at 256 IDs, and frames with IDs past that, or whose allocation failed, are added to `ids_untracked`, so memory stays bounded whatever is on the bus.

For the load estimate each frame is counted as 47 bits (standard) or 67 bits (extended), plus 8 per data byte. Derived from the frame format: SOF 1, ID 11, RTR 1, IDE 1, r0 1, DLC 4, CRC 15, CRC delimiter 1, ACK slot 1, ACK delimiter 1, EOF 7 and the 3 bit gap after the frame make 47. An extended frame adds SRR 1, 18 more ID bits and r1 1, so 67. Remote frames have no data field. Stuff bits depend on the bit pattern and are not counted, so the estimate is a lower bound.

An hrtimer fires every 1000 ms. `rate_tick()` takes the change in `frames_total` and `bits_total` since the last tick and turns it into `rate_fps` and `load_x10`, the load in tenths of a percent (`bits * 1000 / bitrate`), and keeps the peaks. It runs in hard interrupt context, so it only reads atomics and writes plain `u32` values with `WRITE_ONCE()`, which readers pick up with `READ_ONCE()`. `hrtimer_forward_now()` keeps the period at one second.

`/proc/can_analyzer/stats` is a single `seq_file` (`proc_create_single()`). `analyzer_stats_snapshot()` reads the top counters once into one struct, and both `/proc` and the netlink reply print from it, so the two always agree.

## Error frames

The CAN core keeps error frame receivers in their own list. A receiver goes there only when `CAN_ERR_FLAG` is in its mask, and an error frame is delivered when its class bits overlap that mask. A receiver with mask 0 gets every data frame but never an error frame. So the module registers a second time with id 0 and mask `CAN_ERR_FLAG | CAN_ERR_MASK`, which takes every class.

`analyzer_stats_error()` counts each class bit set in the error frame's ID: tx_timeout, lost_arbitration, controller, protocol, transceiver, no_ack, bus_off, bus_error, restarted and counters. For a controller error it keeps `data[1]` as `last_ctrl_status`. Values seen on the board: 0x08 TX warning, 0x20 TX passive, 0x40 back to error active.

The c_can driver sends bus error frames (ACK, CRC, stuff, form, bit) only when the interface is up with `berr-reporting on`. State changes (warning, passive, bus-off) come either way. `scripts/setup_can.sh` does not turn berr-reporting on.

## Control plane

### ioctl

`/dev/can_analyzer` takes three ioctl calls, defined in `can_analyzer_uapi.h` with magic `0xCA`:

| Call | Number | Permission |
|-|-|-|
| `CAN_ANALYZER_ADD_RULE` | `_IOW(0xCA, 1, struct can_analyzer_rule)` | `CAP_NET_ADMIN` |
| `CAN_ANALYZER_DEL_RULE` | `_IOW(0xCA, 2, struct can_analyzer_rule)` | `CAP_NET_ADMIN` |
| `CAN_ANALYZER_LIST_RULES` | `_IOR(0xCA, 3, struct can_analyzer_rule_list)` | anyone |

The uapi structs use only `__u32` fields, so the layout is the same for the C tool and for `scripts/can_rules.py`, which packs them with Python's `struct` ("III"). The list is allocated with `kzalloc()`, because the whole struct is copied to user space and the unused slots must be zeros, not old kernel memory. An unknown command gets `ENOTTY`.

### Generic netlink

The family is `can_analyzer`, version 1 (can_analyzer_netlink.c):

| Command | Who | What |
|-|-|-|
| `CAN_ANALYZER_CMD_GET_STATS` | anyone | replies with every counter |
| `CAN_ANALYZER_CMD_ADD_RULE` | `GENL_ADMIN_PERM` | same code as the ioctl |
| `CAN_ANALYZER_CMD_DEL_RULE` | `GENL_ADMIN_PERM` | same code as the ioctl |
| `CAN_ANALYZER_CMD_ERROR_EVENT` | sent by the kernel | one per error frame, to the group `events` |

`GENL_ADMIN_PERM` makes the generic netlink core check `CAP_NET_ADMIN` before the handler runs. The policy makes the core check that the rule attributes are `u32`. 64 bit counters go out with `nla_put_u64_64bit()` and a pad attribute, so they stay 8 byte aligned.

Error events start in the softirq. `analyzer_nl_error_event()` first asks `genl_has_listeners()` and returns at once when nobody has joined the group, so an idle analyzer builds no messages. Otherwise it allocates with `GFP_ATOMIC` and sends with `genlmsg_multicast()`.

`userspace/can_analyzer_tool.c` uses libnl-genl. It resolves the family by name, then either sends `GET_STATS` and prints the reply, sends `ADD_RULE` or `DEL_RULE` and waits for the kernel's ACK, or joins `events` and prints each event as it comes.

### sysfs

`enable` (`DEVICE_ATTR_RW`, mode 0644, so only root can write) and `frame_count` (`DEVICE_ATTR_RO`) are an attribute group attached to the misc device through its `.groups` field. The driver core creates them together with the device and removes them with it, so user space never sees the device without its attributes.

`enable_store()` parses the value with `kstrtobool()` (1, 0, y, n, on, off and so on) and refuses anything else with `EINVAL`. `analyzer_enabled` is a single bool, written with `WRITE_ONCE()` and read in the softirq with `READ_ONCE()`, so the compiler can't keep a stale copy in a register or split the access. No lock is needed: a frame that arrives while the value changes is either counted or not, and both are fine.

## Loading

`analyzer_init()` in can_analyzer_rx.c:

1. `kfifo_alloc()`
2. `dev_get_by_name(&init_net, ifname)` and a check that it is `ARPHRD_CAN`. This takes a reference on the interface, released at unload.
3. `misc_register()`: `/dev/can_analyzer` and its sysfs attributes
4. `analyzer_stats_init()`: the `/proc` entry and the hrtimer
5. `genl_register_family()`
6. `can_rx_register()` for data frames
7. `can_rx_register()` for error frames

The receivers come last because a callback can run as soon as it is registered, and the callbacks use the fifo, the wait queue, the statistics and the netlink family. If a step fails, the goto chain undoes the earlier steps in reverse.

## Unloading

`analyzer_exit()`:

1. `can_rx_unregister()` for error frames, then for data frames, with the same arguments that were used to register, which is how af_can finds the entries
2. `synchronize_rcu()`
3. `genl_unregister_family()`
4. `analyzer_stats_exit()`: `hrtimer_cancel()` (waits for a running callback), `proc_remove()` (waits for readers), free the per-ID entries
5. `misc_deregister()`
6. `dev_put()`
7. print the summary and any unread frames, `kfifo_free()`
8. free the filter rules

Step 2 is there because af_can calls the receivers inside `rcu_read_lock()` in `can_receive()`, and `can_rx_unregister()` only unlinks the entry with `hlist_del_rcu()` and frees it later with `call_rcu()`. When `can_rx_unregister()` returns, a callback that started just before can still be running. `synchronize_rcu()` returns only after every RCU reader that started before it has finished, and from then on nothing can reach the fifo or the statistics.

While `/dev/can_analyzer` is open the module can't be unloaded at all. `.owner = THIS_MODULE` in the `file_operations` makes `open()` take a reference on the module, and `rmmod` refuses with "Module can_analyzer is in use". So when `analyzer_exit()` runs, no ioctl can be in progress, and the rules can be freed with a plain `kfree()`.

## The device tree overlay

`dts/can_analyzer.dts` has four parts, which `dtc` turns into four fragments:

- `/chosen/overlays`: a property named `can_analyzer`, so `/proc/device-tree/chosen/overlays/` shows the overlay was applied.
- `&ocp`: disables `P9_24_pinmux` and `P9_26_pinmux`, the cape-universal helper nodes that would otherwise claim the two pins.
- `&am33xx_pinmux`: the node `can_analyzer_dcan1_pins` with the pin settings.
- `&dcan1`: `status = "okay"`, and `pinctrl-0` pointing at that node.

`pinctrl-single,pins` takes three numbers per pin: the pad offset from the control module's pad base `0x44e10800`, the pad configuration and the mux mode. The two are ORed into the pad register.

| Pin | Pad | Offset | Conf | Mux | Register | Function |
|-|-|-|-|-|-|-|
| P9_24 | uart1_txd | 0x184 | 0x30 | 2 | 0x32 | dcan1_rx |
| P9_26 | uart1_rxd | 0x180 | 0x10 | 2 | 0x12 | dcan1_tx |

In `include/dt-bindings/pinctrl/am33xx.h` and `omap.h` terms, 0x10 is `PULL_UP` (`PIN_OUTPUT_PULLUP`) and 0x30 is `INPUT_EN | PULL_UP` (`PIN_INPUT_PULLUP`). Bit 3, `PULL_DISABLE`, is clear in both, so the pull-up is on. `INPUT_EN` turns on the pad's input receiver, without which DCAN1 never sees the bus.

The overlay is compiled with `dtc -@`, which keeps the labels as `__symbols__` and `__fixups__`. U-Boot uses them to resolve `&ocp`, `&am33xx_pinmux` and `&dcan1` against the board's base device tree when it applies the overlay at boot. On the board, after a reboot, the two pad registers read `44e10980 00000012` and `44e10984 00000032`, and three frames from `can0` reached `can1` with `can0` still ERROR-ACTIVE and both of its error counters at 0.
