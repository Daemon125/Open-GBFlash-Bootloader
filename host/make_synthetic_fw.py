#!/usr/bin/env python3
"""Build a synthetic GBFlash firmware image for the test harness.

The protocol layer cares about framing, CRCs, sequencing and lengths — never
about what the payload bytes mean. So the fixtures and the offline rehearsal can
be driven by an image this script generates, and the repository needs no vendor
firmware of any kind.

The image is structurally identical to a real `fw.bin`:

    0x000  u16   marker, 0xFFFF (what every distributed image carries)
    0x002  4     ASCII "LFBG"
    0x006  u16   CRC16 of the application payload
    0x008  u32   application length
    0x00C  u16   CRC16 of header bytes 0x00..0x0B
    0x00E..0x1FF  0xFF padding
    0x200  ...   application payload

The payload's first two words are a plausible vector table, because both the
writer (proto.c app_gates) and the validator (boot.c bl_app_valid) gate on them:

    word 0   initial SP, must satisfy (sp & 0x2FFE0000) == 0x20000000
    word 1   reset vector, Thumb bit set, inside [0x4000, 0x4000 + applen)

Everything after that is a deterministic pattern, so the images are reproducible
and a mis-programmed byte is easy to spot in a failure dump.

    python3 make_synthetic_fw.py out.bin              # default 0x7520 payload
    python3 make_synthetic_fw.py out.bin --applen 0x200
    python3 make_synthetic_fw.py --list-shapes        # interesting edge sizes
"""

import argparse
import struct
import sys

APP_BASE = 0x4000
HDR_LEN = 0x200
SP_VALUE = 0x20008000

_CRC_TABLE = [
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400,
]


def crc16(data):
    """Nibble-table MODBUS-style CRC16, init 0xFFFF — the one this family uses."""
    crc = 0xFFFF
    for b in data:
        crc = _CRC_TABLE[(b ^ crc) & 0x0F] ^ (crc >> 4)
        crc = _CRC_TABLE[((b >> 4) ^ crc) & 0x0F] ^ (crc >> 4)
    return crc


def build(applen):
    """Return a complete synthetic fw.bin of HDR_LEN + applen bytes."""
    if applen < 0x90:
        raise ValueError("applen must be at least 0x90 (a 36-entry vector table)")
    if applen % 4:
        raise ValueError("applen must be a multiple of 4")

    # Payload: a plausible vector table, then a deterministic pattern.
    reset = (APP_BASE + 0x90) | 1          # Thumb, inside the image
    app = bytearray(struct.pack("<II", SP_VALUE, reset))
    # Remaining vector slots point at the same handler; harmless and realistic.
    while len(app) < 0x90:
        app += struct.pack("<I", reset)
    i = 0
    while len(app) < applen:
        app += struct.pack("<I", 0xA5A50000 | (i & 0xFFFF))
        i += 1
    app = bytes(app[:applen])

    hdr = bytearray(b"\xFF" * HDR_LEN)
    hdr[0x00:0x02] = struct.pack("<H", 0xFFFF)
    hdr[0x02:0x06] = b"LFBG"
    hdr[0x06:0x08] = struct.pack("<H", crc16(app))
    hdr[0x08:0x0C] = struct.pack("<I", applen)
    hdr[0x0C:0x0E] = struct.pack("<H", crc16(bytes(hdr[0x00:0x0C])))
    return bytes(hdr) + app


# Sizes worth exercising. A real vendor image is one arbitrary point in this
# space; generating our own lets the suite cover the boundaries it never hits.
SHAPES = {
    "min":         0x90,      # smallest legal: exactly a vector table
    "one-sector":  0x200,     # exactly one erase sector
    "sector+1":    0x204,     # one sector plus a word
    "typical":     0x7520,    # same size as a shipping image
    "odd-tail":    0x7514,    # a different tail, exercises length handling
    "page-exact":  0x7600,    # exact multiple of the 512-byte page size
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", help="output path")
    ap.add_argument("--applen", default="0x7520",
                    help="payload length, decimal or 0x-prefixed (default 0x7520)")
    ap.add_argument("--shape", choices=sorted(SHAPES),
                    help="use a named size from the shape table instead of --applen")
    ap.add_argument("--list-shapes", action="store_true")
    args = ap.parse_args()

    if args.list_shapes:
        for name, n in sorted(SHAPES.items(), key=lambda kv: kv[1]):
            print("  %-12s 0x%05X  (%d bytes payload, %d total)"
                  % (name, n, n, n + HDR_LEN))
        return 0

    if not args.out:
        ap.error("an output path is required unless --list-shapes is given")

    applen = SHAPES[args.shape] if args.shape else int(args.applen, 0)
    img = build(applen)
    with open(args.out, "wb") as f:
        f.write(img)
    print("wrote %s: %d bytes (0x200 header + 0x%X payload), image crc16 0x%04X"
          % (args.out, len(img), applen, crc16(img)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
