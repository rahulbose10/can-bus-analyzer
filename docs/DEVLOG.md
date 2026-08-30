# Development Log

## Phase 0 — Setup
- Set up Ubuntu 22.04 VM with ARM cross-compiler toolchain
- Created project directory structure
- Initialized git repo and pushed to GitHub
- Reading done: LDD3 Ch 1-2, Billimoria Ch 1-2

## 2026-08-05 — Week 0, session 2
- Built and loaded hello.ko natively on the VM (6.8.0-136-generic).
  dmesg: "hello: loaded by pid 22310 (insmod)".
- On load, dmesg also threw two taint warnings: "loading out-of-tree module taints kernel" and "module verification failed: signature and/or required key missing - tainting kernel." That's a different mechanism than the MODULE_LICENSE taint I read about earlier, this one's about module signing, not licensing. My VM has no signing key set up, so anything built outside the actual kernel tree gets flagged as unsigned. Doesn't stop it from loading, just marks the kernel as running code nobody verified.
- Traced __init/__exit to the linker level: __exit is a section attribute
  that places code in .exit.text, which vmlinux.lds.h DISCARDs for
  built-in kernel code (no rmmod path can ever exist there) but keeps
  intact for .ko builds, where rmmod needs to call it.
- ARM cross-build still blocked: BBB unopened, don't know its kernel
  version yet.

## 2026-08-30 — Week 0, session 3 — Week 0, session 3
- Brought up vcan0. Got mtu 72 instead of the 16 I expected, this
  kernel's vcan driver defaults to CAN FD sized frames rather than
  classic can_frame. Classic 8-byte frames still work fine over it.
- candump/cansend loopback confirmed working, sent 123#DEADBEEF,
  candump showed it immediately.
- lsmod before had vcan and can_dev only. After sending a frame,
  can_raw and can both appeared, can_raw depends on can. vcan's own
  use count stayed 0 the whole time.
- Saved setup as scripts/setup_vcan.sh.
