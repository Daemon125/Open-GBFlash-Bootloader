#!/usr/bin/env python3
"""Build the stage-2 composite CodeFlash image: bootloader + the CURRENT application.

    0x0000 .. len(bl)-1   the bootloader binary
    len(bl) .. 0x3DFF     0xFF fill (erased state)
    0x3E00 ..             the application image, boot-info header included

WHY THIS TOOL EXISTS, AND WHY THE OLD ONES MUST NOT BE USED
-----------------------------------------------------------
General-purpose full-image builders for this device (`gbflash_update.py` and the
like) synthesise 0x0000 by copying the application's first 0xB8 payload bytes --
the app's own vector table -- because a bootloader-less GBFlash has no other way
to get vectors to address 0.  That is correct for a device with no bootloader
and WRONG the moment one exists: the reset vector would point straight at the
app and the bootloader would never run.  They have no flag that suppresses it,
so this tool replaces that step; it never invents vectors.  docs/BUILDING.md
says the same at length.

The application region is taken verbatim.  Nothing in it is recomputed,
re-CRC'd or re-padded -- it is copied byte for byte and then checked.

USAGE
-----
From a release download (bootloader.bin and this script in one directory, plus
the full-device backup you took with backup-codeflash.py):

    python3 build_composite.py --backup my-device-backup.bin --out install.bin

--backup is the whole flow in one flag: the application half is taken from the
backup, and the SAME file is used as the install-safety baseline, so the run
proves the image you are about to flash changes nothing at or above 0x3E00.

From a source checkout, naming every input explicitly:

    python3 tools/build_composite.py \
        --bootloader build/bootloader.bin \
        --app        /path/to/current-device-dump.bin \
        --compare    /path/to/vendor/fw.bin \
        --baseline   /path/to/current-device-dump.bin \
        --out        /path/to/composite.bin

With no --out the image is verified and nothing is written.

Either form accepts a `--all` backup (the whole 250 KB array).  The erased tail
past the end of the application is ignored, and the run says so.

A NOTE ON SKIPPED CHECKS.  There are 26 checks in total.  --compare and
--baseline are optional, and without them four cannot run -- including the
install-safety comparison, the one that proves the write cannot touch your
firmware.  Those are reported as SKIP and named in the RESULT line, so a
weaker verification cannot read as the full one:

    --backup alone                      25 ran, 1 SKIP   (no --compare)
    --backup plus a vendor --compare    26 ran, 0 SKIP
    --app alone, no baseline            22 ran, 4 SKIP

"PASS with N check(s) SKIPPED" is a normal result for the release flow, not a
fault.  --compare pointed at the file --app already names is reported as a SKIP
rather than a PASS: comparing a file with itself is not a second copy, and
counting it would clear the SKIP list and upgrade the RESULT line on the
strength of a tautology.
"""

import argparse
import os
import struct
import sys

# HERE is the directory this script sits in: the release download in a release,
# tools/ in a checkout.  BLDIR is the checkout root in the latter case.  Nothing
# outside the tree is ever referenced.
HERE = os.path.dirname(os.path.abspath(__file__))
BLDIR = os.path.dirname(HERE)

APP_BASE = 0x3E00          # boot-info page; also the bootloader's flash budget
PAYLOAD_OFF = 0x200        # app payload begins here, loads at 0x4000
APP_LOAD = 0x4000
FILL = 0xFF                # erased CodeFlash; programming 0xFF is a no-op
BL_APP_MAX_LEN = 0x3A800   # must match include/boot.h and include/proto.h
BL_APP_MIN_LEN = 0x90

_CRC_TABLE = [
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400,
]


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc = _CRC_TABLE[(b ^ crc) & 0x0F] ^ (crc >> 4)
        crc = _CRC_TABLE[((b >> 4) ^ crc) & 0x0F] ^ (crc >> 4)
    return crc


FAILS = []
SKIPS = []
RAN = []


def check(ok, label, detail=""):
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label,
                           ("\n         " + detail) if detail else ""))
    RAN.append(label)
    if not ok:
        FAILS.append(label)
    return ok


def skip(label, why):
    """Report a check that could not run: not a pass, not a failure, but it
    must be visible.  A run missing --baseline produces 22 checks instead of
    26, and without this the only symptom is a number nobody counts."""
    print("  [SKIP] %s\n         %s" % (label, why))
    SKIPS.append(label)


def die(msg):
    sys.stderr.write("error: %s\n" % msg)
    sys.exit(2)


def read_file(path, what):
    """Read an input file, or fail with a sentence instead of a traceback."""
    if not os.path.exists(path):
        die("%s not found: %s" % (what, path))
    if not os.path.isfile(path):
        die("%s is not a file: %s" % (what, path))
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        die("cannot read %s (%s): %s" % (what, path, e))
    if not data:
        die("%s is empty: %s" % (what, path))
    return data


def hexbytes(b):
    """`bytes.hex(sep)` only exists from Python 3.8; this works everywhere."""
    return " ".join("%02x" % x for x in b)


def app_length(raw, hdr_off):
    """The payload length the boot-info record at hdr_off claims, or None.

    None means "do not trust this" -- no readable record, or a length outside
    the bootloader's own bounds.  The caller then leaves the file alone and
    lets app_valid() report the real problem, rather than acting on a number
    that is already known to be wrong.
    """
    if len(raw) < hdr_off + 14:
        return None
    if raw[hdr_off + 2:hdr_off + 6] != b"LFBG":
        return None
    n = struct.unpack("<I", raw[hdr_off + 8:hdr_off + 12])[0]
    if not (BL_APP_MIN_LEN <= n <= BL_APP_MAX_LEN):
        return None
    return n


def trim_erased_tail(raw, hdr_off, what, path):
    """Drop erased flash past the end of the application.

    backup-codeflash.py --all reads the whole 250 KB array, so the dump is the
    application followed by ~200 KB of 0xFF.  That is the MORE thorough of the
    two backups it offers, so it must not be the one that gets rejected by the
    "length field matches the payload actually present" gate.

    The tail is dropped only when it is genuinely erased.  Anything else in it
    is real content the ISP write is about to erase, and the owner needs to be
    told rather than have it silently discarded.
    """
    n = app_length(raw, hdr_off)
    if n is None:
        return raw, 0
    end = hdr_off + PAYLOAD_OFF + n
    if len(raw) <= end:
        return raw, 0
    tail = raw[end:]
    dirty = next((i for i, b in enumerate(tail) if b != FILL), None)
    if dirty is not None:
        die("%s has %d byte(s) past the end of the application and they are not "
            "erased: %s\n"
            "       first non-0xFF byte at 0x%X (0x%02X).\n"
            "       A CodeFlash backup should be the application followed by "
            "erased (0xFF) flash.  Whatever is\n"
            "       there would be destroyed by the ISP write, which erases "
            "the whole array, so this\n"
            "       is not something to wave through.  Re-take the backup, or "
            "truncate the file to 0x%X bytes\n"
            "       yourself if you know what those bytes are."
            % (what, len(tail), path, end + dirty, tail[dirty], end))
    return raw[:end], len(tail)


def note_trim(ntrim, path):
    if ntrim:
        print("  note: ignoring %d trailing erased (0xFF) byte(s) in %s.\n"
              "        A --all backup reads the whole array; the ISP erases all "
              "of it either way, so\n"
              "        those bytes are erased before and after the install."
              % (ntrim, os.path.basename(path)))


def default_bootloader():
    """Find bootloader.bin without assuming anyone's directory layout: a
    release download is flat and has it next to this script, a source checkout
    has it in build/."""
    for cand in (os.path.join(HERE, "bootloader.bin"),
                 os.path.join(BLDIR, "build", "bootloader.bin"),
                 os.path.join(os.getcwd(), "bootloader.bin"),
                 os.path.join(os.getcwd(), "build", "bootloader.bin")):
        if os.path.isfile(cand):
            return cand
    return None


def app_valid(app):
    """src/boot.c bl_app_valid()'s gates, run offline, plus two STRICTER ones.

    If a bl_app_valid() gate fails, the installed bootloader will refuse to hand
    off and the device will sit in (empty) update mode after the very first
    power-on.

    THE TWO EXTRA GATES are labelled rather than folded in, because a stand-in
    that looks like the shipping predicate and quietly is not is the failure
    mode here:

      * the boot-info MARKER at 0x00.  bl_app_valid() carries this test behind
        `#if (BL_VALIDATOR_GATES & BL_GATE_MARKER)` and that bit is CLEAR
        (include/boot.h: "BL_GATE_MARKER is deliberately ABSENT"), so the
        shipping validator does NOT compile it -- the record CRC over
        0x00..0x0B already authenticates the field.
      * the 0xFF fill over the rest of the boot-info page.  Nothing on the
        device looks at bytes 0x0E..0x1FF at all.

    Both are stricter than the device, so either can only refuse an image the
    bootloader would in fact accept; neither can wave one through.  They are
    kept because this tool builds an image for a WHOLE-FLASH install over the
    ISP, where an unfamiliar marker or a dirty header page means the input is
    not the stock layout.  They are reported as "(stricter than bl_app_valid)"
    so a failure is not misread as "the bootloader will reject this".

    bl_app_valid()'s `(BL_APP_BASE + len) > BL_CODEFLASH_END` gate has no
    separate line here: with BL_APP_BASE = 0x4000 and BL_CODEFLASH_END =
    0x3E800 it is exactly the BL_APP_MAX_LEN bound checked below, which is why
    boot.c calls it "redundant, kept explicit on purpose".
    """
    print("\napplication acceptance -- the gates bl_app_valid() applies on-chip")
    if not check(len(app) > PAYLOAD_OFF, "image is larger than the 0x200 boot-info page",
                 "%d bytes" % len(app)):
        return
    h = app[:14]
    marker = struct.unpack("<H", h[0:2])[0]
    check(marker in (0xFFFF, 0x5555),
          "boot-info marker is 0xFFFF or 0x5555 (stricter than bl_app_valid: "
          "BL_GATE_MARKER is not compiled into the validator)",
          "0x%04X" % marker)
    check(h[2:6] == b"LFBG", "magic is 'LFBG'", repr(h[2:6]))
    hdr_crc = struct.unpack("<H", h[12:14])[0]
    check(crc16(h[0:12]) == hdr_crc, "header CRC over bytes 0x00..0x0B",
          "stored 0x%04X, computed 0x%04X" % (hdr_crc, crc16(h[0:12])))
    length = struct.unpack("<I", h[8:12])[0]
    check(BL_APP_MIN_LEN <= length <= BL_APP_MAX_LEN,
          "length is within [0x%X, 0x%X]" % (BL_APP_MIN_LEN, BL_APP_MAX_LEN),
          "0x%X" % length)
    check(length == len(app) - PAYLOAD_OFF,
          "length field matches the payload actually present",
          "field 0x%X, present 0x%X" % (length, len(app) - PAYLOAD_OFF))
    app_crc = struct.unpack("<H", h[6:8])[0]
    got = crc16(app[PAYLOAD_OFF:PAYLOAD_OFF + length])
    check(got == app_crc, "payload CRC over the whole application",
          "stored 0x%04X, computed 0x%04X" % (app_crc, got))
    sp = struct.unpack("<I", app[PAYLOAD_OFF:PAYLOAD_OFF + 4])[0]
    check((sp & 0x2FFE0000) == 0x20000000, "initial SP is SRAM-shaped",
          "0x%08X" % sp)
    pc = struct.unpack("<I", app[PAYLOAD_OFF + 4:PAYLOAD_OFF + 8])[0]
    check(pc & 1, "application reset vector has the Thumb bit set", "0x%08X" % pc)
    check(APP_LOAD <= (pc & ~1) < APP_LOAD + length,
          "application reset vector is inside [0x%04X, 0x%04X)"
          % (APP_LOAD, APP_LOAD + length), "0x%08X" % pc)
    check(app[14:PAYLOAD_OFF] == bytes([0xFF]) * (PAYLOAD_OFF - 14),
          "rest of the boot-info page is erased (0xFF) (stricter than "
          "bl_app_valid: nothing on the device reads 0x0E..0x1FF)")


def bl_valid(bl):
    print("\nbootloader acceptance")
    check(len(bl) <= APP_BASE, "fits the 0x3E00 budget",
          "%d bytes, %d free" % (len(bl), APP_BASE - len(bl)))
    check(len(bl) >= 0xC0, "long enough to hold a vector table", "%d bytes" % len(bl))
    sp = struct.unpack("<I", bl[0:4])[0]
    check((sp & 0x2FFE0000) == 0x20000000, "bootloader initial SP is SRAM-shaped",
          "0x%08X" % sp)
    vecs = struct.unpack("<36I", bl[0:0x90])
    check(all(v & 1 for v in vecs[1:]),
          "every vector 1..35 is non-zero and has the Thumb bit set")
    check(all(v < len(bl) for v in vecs[1:]),
          "every vector 1..35 points inside the bootloader image")
    check(vecs[1] & ~1 < len(bl), "reset vector -> 0x%08X" % vecs[1])


def main():
    ap = argparse.ArgumentParser(
        description="Build the composite CodeFlash image (bootloader + your "
                    "own application) that the ROM ISP writes from address 0.",
        epilog="typical release use:  build_composite.py --backup "
               "my-device-backup.bin --out install.bin")
    ap.add_argument("--backup",
                    help="your full CodeFlash dump from backup-codeflash.py. "
                         "Shorthand for --app BACKUP --baseline BACKUP: the "
                         "application half comes from it, and it is also the "
                         "install-safety baseline. This is the release flow.")
    ap.add_argument("--bootloader",
                    help="the bootloader image (default: bootloader.bin next "
                         "to this script, or build/bootloader.bin in a checkout)")
    ap.add_argument("--app",
                    help="source of the application region; if it is a full "
                         "CodeFlash image its 0x3E00.. slice is used, otherwise "
                         "the whole file is taken as the app image. Overrides "
                         "--backup as the application source.")
    ap.add_argument("--compare",
                    help="second copy of the app image; must match byte for byte")
    ap.add_argument("--baseline",
                    help="the full CodeFlash image currently on the device. "
                         "The composite must differ from it ONLY below 0x3E00 "
                         "— that is what makes this a bootloader install and "
                         "not a firmware change. Defaults to --backup.")
    ap.add_argument("--out")
    args = ap.parse_args()

    # --backup is shorthand, not a separate mode: it fills in whichever of
    # --app / --baseline was not named explicitly, so the two interfaces can be
    # mixed (--backup dump.bin --compare vendor-fw.bin is a sensible run).
    app_path = args.app or args.backup
    baseline_path = args.baseline or args.backup
    if not app_path:
        ap.error("no application source. Give --backup with your full device "
                 "dump (see backup-codeflash.py), or --app.")

    bl_path = args.bootloader or default_bootloader()
    if not bl_path:
        ap.error("bootloader.bin not found next to this script or in build/. "
                 "Name it with --bootloader.")

    def as_app_image(path, what):
        """Take an app image from either a bare fw.bin or a full CodeFlash dump.

        Applied to --compare as well as --app: without it, passing the device
        dump to --compare compares the whole 46 KB file against the 42 KB app
        region and fails with nothing but a mismatch."""
        raw = read_file(path, what)
        if len(raw) > APP_BASE and raw[APP_BASE + 2:APP_BASE + 6] == b"LFBG":
            raw, ntrim = trim_erased_tail(raw, APP_BASE, what, path)
            note_trim(ntrim, path)
            return raw[APP_BASE:], "%s [0x3E00..]" % os.path.basename(path)
        if raw[2:6] == b"LFBG":
            raw, ntrim = trim_erased_tail(raw, 0, what, path)
            note_trim(ntrim, path)
            return raw, os.path.basename(path)
        # Neither shape.  Say so here rather than letting it surface later as a
        # confusing "magic is 'LFBG'" failure on bytes that were never a header.
        die("%s does not look like a GBFlash image: %s\n"
            "       expected 'LFBG' at offset 0x02 (a bare fw.bin) or at "
            "0x3E02 (a full CodeFlash dump).\n"
            "       Found %r at 0x02%s.\n"
            "       A full dump from backup-codeflash.py is %d bytes or more."
            % (what, path, bytes(raw[2:6]),
               (" and %r at 0x3E02" % bytes(raw[APP_BASE + 2:APP_BASE + 6]))
               if len(raw) > APP_BASE + 6 else
               " (file is too short to have a 0x3E02)",
               APP_BASE + PAYLOAD_OFF + BL_APP_MIN_LEN))

    bl = read_file(bl_path, "bootloader image")
    app, src = as_app_image(app_path, "application source (--app/--backup)")

    print("bootloader: %s (%d bytes)" % (bl_path, len(bl)))
    print("application: %s (%d bytes)" % (src, len(app)))

    bl_valid(bl)
    app_valid(app)

    print("\ncross-checks")
    if args.compare and os.path.abspath(args.compare) == os.path.abspath(app_path):
        # THE SAME FILE IS NOT A SECOND COPY.  Pointed at the file --app
        # already named, this compares the application with itself: it cannot
        # fail, and a PASS would clear the SKIP list and upgrade the RESULT
        # line on the strength of a tautology.  The baseline check annotates
        # itself in the same situation; this one has nothing left to measure.
        skip("application region matches a second copy byte for byte",
             "--compare names the same file as the application source, so there "
             "is no second copy to\n         compare against. Point it at an "
             "independently obtained image (a vendor fw.bin) or omit it.")
    elif args.compare:
        other, other_src = as_app_image(args.compare, "comparison image (--compare)")
        check(app == other, "application region matches %s byte for byte"
              % other_src)
    else:
        skip("application region matches a second copy byte for byte",
             "no --compare given. Optional: it re-reads the application from an "
             "independent file (a vendor fw.bin) and proves the two agree.")
    img = bytearray(bl)
    img += bytes([FILL]) * (APP_BASE - len(bl))
    img += app
    img = bytes(img)

    check(len(img) == APP_BASE + len(app), "composite length is 0x3E00 + len(app)",
          "%d bytes (0x%X)" % (len(img), len(img)))
    check(img[:len(bl)] == bl, "bootloader region is the bootloader, unmodified")
    check(img[APP_BASE:] == app, "application region is the app, unmodified")
    check(img[:0xB8] != app[PAYLOAD_OFF:PAYLOAD_OFF + 0xB8],
          "0x0000 is NOT a copy of the application's vector table",
          "this is the gbflash_update.py behaviour that must not be used")
    check(struct.unpack("<I", img[4:8])[0] == struct.unpack("<I", bl[4:8])[0],
          "reset vector at 0x0004 is the bootloader's",
          "0x%08X (app's would be 0x%08X)"
          % (struct.unpack("<I", bl[4:8])[0],
             struct.unpack("<I", app[PAYLOAD_OFF + 4:PAYLOAD_OFF + 8])[0]))

    # THE INSTALL-SAFETY CHECK.  The composite is written to a device that is
    # running fine, so the property that matters most is that the flash it
    # replaces is confined to the bootloader region: every byte at or above
    # 0x3E00 must still be the byte the device already has.  If that holds the
    # install cannot change the firmware, cannot invalidate the boot-info
    # record, and rolls back with a single ISP write of the baseline.
    #
    # `--app` defaults to the same file, in which case this is a tautology and
    # is reported as such.  It stops being one the moment the application
    # region comes from anywhere else, which is when it needs to be true.
    if baseline_path:
        base = read_file(baseline_path, "baseline device image (--baseline/--backup)")
        if not (len(base) > APP_BASE and
                base[APP_BASE + 2:APP_BASE + 6] == b"LFBG"):
            die("baseline device image is not a full CodeFlash dump: %s\n"
                "       expected 'LFBG' at 0x3E02. The baseline must be the "
                "WHOLE flash as it is on the device now\n"
                "       (backup-codeflash.py), not a bare fw.bin -- otherwise "
                "there is nothing to compare the\n"
                "       bootloader region against." % baseline_path)
        # Both sides must be trimmed identically, or the length comparison
        # below turns the more thorough --all backup into a spurious failure.
        base, ntrim = trim_erased_tail(
            base, APP_BASE, "baseline device image (--baseline/--backup)",
            baseline_path)
        note_trim(ntrim, baseline_path)
        same_source = os.path.abspath(baseline_path) == os.path.abspath(app_path)
        n = min(len(img), len(base))
        diffs = [i for i in range(n) if img[i] != base[i]]
        above = [i for i in diffs if i >= APP_BASE]
        check(len(img) == len(base),
              "composite is the same length as the device image",
              "%d vs %d bytes" % (len(img), len(base)))
        check(not above,
              "composite differs from %s ONLY below 0x3E00%s"
              % (os.path.basename(baseline_path),
                 " (note: same file as --app, so this is by construction)"
                 if same_source else ""),
              "%d bytes differ, all in [0x0000, 0x%04X); first 0x%04X, "
              "last 0x%04X" % (len(diffs), APP_BASE, diffs[0], diffs[-1])
              if diffs and not above else
              ("identical" if not diffs else
               "%d byte(s) differ AT OR ABOVE 0x3E00, first at 0x%04X — this "
               "would change the installed firmware" % (len(above), above[0])))
        check(crc16(img[APP_BASE + PAYLOAD_OFF:]) ==
              crc16(base[APP_BASE + PAYLOAD_OFF:]),
              "application payload CRC is unchanged from the device image",
              "0x%04X" % crc16(img[APP_BASE + PAYLOAD_OFF:]))
    else:
        # THE ONE THAT MATTERS MOST IS THE ONE MOST EASILY LEFT OUT.  Without
        # a baseline nothing has compared the composite against the flash it is
        # about to replace.  The image may still be perfectly good, but that is
        # a weaker claim and must not read as the same PASS.
        for lbl in ("composite is the same length as the device image",
                    "composite differs from the device image ONLY below 0x3E00",
                    "application payload CRC is unchanged from the device image"):
            skip(lbl, "no --baseline/--backup given")
        print("\n  ** Nothing has checked this image against the flash now on "
              "your device. **\n"
              "     Re-run with --backup pointing at your full CodeFlash dump "
              "to prove the\n"
              "     install cannot touch anything at or above 0x3E00.")

    print("\nlayout")
    print("  0x0000-0x%04X  bootloader   %s" % (len(bl) - 1, hexbytes(bl[:8])))
    print("  0x%04X-0x3DFF  0x%02X fill    (%d bytes)"
          % (len(bl), FILL, APP_BASE - len(bl)))
    print("  0x3E00-0x%04X  application  %s" % (len(img) - 1, hexbytes(app[:8])))

    if FAILS:
        result = "FAIL -- %d check(s) failed" % len(FAILS)
    elif SKIPS:
        result = ("PASS with %d check(s) SKIPPED -- %d ran, and the skipped "
                  "ones are named above" % (len(SKIPS), len(RAN)))
    else:
        result = "PASS -- composite is consistent"
    print("\nRESULT: %s" % result)
    if FAILS:
        return 2
    if args.out:
        try:
            with open(args.out, "wb") as f:
                f.write(img)
        except OSError as e:
            die("cannot write %s: %s" % (args.out, e))
        import hashlib
        print("\nwrote %s (%d bytes)\n  sha256 %s"
              % (args.out, len(img), hashlib.sha256(img).hexdigest()))
        print("\nThis is the image the ROM ISP writes, in full, from address 0:"
              "\n    wchisp flash %s\n"
              "Short H1 and plug the board in while shorted first. Keep your "
              "backup." % args.out)
    else:
        print("\n(no --out given; nothing written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
