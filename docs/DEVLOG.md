# Development Log

## Phase 0 - Setup
- Set up Ubuntu 22.04 VM with ARM cross-compiler toolchain
- Created project directory structure
- Initialized git repo and pushed to GitHub
- Reading done: LDD3 Ch 1-2, Billimoria Ch 1-2

## 2026-08-05 - Week 0, session 2
- Built and loaded hello.ko natively on the VM (6.8.0-136-generic).
  dmesg: "hello: loaded by pid 22310 (insmod)".
- On load, dmesg also threw two taint warnings: "loading out-of-tree module taints kernel" and "module verification failed: signature and/or required key missing - tainting kernel." That's a different mechanism than the MODULE_LICENSE taint I read about earlier, this one's about module signing, not licensing. My VM has no signing key set up, so anything built outside the actual kernel tree gets flagged as unsigned. Doesn't stop it from loading, just marks the kernel as running code nobody verified.
- Traced __init/__exit to the linker level: __exit is a section attribute
  that places code in .exit.text, which vmlinux.lds.h DISCARDs for
  built-in kernel code (no rmmod path can ever exist there) but keeps
  intact for .ko builds, where rmmod needs to call it.
- ARM cross-build still blocked: BBB unopened, don't know its kernel
  version yet.

## 2026-08-30 - Week 0, session 3
- Brought up vcan0. Got mtu 72 instead of the 16 I expected, this
  kernel's vcan driver defaults to CAN FD sized frames rather than
  classic can_frame. Classic 8-byte frames still work fine over it.
- candump/cansend loopback confirmed working, sent 123#DEADBEEF,
  candump showed it immediately.
- lsmod before had vcan and can_dev only. After sending a frame,
  can_raw and can both appeared, can_raw depends on can. vcan's own
  use count stayed 0 the whole time.
- Saved setup as scripts/setup_vcan.sh.


## 2026-09-22 - Week 0, session 4
- Read through struct can_frame and struct canfd_frame in the actual kernel header, /usr/include/linux/can.h. Saw the real fields, not just a description of them.
- Also found struct can_filter in the same header, the exact filter formula from earlier is a real kernel struct, not something made up for teaching. Confirmed it uses can_id and can_mask.
- Sent four different frame shapes on vcan0 and watched candump print each one differently: a normal frame with data, an empty frame with no data, an extended ID frame, and a remote request frame.
- Ran the filter test, candump vcan0,123:7FF. Hit a snag first, both terminals froze after running things too fast without pausing to check output between steps. Fixed it with pgrep -a candump to check nothing was stuck running, and ip -details link show vcan0 to confirm the interface was still up. Both came back clean, nothing was actually broken.
- Retried slower, one command at a time. Sent ID 123, it showed up in candump. Sent ID 124, it did not show up. Filter correctly let one through and blocked the other.
- Lesson for myself: a filter that shows nothing isn't automatically broken, it might just mean nothing matching has happened yet. Have to test with something that should match before assuming it's broken.
 

## 2026-09-28 - Week 0, session 6
- Checked the receive lists live through /proc/net/can. After sudo modprobe can, all 8 files were there: rcvlist_all, rcvlist_eff, rcvlist_err, rcvlist_fil, rcvlist_inv, rcvlist_sff, reset_stats, stats.
- rcvlist_all was empty before anything ran. Started candump vcan0 and one entry showed up, can_id 000, can_mask 00000000, ident raw, matches 0. That's candump's subscription, mask 0 means everything.
- Sent 123#11223344, matches went to 1. Sent 456#AABB, matches went to 2. No filtering at all with mask 0, every frame gets handed over.
- Closed candump with Ctrl+C and the entry disappeared. Closing the program removes its subscription.
- Started candump vcan0,123:7FF. This time rcvlist_all stayed empty and the entry showed up in rcvlist_fil instead, can_mask 000007ff. A filtered subscription goes in a different list.
- Sent 123#11, delivered. Sent 124#22, never showed up. Then sent 00000123#AA, an extended frame, and it got through. matches ended at 2. Mask 7FF only checks the bottom 11 bits. The extended flag is bit 31, outside the mask, so nobody checks it.
- Restarted with candump vcan0,123:800007FF. Sent 123#11, delivered. Sent 00000123#AA, blocked this time. matches ended at 1. Adding bit 31 to the mask made the filter check the extended flag too.
- Lesson: the filtering happens inside the kernel, before candump sees anything. And a 7FF mask alone doesn't keep extended frames out.



## 2026-10-06 - Week 0, session 7
- Soldered the 6-pin headers onto both CJMCU-230 transceiver boards, using the breadboard to hold the pins straight. Board B's CRX and CTX joints came out lumpy with spiky tips, but they stayed separate. Wrote A and B on the back of the boards.
- Meter on 200 ohm, nothing powered. Board A: CANH to CANL 120.7, 3V3 to GND OL, CANH-CRX, CRX-CTX and CTX-GND all OL. Board B: CANH to CANL 119.7 (119.9 on a re-check), 3V3 to GND OL, the three neighbour pairs all OL. So both boards have their own 120 ohm terminator and no solder bridges.
- Powered the BeagleBone alone from laptop USB, no SD card, it booted from the eMMC and USR0 did the heartbeat. My first try had both measuring wires one socket off, fixed that. P9 pin 1 to pin 3 read 3.41 V. Moved the second wire from pin 3 to pin 32 and got a steady 1.80 V. Pin 32 is the only P9 pin with 1.8 V, so my pair counting is right and the inner row really is the even pins.
- Put the boards on the breadboard, B in columns 20 to 25, A in 40 to 45. First attempt was in the wrong half of the breadboard, moved them. On my breadboard the board pins sit in row f, which is the manual's row e. The manual's row a is my row j.
- Wired board A: W1 black P9 pin 1 to GND, W2 red pin 3 to 3V3, W3 yellow pin 26 to CTX, W4 green pin 24 to CRX, breadboard ends in row j. The first wires I grabbed had female ends and wouldn't go into anything, swapped them for male to male.
- Lesson: the meter tests before power are what made the soldering safe to trust, the joints looked worse than they measured. And pin 32 at 1.8 V proves the counting before any real wire goes in.



## 2026-10-08 - Week 0, session 8
- The evening before (7 Oct), wired board B: W5 black P9 pin 2 to GND, W6 red pin 4 to 3V3, W7 yellow pin 20 to CTX, W8 green pin 19 to CRX. Joined the bus with W9 orange CANH to CANH and W10 blue CANL to CANL, row h on my breadboard, twisted loosely.
- Measured CANH to CANL with everything unpowered. Expected about 60 ohm, two 120 ohm terminators in parallel (derived, 120 x 120 / 240). Got about 120 instead, and it jumped around: 121, 131, 157, 181.
- Beep tests on W9 and W10 both passed, so the bus wires were fine. Lifted W9 to split the boards. Through the breadboard, the board in columns 40 to 45 read about 121 and the board in 20 to 25 read OL. Directly on its own solder joints that second board read about 119, so its terminator was fine. Pressing down on the board made the bus read 61 for a moment. Loose contact somewhere.
- Swapped the two boards. The 120 reading stayed with breadboard columns 20 to 25, not with the board. The clips in those columns are loose. Moved that board and its six wires to columns 28 to 33. After that, 60.4 ohm from one end and 61.1 from the other. Board A now sits in 28 to 33 as DCAN0, board B in 40 to 45 as DCAN1.
- Wired the CP2102 to J1, GND to pin 1, TXD to pin 4, RXD to pin 5. Beep test from the CP2102 GND to the transceiver GND passed. sudo picocom -b 115200 /dev/ttyUSB0 gave the login prompt (I'm not in the dialout group yet). picocom had local echo off, so seeing my typing come back meant both wires worked.
- First login forced a password change, the default one is expired on this image.
- Board is a rev D, not rev C like I'd assumed. /proc/device-tree/model says TI AM335x BeagleBone Black rev D, EEPROM reads A335BNLT 00D0. Kernel 6.12.28-bone25, image is Debian Bookworm Base 2025-05-27.
- No BB-CAN0 or BB-CAN1 overlay anywhere on this image, searched /boot, /usr and /opt. But /boot/config shows CONFIG_CAN_C_CAN_PLATFORM=m and CAN_RAW=m, so the driver is there, just nothing turning the controllers on.
- Mistake on shutdown: after sudo poweroff, Ctrl+A then Ctrl+X didn't close picocom, so the CP2102 stayed plugged into the powered-off board for a few minutes. From now on I unplug the CP2102 right after typing sudo poweroff.
- Lesson: if a reading changes when you press on something, it's the contact, not the part. And swapping two things tells you which one the fault follows.



## 2026-10-10 - Week 0, session 9
- First boot from the SD card I flashed with the Trixie image. Held S2 while plugging in the USB power. The laptop made its USB unplug and plug sound once while the board was booting, that worried me, so I pulled the cable and started again with S2 held. The boot log later showed bb-usb-gadgets.service starting, which is probably what the laptop noticed. findmnt -n -o SOURCE / printed /dev/mmcblk0p3, so root is on the SD card. Login banner says Debian GNU/Linux 13, BeagleBoard.org Debian Trixie Base Image 2026-09-20, and uname -r says 6.12.109-bone72.
- Pasted findmnt -n -o SOURCE / into the BeagleBone login: prompt instead of typing the username, twice, then got the password wrong once, so a few Login incorrect before I got in as debian. It forced a password change straight away, same as the eMMC image did.
- On this image /proc/device-tree/model says TI AM335x BeagleBone Black, without the rev D the eMMC system showed on 8 Oct, and /proc/device-tree/compatible lists ti,am335x-bone-black, ti,am335x-bone, ti,am33xx. I haven't compared this with the eMMC system's compatible list or looked up what a rev D device tree changes, so this is only noted for now. Both CAN overlays are on this image, BB-CAN0-00A0.dtbo and BB-CAN1-00A0.dtbo in /boot/dtbs/6.12.109-bone72/overlays.
- Network and tools. eth0 got 192.168.0.210/24 from the router, ping -c 3 deb.debian.org got 3 of 3 back with 0% loss and 50.622 ms average, and timedatectl said System clock synchronized: yes. sudo apt update fetched 15.0 MB and listed 13 upgradable packages, which I left alone on purpose because the Week 1 module has to match the running kernel exactly. sudo apt install -y can-utils installed version 2023.03-1, and which candump cansend gave /usr/bin/candump and /usr/bin/cansend.
- Enabled the overlays. Copied /boot/uEnv.txt to /boot/uEnv.txt.orig first, then two sed lines turned the commented #uboot_overlay_addr4 and #uboot_overlay_addr5 lines into uboot_overlay_addr4=BB-CAN0-00A0.dtbo and uboot_overlay_addr5=BB-CAN1-00A0.dtbo. diff showed only lines 19 and 20 changed, and enable_uboot_overlays=1 was already set on line 10. After sudo reboot it came back on the same kernel from mmcblk0p3, and /proc/device-tree/chosen/overlays listed BB-CAN0-00A0.kernel and BB-CAN1-00A0.kernel, next to four overlays I didn't add: AM335X-PRU-UIO-00A0, BB-ADC-00A0, BB-BONE-eMMC1-01-00A0 and BB-HDMI-IT66122-00A0.
- sudo dmesg | grep -i c_can showed c_can_platform device registered for 481cc000.can (irq 55) and 481d0000.can (irq 56). In /sys/class/net, can0 points to 481cc000.can and can1 to 481d0000.can, so on this board DCAN0 is can0 and DCAN1 is can1. Read off the board, not assumed.
- Brought both up with sudo ip link set can0 up type can bitrate 500000, and the same for can1. dmesg printed setting BTR=1c02 for both. ip -details link show gave ERROR-ACTIVE, bitrate 500000, sample-point 0.875, tq 125, prop-seg 6, phase-seg1 7, phase-seg2 2, brp 3, clock 24000000 on both. Derived: brp 3 at 24 MHz is a 125 ns time quantum, 1 + 6 + 7 + 2 = 16 quanta is 2 us per bit, which is 500000 bit/s, and (1 + 6 + 7) / 16 = 0.875.
- First real frame test failed. candump can1 in the background, cansend can0 123#DEADBEEF, and nothing showed up. can0 ended ERROR-PASSIVE with berr-counter tx 128 rx 0, error-warn 1, error-pass 1, bus-off 0 and TX packets 0. can1 stayed ERROR-ACTIVE with both counters 0 and RX packets 0. 128 is 16 failed sends at 8 each (derived, every transmit error adds 8). My reading: nobody was answering the ACK, which would also explain why it stopped at 128 instead of going bus-off.
- Checked the software side next. The pin mux registers in /sys/kernel/debug/pinctrl read 00000012 at 44e10978 (P9 pin 20, DCAN0 TX), 00000032 at 44e1097c (pin 19, DCAN0 RX), 00000012 at 44e10980 (pin 26, DCAN1 TX) and 00000032 at 44e10984 (pin 24, DCAN1 RX), which is what the two overlays are meant to set.
- Reverse test, can1 sending to can0. candump can0 in the background, cansend can1 321#CAFE. The kernel printed can1: bus-off, can1 ended BUS-OFF with berr-counter tx 248, and can0 got nothing, its RX packets and both error counters stayed 0. So nothing from DCAN1's side got as far as DCAN0, and this time the errors kept counting until bus-off. Both tests pointed at DCAN1's side of the bus, board B in columns 40 to 45 and its wires, not at the software.
- Meter on DC V 20 with the board on: 3V3 to GND read 3.40 V at both boards' breadboard columns. Power looked fine, so I shut down and took board B out. Close photos of its solder joints showed the GND and 3V3 pins had no solder on them at all, just the pin sitting in the hole with a gap around it and the copper pad bare. The 3.40 V was on the header pin in the breadboard, but without solder nothing connected that pin to the board, so transceiver B had no power. A chip with no power neither hears the bus nor drives it, which fits both failed tests.
- Soldered GND and 3V3 on board B. Took board A out as well to photograph its joints, all six are soldered on A. Meter on 200 ohm, probes touching read 0.1. Board B on its own: CANH to CANL 119.7, same as on 6 Oct, and 3V3 to GND, GND to CTX and CTX to CRX all OL, so the new solder didn't bridge anything.
- Put both boards back, A in columns 28 to 33 as DCAN0 and B in 40 to 45 as DCAN1, and measured the bus across B's CANH and CANL: 119.7, so only one terminator. I had pushed both boards into row e. On my breadboard they belong in row f, the manual's row e, on the same side of the middle gap as the wire ends in rows h and j. Moved them to row f and the bus read 60.6.
- Cold start again from the SD card, uname -r 6.12.109-bone72, both interfaces up at 500000. Pasted candump can1 & and cansend can0 123#DEADBEEF in one go along with the ip link lines and got nothing on screen. Typed cansend can0 123#DEADBEEF again on its own and candump printed can1 123 [4] DE AD BE EF. Stats after that: can0 TX 2 packets, 8 bytes, can1 RX 2 packets, 8 bytes, both ERROR-ACTIVE with berr-counter tx 0 rx 0 and no warn, passive or bus-off events. So the first frame crossed the bus too, it most likely went out before candump had started listening.
- cansend can0 12345678#AABB came through as can1 12345678 [2] AA BB, an extended frame with a 29-bit ID. These are real frames through both transceivers and the two bus wires, no loopback mode anywhere. The hardware is done.
- Same shutdown mistake as 8 Oct. After sudo poweroff I watched the shutdown messages, and the CP2102 was still plugged in when the board printed reboot: Power down. Pulled it then. It has to come out right after Enter on sudo poweroff, not after the output.
- Lesson: a voltage on the breadboard column only proves the pin has power, not the board behind it. The solder joint is what carries it in, and a close photo showed what the meter couldn't. Also, one command at a time: pasting a block hid the first frame and made a working bus look broken.


## Session 10: 11 to 12 Oct 2026, the real bus stopped working

### What happened
- On 10 Oct the two-node bus worked: frames went from one controller to the other with zero errors.
- On 11 Oct, after I reseated the transceiver boards on the breadboard, no frame crossed in either direction.
- Added scripts/setup_can.sh. It brings can0 and can1 up together at 500000, because a node alone on the bus gets no ACK and keeps resending.

### Symptoms
- can0 sends, candump on can1: can0 goes ERROR-PASSIVE with berr-counter tx 128 rx 0 and TX packets 0. can1 shows nothing, no frames and no errors.
- can1 sends, candump on can0: the mirror image, can1 ERROR-PASSIVE with tx 128 rx 0.
- tx stuck at 128 means ACK errors only. The sender reads its own bits back fine, but nobody acknowledges. So each transceiver works on its own and the link between them is broken.
- Once, while I was probing joints with the meter, 2 frames went through (can1 TX 2, can0 RX 2). The fault is intermittent.

### Ruled out
- Power: 3V3 reads 3.40 V at both boards.
- Pin mux and controllers: each controller gets its own bits back through its transceiver.
- P9 wiring: checked against a photo of the header. The CTX and CRX wires read 0.2 ohm and stay steady when bent.
- Idle bus: CANH and CANL both sit at about 1.70 V.
- Breadboard and bus wires: moved both boards to a new breadboard with new CANH and CANL wires, then removed the breadboard completely and put female jumpers straight onto the module pins. Same result every time.

### What the meter shows (power off, boards joined)
- CANH to CANL measured on board A: 120.7 ohm. Measured on board B: 60.5 ohm.
- Two 120 ohm terminators joined by wires must read the same 60 from both ends. One end reading 120 and the other 60 means the connection changes between the two measurements: a joint that opens and closes when pressed.
- Board A alone at its pin tips reads a steady 120.6 to 120.87, even when the pins are pushed.
- Close-up photos of the header joints show cold joints on both boards: spikes, grainy solder, dark voids next to the CANL and CANH pins, and a gap between pin and solder on others.

### Current theory, not confirmed yet
- A cold joint on board B's CANH or CANL header pin, open at rest and closed under probe pressure. It fits every reading so far.

### Next
- Measure board B alone at its pin tips, first with a light touch, then with pressure.
- Reflow every header joint on both boards. Check each board alone (about 120, steady) and joined (about 60 from both sides), then run the forward and reverse tests again.

### Mistakes I made while debugging
- Swapped the boards to see if the fault followed one board. For a broken link between two boards that test tells nothing, because both sides look the same.
- Blamed the breadboard. Removing it changed nothing.
- Trusted single meter readings. A volt reading of 0.00 was the probe sitting on flux, not a real zero.
