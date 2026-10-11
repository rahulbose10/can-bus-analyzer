#!/bin/bash
# Bring up both CAN controllers on the BeagleBone Black.
# Needs the BB-CAN0-00A0 and BB-CAN1-00A0 overlays enabled in /boot/uEnv.txt.
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
