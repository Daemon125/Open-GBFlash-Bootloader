#!/usr/bin/env python3
"""Answer one question about a GBFlash: is there a bootloader in flash 0x0000..0x3DFF?

READ-ONLY. This script sends exactly two opcodes, 0xA1 (QUERY_FW_INFO) and
0xAD (GET_VARIABLE). It never writes anything, never resets the device, and
never touches DataFlash or the user configuration word. It is safe to run on a
device you care about, and it needs no jumper.

HOW IT READS FLASH
------------------
GBFlash firmware exposes GET_VARIABLE (0xAD), which resolves a caller-supplied
index against a fixed base and returns the value there. The index is not
bounds-checked, so with the right index the read lands anywhere in the address
map -- including memory-mapped CodeFlash at 0x00000000. For 32-bit reads the
firmware computes:

    value = *(uint32_t *)(0x200000D4 + index * 4)

and the index is added as a 32-bit register value, so an index that "wraps"
below the base still resolves correctly. Turning an address into an index is
therefore just:

    index = ((addr - 0x200000D4) // 4) & 0xFFFFFFFF

Reads are harmless. This is the same primitive a full firmware backup uses.

WHAT THE ANSWER LOOKS LIKE
--------------------------
A GBFlash that shipped with no bootloader has:

  * 0x0000..0x00B7 -- a verbatim copy of the application's own vector table and
    reset stub, so that reset lands directly in the application;
  * 0x00B8..0x3DFF -- ALL ZEROS, 15,688 bytes of nothing. This is the
    definitive symptom: there is no code for BOOTLOADER_RESET to reset into,
    which is why firmware updates fail;
  * the reset vector at 0x0004 pointing at 0x4000 or above, i.e. into the
    application.

A GBFlash with this bootloader installed has a non-zero 0x00B8..0x3DFF and a
reset vector at 0x0004 pointing BELOW 0x3E00, into the bootloader itself.

USAGE
-----
    python3 check-bootloader-region.py                  # full read, ~15.7 KB
    python3 check-bootloader-region.py --quick          # sample one word/sector
    python3 check-bootloader-region.py /dev/cu.usbserial-XXXX
    python3 check-bootloader-region.py --dump region.bin

--dump writes only 0x0000..0x3DFF. THAT IS NOT A BACKUP: you cannot restore a
device from it. Use backup-codeflash.py, in this directory, for a full image.

Requires pyserial (`pip install pyserial`). Close FlashGBX first -- only one
process can hold the serial port.
"""

import argparse
import glob
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

REGION_LO = 0x000000B8       # first byte a bootloader would occupy that the
REGION_HI = 0x00003E00       # application's vector copy does not already fill
VECTORS_LO = 0x00000000
VECTORS_HI = 0x000000B8
BOOTINFO_BASE = 0x00003E00
APP_BASE = 0x00004000
SECTOR = 0x200

# GET_VARIABLE as an arbitrary read is only present from this firmware version on.
MIN_FW_VER = 10


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


def read_range(dev, lo, hi, label):
    out = bytearray()
    total = hi - lo
    t0 = time.time()
    for addr in range(lo, hi, 4):
        out += read_word(dev, addr)
        done = addr - lo
        if done % 0x400 == 0:
            print("\r  %s %3d%% (0x%04X/0x%04X)"
                  % (label, 100 * done // total, done, total),
                  end="", flush=True)
    print("\r  %s 100%% -- %d bytes in %.0f s"
          % (label, len(out), time.time() - t0))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(
        description="Read-only check for a GBFlash bootloader region.")
    ap.add_argument("port", nargs="?", help="serial port (auto-detected if omitted)")
    ap.add_argument("--quick", action="store_true",
                    help="sample the first word of each 512-byte sector "
                         "instead of reading all 15,688 bytes")
    ap.add_argument("--dump", metavar="FILE",
                    help="write the bytes read to FILE. Not a backup: this is "
                         "only 0x0000..0x3DFF. Use backup-codeflash.py for an "
                         "image you can restore from. Incompatible with --quick.")
    args = ap.parse_args()

    if args.dump and args.quick:
        sys.exit("--dump needs the full read; it cannot be combined with "
                 "--quick, which only samples one word per sector.")

    # Prove the destination is writable NOW, not after a multi-minute read.
    # Failing at the end would throw away the whole read.
    if args.dump:
        try:
            os.makedirs(os.path.dirname(os.path.abspath(args.dump)),
                        exist_ok=True)
            open(args.dump, "wb").close()
        except OSError as e:
            sys.exit("Cannot write %s: %s" % (args.dump, e))

    port = args.port or find_port()
    print("port: %s" % port)

    with serial.Serial(port, BAUD, timeout=2) as dev:
        fw = query_fw(dev)
        print("firmware: %s%d, PCB version %d, built %s%s"
              % (fw["cfw_id"], fw["fw_ver"], fw["pcb_ver"],
                 time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(fw["fw_ts"])),
                 (", %r" % fw["pcb_name"]) if fw["pcb_name"] else ""))
        if fw["fw_ver"] < MIN_FW_VER:
            sys.exit("GET_VARIABLE-as-a-read needs fw_ver >= %d; this device "
                     "reports %d." % (MIN_FW_VER, fw["fw_ver"]))
        print()

        head = read_range(dev, VECTORS_LO, VECTORS_HI, "0x0000..0x00B7")
        sp = struct.unpack("<I", head[0:4])[0]
        reset = struct.unpack("<I", head[4:8])[0]

        if args.quick:
            print("  sampling one word per 512-byte sector "
                  "(0x00B8..0x3DFF, %d sectors)"
                  % ((REGION_HI - REGION_LO + SECTOR - 1) // SECTOR))
            body = bytearray()
            body += read_word(dev, REGION_LO - (REGION_LO % 4))
            addr = (REGION_LO + SECTOR) & ~(SECTOR - 1)
            while addr < REGION_HI:
                body += read_word(dev, addr)
                addr += SECTOR
            body = bytes(body)
            coverage = "sampled"
        else:
            body = read_range(dev, REGION_LO - (REGION_LO % 4), REGION_HI,
                              "0x00B8..0x3DFF")
            coverage = "complete"

        nonzero = sum(1 for b in body if b != 0x00)
        erased = sum(1 for b in body if b != 0xFF)

    print()
    print("initial SP at 0x0000    : 0x%08X" % sp)
    print("reset vector at 0x0004  : 0x%08X" % reset)
    print("0x00B8..0x3DFF (%s): %d non-zero bytes of %d read"
          % (coverage, nonzero, len(body)))
    print()

    target = reset & ~1
    if nonzero == 0:
        print("VERDICT: NO BOOTLOADER.")
        print()
        print("  The bootloader region is programmed with zeros. Reset fetches")
        print("  the application's own vectors from 0x0000 and enters the")
        print("  application directly, so the BOOTLOADER_RESET command lands in")
        print("  nothing and firmware updates cannot run. This is the device")
        print("  this project exists for.")
        rc = 1
    elif erased == 0:
        print("VERDICT: REGION IS ERASED (all 0xFF), not programmed.")
        print()
        print("  An erased region is not a bootloader either, but it is also not")
        print("  the stock zero-filled state -- something has erased it. Do not")
        print("  install anything until you know what. See docs/RECOVERY.md.")
        rc = 2
    elif target < BOOTINFO_BASE:
        print("VERDICT: A BOOTLOADER IS PRESENT.")
        print()
        print("  The region carries code and the reset vector points into it")
        print("  (0x%08X, below the boot-info record at 0x%04X). Firmware"
              % (target, BOOTINFO_BASE))
        print("  updates should work. Do NOT install over this without reading")
        print("  docs/INSTALLING.md -- it is not necessarily this bootloader.")
        rc = 0
    else:
        print("VERDICT: UNEXPECTED.")
        print()
        print("  The region is not empty, but the reset vector at 0x0004 points")
        print("  to 0x%08X, which is at or above 0x%04X -- into application"
              % (target, APP_BASE))
        print("  space, not into the region. Investigate before writing flash.")
        rc = 3

    if args.dump:
        with open(args.dump, "wb") as f:
            f.write(head + body)
        print()
        print("wrote %s (%d bytes, flash 0x0000..0x%04X)"
              % (args.dump, len(head) + len(body), REGION_HI - 1))
        print("NOTE: this is the bootloader region only. It is NOT a restore "
              "image.")
        print("      For a backup you can write back, use backup-codeflash.py.")

    return rc


if __name__ == "__main__":
    sys.exit(main())
