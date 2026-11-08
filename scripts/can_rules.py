#!/usr/bin/env python3
"""Add, delete and list can_analyzer filter rules through ioctl.

usage:
  can_rules.py list
  sudo can_rules.py add <id> <mask> accept|drop
  sudo can_rules.py del <id> <mask>

id and mask are hex. An id written with 8 hex digits is a 29-bit
(extended) id, the same rule cansend uses.
"""

import fcntl
import os
import struct
import sys

DEV = "/dev/can_analyzer"
EFF = 0x80000000
MAX_RULES = 64
RULE = struct.Struct("III")
ACTIONS = {"accept": 1, "drop": 2}


def ioc(direction, nr, size):
    # Same layout as _IOW/_IOR in the kernel: dir, size, magic 0xCA, number.
    return (direction << 30) | (size << 16) | (0xCA << 8) | nr


ADD = ioc(1, 1, RULE.size)
DEL = ioc(1, 2, RULE.size)
LIST = ioc(2, 3, 4 + MAX_RULES * RULE.size)


def parse_id(text):
    digits = text[2:] if text.lower().startswith("0x") else text
    value = int(digits, 16)
    return value | EFF if len(digits) == 8 else value


def call(cmd, buf):
    fd = os.open(DEV, os.O_RDONLY)
    try:
        fcntl.ioctl(fd, cmd, buf)
    finally:
        os.close(fd)


def show():
    buf = bytearray(4 + MAX_RULES * RULE.size)
    call(LIST, buf)
    (count,) = struct.unpack_from("I", buf)
    for i in range(count):
        can_id, mask, action = RULE.unpack_from(buf, 4 + i * RULE.size)
        if can_id & EFF:
            ident = "%08x" % (can_id & ~EFF)
        else:
            ident = "%03x" % can_id
        name = "accept" if action == ACTIONS["accept"] else "drop"
        print("%s mask=%x %s" % (ident, mask, name))
    print("%d rule(s)" % count)


def main(args):
    if args == ["list"]:
        show()
    elif len(args) == 4 and args[0] == "add" and args[3] in ACTIONS:
        rule = RULE.pack(parse_id(args[1]), int(args[2], 16), ACTIONS[args[3]])
        call(ADD, rule)
    elif len(args) == 3 and args[0] == "del":
        call(DEL, RULE.pack(parse_id(args[1]), int(args[2], 16), 0))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except OSError as e:
        sys.exit("error: %s" % e.strerror)
