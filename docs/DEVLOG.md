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

