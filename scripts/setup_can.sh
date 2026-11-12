#!/bin/bash
# Bring up both CAN controllers on the BeagleBone Black.
# Needs DCAN0 and DCAN1 enabled in /boot/uEnv.txt: BB-CAN0-00A0 for DCAN0, and
# can_analyzer.dtbo from dts/can_analyzer.dts (or BB-CAN1-00A0) for DCAN1.
# On this board can0 is DCAN0 (481cc000.can) and can1 is DCAN1 (481d0000.can).
# Both go up together, because a node alone on the bus gets no ACK and resends forever.
# Usage: ./scripts/setup_can.sh [bitrate], default 500000.

set -e

BITRATE=${1:-500000}

for dev in can0 can1; do
    if [ ! -e /sys/class/net/$dev ]; then
        echo "$dev not found, are the CAN overlays enabled in /boot/uEnv.txt?"
        exit 1
    fi
done

for dev in can0 can1; do
    sudo ip link set $dev down
    sudo ip link set $dev type can bitrate $BITRATE
    sudo ip link set $dev up
done

ip -details link show can0
ip -details link show can1
