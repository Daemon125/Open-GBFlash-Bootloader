#!/usr/bin/env python3
"""Take a full CodeFlash backup of a GBFlash, over USB, with no jumper.

THIS IS THE BACKUP YOU RESTORE FROM. docs/check-bootloader-region.py --dump
covers only 0x0000..0x3DFF, which is the bootloader region and NOT enough to
put a device back the way it was. This script reads from 0x0000 up through the
end of the application, so `install.py --restore <that file>` returns the
device to exactly the state it was in when you ran this.

READ-ONLY. It sends exactly two opcodes, 0xA1 (QUERY_FW_INFO) and 0xAD
(GET_VARIABLE). It never writes anything, never resets the device, and never
touches DataFlash or the user configuration word. It needs no jumper.

HOW IT READS FLASH
------------------
GBFlash firmware exposes GET_VARIABLE (0xAD), which resolves a caller-supplied
index against a fixed base and returns the value there. The index is not
bounds-checked, so with the right index the read lands anywhere in the address
map -- including memory-mapped CodeFlash at 0x00000000. For 32-bit reads the
firmware computes:

    value = *(uint32_t *)(0x200000D4 + index * 4)

and the index is added as a 32-bit register value, so an index that "wraps"
below the base still resolves correctly:

    index = ((addr - 0x200000D4) // 4) & 0xFFFFFFFF

HOW MUCH IT READS
-----------------
By default it reads the boot-info record at 0x3E00 first, takes the application
length from it, and reads 0x0000 .. 0x4000 + applen - 1. On a stock L15 device
that is 0x0000..0xB51F, 46,368 bytes. Override with --end, or use --all to read
the whole 250 KB CodeFlash array (slow, and mostly erased 0xFF).

The output file is opened and written incrementally BEFORE the read finishes,
so an interrupted read leaves a short file rather than nothing at all. A short
file is not a backup -- rerun it -- but it is better than discovering at the
end that the destination was not writable.

USAGE
-----
    python3 backup-codeflash.py backup/device_full.bin
    python3 backup-codeflash.py backup/device_full.bin /dev/cu.usbserial-XXXX
    python3 backup-codeflash.py --all backup/device_all.bin
    python3 backup-codeflash.py --end 0xB520 backup/device_full.bin

Requires pyserial (`pip install pyserial`). Close FlashGBX first -- only one
process can hold the serial port.
"""

import argparse
import glob
import hashlib
import os
import struct
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("This script needs pyserial:  pip install pyserial")

VID, PID = 0x1A86, 0x7523
BAUD = 2000000

QUERY_FW_INFO = 0xA1
GET_VARIABLE = 0xAD

BASE32 = 0x200000D4          # the u32 base GET_VARIABLE resolves against

BOOTINFO_BASE = 0x00003E00   # boot-info record; fw.bin byte 0 lands here
APP_BASE = 0x00004000        # application vectors
CODEFLASH_END = 0x0003E800   # 250 KB; DataFlash begins here
SECTOR = 0x200

# Boot-info record layout (little-endian), from include/boot.h.
HDR_OFF_TAG = 0x02
HDR_OFF_APPCRC = 0x06
HDR_OFF_APPLEN = 0x08
HDR_OFF_HDRCRC = 0x0C
HDR_CRC_COVERAGE = 0x0C
HDR_TAG = b"LFBG"

# Sanity bounds on the length field, so a garbled record cannot send us reading
# for an hour. These mirror the bootloader's own gates.
APP_LEN_MIN = 0x1000
APP_LEN_MAX = CODEFLASH_END - APP_BASE

# GET_VARIABLE as an arbitrary read is only present from this firmware version on.
MIN_FW_VER = 10

_CRC_TABLE = [
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400,
]


def crc16(data):
    """CRC-16/MODBUS, the same one the boot-info record uses."""
    crc = 0xFFFF
    for b in data:
        crc = _CRC_TABLE[(b ^ crc) & 0x0F] ^ (crc >> 4)
        crc = _CRC_TABLE[((b >> 4) ^ crc) & 0x0F] ^ (crc >> 4)
    return crc


def find_port():
    all_ports = list(serial.tools.list_ports.comports())
    ports = [p.device for p in all_ports if p.vid == VID and p.pid == PID]
    if ports:
        if len(ports) > 1:
            print("Multiple candidates: %s -- using the first."
                  % ", ".join(ports))
        return ports[0]

    # Some platforms do not populate vid/pid; fall back to the usual names.
    ports = sorted(glob.glob("/dev/cu.usbserial*")
                   + glob.glob("/dev/cu.wchusbserial*")
                   + glob.glob("/dev/ttyUSB*"))
    if len(ports) == 1:
        print("No VID/PID match; using the only usbserial port present.")
        return ports[0]
    if len(ports) > 1:
        sys.exit("Several usbserial ports and no VID/PID match. Pass one:\n  "
                 + "\n  ".join(ports))
    sys.exit("No GBFlash found (looked for VID:PID %04X:%04X and the usual "
             "serial device names).\nIs it plugged in, and is FlashGBX closed?"
             % (VID, PID))


def read_exact(dev, n, what):
    buf = dev.read(n)
    if len(buf) != n:
        sys.exit("Short read on %s: wanted %d bytes, got %d (%s).\n"
                 "Is FlashGBX still open, or is this not a GBFlash?"
                 % (what, n, len(buf), buf.hex(" ") or "nothing"))
    return buf


def query_fw(dev):
    """QUERY_FW_INFO. Read-only; used to confirm the firmware and version."""
    dev.reset_input_buffer()
    dev.reset_output_buffer()
    dev.write(bytes([QUERY_FW_INFO]))
    dev.flush()

    size = read_exact(dev, 1, "length byte")[0]
    if size != 8:
        sys.exit("Expected a length byte of 8, got %d. This does not look like "
                 "GBFlash firmware." % size)
    info = read_exact(dev, 8, "info block")
    fw = {
        "cfw_id": chr(info[0]),
        "fw_ver": int.from_bytes(info[1:3], "big"),
        "pcb_ver": info[3],
        "fw_ts": int.from_bytes(info[4:8], "big"),
        "pcb_name": None,
    }
    if fw["cfw_id"] == "L" and fw["fw_ver"] >= 12:
        n = read_exact(dev, 1, "name length")[0]
        fw["pcb_name"] = read_exact(dev, n, "name").decode(
            "utf-8", "replace").strip("\x00").strip()
        read_exact(dev, 1, "capability byte")
        read_exact(dev, 1, "flags byte")
    return fw


def read_word(dev, addr):
    """One 32-bit flash word, returned as 4 bytes in flash order."""
    if addr & 3:
        raise ValueError("0x%08X is not 4-byte aligned" % addr)
    index = ((addr - BASE32) // 4) & 0xFFFFFFFF
    if index == 0xFF:
        raise ValueError("index 0xFF is reserved by the firmware")
    dev.reset_input_buffer()
    dev.write(bytes([GET_VARIABLE, 4]) + struct.pack(">I", index))
    dev.flush()
    value = int.from_bytes(read_exact(dev, 4, "GET_VARIABLE reply"), "big")
    return struct.pack("<I", value)


def read_block(dev, lo, hi):
    """Read [lo, hi) with no progress output. Both must be word-aligned."""
    out = bytearray()
    for addr in range(lo, hi, 4):
        out += read_word(dev, addr)
    return bytes(out)


def read_bootinfo(dev):
    """The 16 bytes at 0x3E00, and what they say the application length is."""
    rec = read_block(dev, BOOTINFO_BASE, BOOTINFO_BASE + 0x10)
    tag = rec[HDR_OFF_TAG:HDR_OFF_TAG + 4]
    applen = struct.unpack("<I", rec[HDR_OFF_APPLEN:HDR_OFF_APPLEN + 4])[0]
    appcrc = struct.unpack("<H", rec[HDR_OFF_APPCRC:HDR_OFF_APPCRC + 2])[0]
    hdrcrc = struct.unpack("<H", rec[HDR_OFF_HDRCRC:HDR_OFF_HDRCRC + 2])[0]
    return {
        "raw": rec,
        "tag": tag,
        "applen": applen,
        "appcrc": appcrc,
        "hdrcrc": hdrcrc,
        "hdrcrc_ok": crc16(rec[:HDR_CRC_COVERAGE]) == hdrcrc,
    }


def main():
    ap = argparse.ArgumentParser(
        description="Full CodeFlash backup of a GBFlash, read-only, no jumper.")
    ap.add_argument("out", help="file to write the image to")
    ap.add_argument("port", nargs="?",
                    help="serial port (auto-detected if omitted)")
    ap.add_argument("--end", metavar="ADDR",
                    help="read up to (not including) this address instead of "
                         "the end of the application, e.g. --end 0xB520")
    ap.add_argument("--all", action="store_true",
                    help="read the whole 250 KB CodeFlash array. Slow, and "
                         "everything past the application is erased 0xFF.")
    args = ap.parse_args()

    if args.end and args.all:
        sys.exit("--end and --all are mutually exclusive.")

    # Make sure we can write the destination BEFORE spending minutes reading.
    # Discovering an unwritable path after the read is how a backup gets lost.
    outdir = os.path.dirname(os.path.abspath(args.out))
    try:
        os.makedirs(outdir, exist_ok=True)
        out_fp = open(args.out, "wb")
    except OSError as e:
        sys.exit("Cannot write %s: %s" % (args.out, e))

    rc = 0
    with out_fp:
        port = args.port or find_port()
        print("port: %s" % port)

        with serial.Serial(port, BAUD, timeout=2) as dev:
            fw = query_fw(dev)
            print("firmware: %s%d, PCB version %d, built %s%s"
                  % (fw["cfw_id"], fw["fw_ver"], fw["pcb_ver"],
                     time.strftime("%Y-%m-%d %H:%M:%S",
                                   time.localtime(fw["fw_ts"])),
                     (", %r" % fw["pcb_name"]) if fw["pcb_name"] else ""))
            if fw["fw_ver"] < MIN_FW_VER:
                sys.exit("GET_VARIABLE-as-a-read needs fw_ver >= %d; this "
                         "device reports %d." % (MIN_FW_VER, fw["fw_ver"]))

            hdr = read_bootinfo(dev)
            print()
            print("boot-info record at 0x%04X: tag %s, applen 0x%X, "
                  "app CRC 0x%04X, header CRC %s"
                  % (BOOTINFO_BASE,
                     hdr["tag"].decode("latin-1")
                     if hdr["tag"] == HDR_TAG else hdr["tag"].hex(" "),
                     hdr["applen"], hdr["appcrc"],
                     "ok" if hdr["hdrcrc_ok"] else "BAD"))

            if args.all:
                end = CODEFLASH_END
                why = "--all"
            elif args.end:
                end = int(args.end, 0)
                why = "--end"
            else:
                if hdr["tag"] != HDR_TAG:
                    sys.exit("The boot-info tag is not 'LFBG', so the "
                             "application length cannot be trusted. Pass "
                             "--end or --all to read anyway.")
                if not (APP_LEN_MIN <= hdr["applen"] <= APP_LEN_MAX):
                    sys.exit("Boot-info application length 0x%X is outside "
                             "[0x%X, 0x%X]. Pass --end or --all to read "
                             "anyway." % (hdr["applen"], APP_LEN_MIN,
                                          APP_LEN_MAX))
                end = APP_BASE + hdr["applen"]
                why = "boot-info record"

            if end & 3:
                end = (end + 3) & ~3
            if not (0 < end <= CODEFLASH_END):
                sys.exit("End address 0x%X is outside CodeFlash." % end)

            print("reading 0x0000..0x%04X (%d bytes), end from %s"
                  % (end - 1, end, why))
            print()

            image = bytearray()
            t0 = time.time()
            for addr in range(0, end, 4):
                word = read_word(dev, addr)
                image += word
                out_fp.write(word)
                if addr % 0x400 == 0:
                    out_fp.flush()
                    el = time.time() - t0
                    rate = (addr / el) if el > 0 else 0
                    eta = ((end - addr) / rate) if rate > 0 else 0
                    print("\r  %3d%% (0x%05X/0x%05X)  %5.0f B/s  ETA %3.0f s"
                          % (100 * addr // end, addr, end, rate, eta),
                          end="", flush=True)
            out_fp.flush()
            print("\r  100%% -- %d bytes in %.0f s%s"
                  % (len(image), time.time() - t0, " " * 24))

    image = bytes(image)

    # ---- verify the dump, because an unchecked dump is not a backup --------
    print()
    print("wrote %s" % args.out)
    print("  %d bytes, sha256 %s"
          % (len(image), hashlib.sha256(image).hexdigest()))
    print()
    print("verification")

    def ok(cond, text, detail=""):
        nonlocal rc
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", text,
                               ("\n         " + detail) if detail else ""))
        if not cond:
            rc = 1
        return cond

    ok(len(image) == end, "the file is the expected length",
       "%d bytes" % len(image))

    if len(image) > BOOTINFO_BASE + 0x10:
        tag = image[BOOTINFO_BASE + HDR_OFF_TAG:BOOTINFO_BASE + HDR_OFF_TAG + 4]
        ok(tag == HDR_TAG,
           "boot-info tag 'LFBG' is present at 0x%04X" % (BOOTINFO_BASE + 2),
           tag.hex(" "))
        rec = image[BOOTINFO_BASE:BOOTINFO_BASE + 0x10]
        ok(crc16(rec[:HDR_CRC_COVERAGE]) == hdr["hdrcrc"],
           "the boot-info record's own CRC checks out")

        applen = hdr["applen"]
        if len(image) >= APP_BASE + applen:
            payload = image[APP_BASE:APP_BASE + applen]
            got = crc16(payload)
            ok(got == hdr["appcrc"],
               "application payload CRC matches the boot-info record",
               "computed 0x%04X, record says 0x%04X" % (got, hdr["appcrc"]))
        else:
            print("  [skip] payload CRC — the dump stops before the end of "
                  "the application")

        sp = struct.unpack("<I", image[APP_BASE:APP_BASE + 4])[0]
        ok((sp & 0x2FFE0000) == 0x20000000,
           "the application's initial SP is SRAM-shaped", "0x%08X" % sp)

    if len(image) > APP_BASE + hdr["applen"] + 4:
        tail = image[APP_BASE + hdr["applen"]:]
        nonff = sum(1 for b in tail if b != 0xFF)
        ok(nonff == 0, "everything past the application is erased (0xFF)",
           "%d non-0xFF bytes in %d" % (nonff, len(tail)))

    reset = struct.unpack("<I", image[4:8])[0]
    if (reset & ~1) < BOOTINFO_BASE:
        where = "the bootloader"
    elif (reset & ~1) >= APP_BASE:
        where = "the application (no bootloader installed)"
    else:
        where = "somewhere unexpected"
    print("  [info] reset vector at 0x0004 is 0x%08X -- %s" % (reset, where))

    print()
    if rc == 0:
        print("This file is a usable restore image:")
        print("    python3 install.py --restore %s" % args.out)
        print("Keep it somewhere that is not the device.")
    else:
        print("VERIFICATION FAILED. Do not rely on this file as a backup.")
        print("Re-run the read before writing anything to the device.")

    return rc


if __name__ == "__main__":
    sys.exit(main())
