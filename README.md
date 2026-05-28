# CAN Bus Analyzer

Linux kernel module for real-time CAN bus traffic analysis on BeagleBone Black
(TI AM3358 DCAN1 + SN65HVD230 transceiver).

**Status:** Work in Progress

## Overview

This project implements a kernel-space CAN bus analyzer that hooks into
the Linux SocketCAN stack using can_rx_register(). It captures, filters,
and analyzes CAN frames with configurable rules and real-time statistics.
