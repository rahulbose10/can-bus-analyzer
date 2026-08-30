# Development Log

## Phase 0 — Setup
- Set up Ubuntu 22.04 VM with ARM cross-compiler toolchain
- Created project directory structure
- Initialized git repo and pushed to GitHub
- Reading done: LDD3 Ch 1-2, Billimoria Ch 1-2
## 2026-08-05 — Week 0, session 2
- Built and loaded hello.ko natively on the VM (6.8.0-136-generic).
  dmesg: "hello: loaded by pid <YOUR REAL NUMBER> (insmod)".
- <what actually happened when you built it — a real error you hit and
  fixed, or honestly "built and loaded on the first try">
- Traced __init/__exit to the linker level: __exit is a section attribute
  that places code in .exit.text, which vmlinux.lds.h DISCARDs for
  built-in kernel code (no rmmod path can ever exist there) but keeps
  intact for .ko builds, where rmmod needs to call it.
- ARM cross-build still blocked: BBB unopened, don't know its kernel
  version yet.
