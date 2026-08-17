#!/usr/bin/env python3
"""Build a synthetic FULL-DEVICE CodeFlash image for CI, so the release flow
can be exercised without any vendor firmware.

`tools/build_composite.py` is the one thing in a release that a user must run
before they can flash anything, and its real input is a full CodeFlash dump
taken off their own device with docs/backup-codeflash.py.  CI has no device and
no vendor firmware, so it builds the same SHAPE from
host/make_synthetic_fw.py's synthetic application:

    0x0000..0x00B7   a copy of the application's vector table
    0x00B8..0x3DFF   0x00 fill
    0x3E00..         the synthetic application image (boot-info record first)

That is deliberately the layout of an AFFECTED device -- the one this project
exists for.  The general-purpose full-image tools synthesise 0x0000 from the
application's first 0xB8 payload bytes, and the region above it is the zeroed
would-be bootloader area, which is exactly the "flash programmed with zeros, no
bootloader" state described in the README.  Feeding CI the affected shape means
the smoke test covers the case a real user is in, and lets it assert that
build_composite.py replaces those vectors rather than preserving them.

    python3 .github/scripts/make_test_backup.py OUT.bin [--applen 0x7520]
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "host"))

from make_synthetic_fw import build  # noqa: E402  (needs the path above)

APP_BASE = 0x3E00       # start of the boot-info record / the application half
PAYLOAD_OFF = 0x200     # application payload begins here inside the image
VECTORS = 0xB8          # bytes of vector table the full-image tools copy to 0
CODEFLASH_END = 0x3E800  # end of the array; --all reads all of it


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--applen", default="0x7520",
                    help="application payload length (default 0x7520)")
    ap.add_argument("--all", action="store_true",
                    help="pad to the whole 0x3E800 CodeFlash array with erased "
                         "0xFF, the shape backup-codeflash.py --all produces. "
                         "That is the more thorough of the two backups the "
                         "script offers, and it used to be rejected outright, "
                         "so CI covers it.")
    args = ap.parse_args()

    fw = build(int(args.applen, 0))

    dump = bytearray(b"\x00" * APP_BASE)
    dump[0:VECTORS] = fw[PAYLOAD_OFF:PAYLOAD_OFF + VECTORS]
    dump += fw
    if args.all:
        dump += b"\xff" * (CODEFLASH_END - len(dump))

    with open(args.out, "wb") as f:
        f.write(dump)

    print("wrote %s: %d bytes (0x%X)" % (args.out, len(dump), len(dump)))
    print("  0x0000..0x%04X  application vector-table copy (affected-device shape)"
          % (VECTORS - 1))
    print("  0x%04X..0x3DFF  0x00 fill (no bootloader)" % VECTORS)
    print("  0x3E00..0x%04X  synthetic application"
          % (APP_BASE + len(fw) - 1))
    if args.all:
        print("  0x%04X..0x%04X  0xFF fill (erased array tail, --all shape)"
              % (APP_BASE + len(fw), len(dump) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
