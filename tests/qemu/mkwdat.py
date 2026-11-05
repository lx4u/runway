#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write the body of a WDAT table that drives the ICH9 TCO timer of QEMU q35.

QEMU adds the ACPI header: -acpitable sig=WDAT,data=<out>. The registers are
those of the TCO block at PMBASE (0x600) + 0x60, as real tables describe them.
"""
import struct
import sys

TCO = 0x600 + 0x60
GCS = 0xFED1C000 + 0x3410       # RCBA + General Control and Status
GCS_NO_REBOOT = 0x20
PRESERVE = 0x80
WRITE_VALUE, WRITE_COUNTDOWN = 2, 3
RESET, SET_COUNTDOWN, SET_RUNNING_STATE, SET_STOPPED_STATE, SET_REBOOT = 0x01, 0x06, 0x09, 0x0B, 0x11


def inst(action, op, port, value, mask, space=1, width=16):
    gas = struct.pack("<BBBBQ", space, width, 0, {8: 1, 16: 2, 32: 3}[width], port)
    return struct.pack("<BBH", action, op, 0) + gas + struct.pack("<II", value, mask)


entries = [
    inst(SET_REBOOT, WRITE_VALUE | PRESERVE, GCS, 0, GCS_NO_REBOOT, space=0, width=32),
    inst(RESET, WRITE_VALUE, TCO + 0x00, 1, 0xFFFF),
    inst(SET_COUNTDOWN, WRITE_COUNTDOWN | PRESERVE, TCO + 0x12, 0, 0x03FF),
    inst(SET_RUNNING_STATE, WRITE_VALUE | PRESERVE, TCO + 0x08, 0x0000, 0x0800),
]
if "--nostop" not in sys.argv:
    entries.append(inst(SET_STOPPED_STATE, WRITE_VALUE | PRESERVE, TCO + 0x08, 0x0800, 0x0800))

# header length, PCI segment/bus/device/function, period (ms), max, min, flags, entry count
head = struct.pack("<IHBBB3xIIIB3xI", 32, 0, 0, 0x1F, 0, 600, 0x3FF, 4, 0, len(entries))
out_file = [a for a in sys.argv[1:] if a != "--nostop"][0]
with open(out_file, "wb") as f:
    f.write(head + b"".join(entries))
