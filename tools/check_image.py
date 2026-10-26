#!/usr/bin/env python3
"""Refuse firmware images that could brick an MK66 or that don't boot.

Checks an Intel HEX image for:
  * Flash configuration field (0x400-0x40F) equal to the Teensy defaults; FSEC must be 0xDE
    (unsecured, mass erase enabled) and FOPT 0xF9, or the MCU can be locked permanently.
  * A sane vector table: SP inside SRAM, Thumb reset vector inside the image.
  * Every data byte lies inside 0x0..0xD6460, the extent of the stock 1.9.2 image. The
    rest of program flash and FlexNVM may hold data the update bootloader or the device
    identity relies on, so nothing may be written there.
  * Only record types 00 (data), 01 (EOF) and 02 (segment address) are used, like stock.
Usage: check_image.py <image.hex>
"""
import sys

STOCK_END = 0xD6460
ALLOWED_RECORDS = {0, 1, 2}

def load_ihex(path):
    mem, upper, types = {}, 0, set()
    for n, line in enumerate(open(path), 1):
        line = line.strip()
        if not line:
            continue
        if line[0] != ":":
            sys.exit(f"line {n}: not Intel HEX")
        raw = bytes.fromhex(line[1:])
        if sum(raw) & 0xFF:
            sys.exit(f"line {n}: bad checksum")
        count, addr, rtype, data = raw[0], (raw[1] << 8) | raw[2], raw[3], raw[4:4 + raw[0]]
        if len(raw) != count + 5:
            sys.exit(f"line {n}: record length mismatch")
        types.add(rtype)
        if rtype == 0:
            for i, b in enumerate(data):
                mem[upper + addr + i] = b
        elif rtype == 2:
            upper = ((data[0] << 8) | data[1]) << 4
        elif rtype == 4:
            upper = ((data[0] << 8) | data[1]) << 16
        elif rtype == 1:
            break
    return mem, types

mem, types = load_ihex(sys.argv[1])
def byte(a): return mem.get(a, 0xFF)
def word(a): return sum(byte(a + i) << (8 * i) for i in range(4))
errors = []
expected_cfg = bytes([0xFF] * 12 + [0xDE, 0xF9, 0xFF, 0xFF])
cfg = bytes(byte(0x400 + i) for i in range(16))
if cfg != expected_cfg:
    errors.append(f"flash config field {cfg.hex()} != {expected_cfg.hex()} (risk of locking the MCU)")
sp, reset = word(0), word(4)
if not (0x1FFF0000 < sp <= 0x20030000):
    errors.append(f"initial SP 0x{sp:08x} outside SRAM")
top = max(mem) + 1
if not (reset & 1) or not (0x40 <= (reset & ~1) < top and not (0x400 <= (reset & ~1) < 0x410)):
    errors.append(f"reset vector 0x{reset:08x} not a Thumb address inside the image")
if top > STOCK_END:
    errors.append(f"image end 0x{top:x} beyond stock extent 0x{STOCK_END:x}")
if min(mem) < 0:
    errors.append("negative address")
if types - ALLOWED_RECORDS:
    errors.append(f"unexpected Intel HEX record types {sorted(types - ALLOWED_RECORDS)}")
if errors:
    print("IMAGE CHECK FAILED:\n  " + "\n  ".join(errors))
    sys.exit(1)
print(f"image ok: {top} bytes, SP=0x{sp:08x}, reset=0x{reset:08x}, flash config unlocked")
