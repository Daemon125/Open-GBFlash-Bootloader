#!/usr/bin/env python3
"""Installs the GBFlash bootloader, so that firmware updates work.

    python3 install.py                  install it -- this is the one you want
    python3 install.py --check          just tell me whether I need it
    python3 install.py --backup FILE    just save a copy of my flash
    python3 install.py --restore FILE   put a saved copy back
    python3 install.py --dry-run        practise on a pretend device

It backs your device up before it changes anything, and asks before it writes.
If the install ever goes wrong, --restore puts your device back.

Needs Python 3 and pyserial (python3 -m pip install pyserial), plus wchisp, which
talks to the chip over the ROM ISP.  It offers to download wchisp if it is not here.
--restore needs neither pyserial nor a working device.
"""

import argparse
import glob
import hashlib
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

# --------------------------------------------------------------------------
# Constants.  These mirror include/boot.h, include/proto.h and the two docs/
# scripts; host/check_headers.c is what keeps the on-chip copies in agreement.
# --------------------------------------------------------------------------

VID_APP, PID_APP = 0x1A86, 0x7523      # CH340, the running application
VID_ISP, PID_ISP = 0x4348, 0x55E0      # CH579 ROM ISP
BAUD = 2000000

QUERY_FW_INFO = 0xA1
GET_VARIABLE = 0xAD
BASE32 = 0x200000D4                    # the u32 base GET_VARIABLE resolves against
MIN_FW_VER = 10                        # GET_VARIABLE-as-a-read appears here

VECTORS_LO = 0x00000000
REGION_LO = 0x000000B8                 # first byte a bootloader owns outright
BOOTINFO_BASE = 0x00003E00             # boot-info record; fw.bin byte 0 lands here
APP_BASE = 0x00004000                  # application vectors
PAYLOAD_OFF = 0x200                    # app payload offset within the app image
CODEFLASH_END = 0x0003E800             # 250 KB; DataFlash begins here
SECTOR = 0x200
FILL = 0xFF

BL_APP_MIN_LEN = 0x90
BL_APP_MAX_LEN = 0x3A800

HDR_TAG = b"LFBG"

# The shipping bootloader.  STAMPED BY `make dist`, not maintained by hand:
# the release carries the digest of the bootloader.bin staged beside it, so the
# pair is self-consistent whichever compiler CI happened to use.  None means
# "built here, nothing to compare against" -- which is the honest answer in a
# source checkout, and stops a from-source build being called a forgery.
#
# It was a hand-written constant once.  That made the released install.py
# denounce the released bootloader.bin, because the machine that wrote the
# constant was never the machine that built the binary.
BL_SHA256 = None

# Exit codes.  Distinct on purpose: a refusal is not a failure, and neither is
# "no device".
EXIT_OK = 0
EXIT_USAGE = 2
EXIT_REFUSED = 3        # a precondition was not met; NOTHING was written
EXIT_FAILED = 4         # something went wrong; read the recovery block
EXIT_NODEV = 5          # no device found

HERE = os.path.dirname(os.path.abspath(__file__))

_CRC_TABLE = [
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400,
]


def crc16(data):
    """CRC-16/MODBUS, the one the boot-info record and the protocol both use."""
    crc = 0xFFFF
    for b in data:
        crc = _CRC_TABLE[(b ^ crc) & 0x0F] ^ (crc >> 4)
        crc = _CRC_TABLE[((b >> 4) ^ crc) & 0x0F] ^ (crc >> 4)
    return crc


# --------------------------------------------------------------------------
# Output.  One line per thing that happened, each prefixed with a marker:
#
#     [ ok ]  it worked          [ !! ]  read this one
#     [ xx ]  it stopped         [ .. ]  in progress; rewrites itself in place
#
# The markers are ASCII on every platform: a tick and a cross put a
# UnicodeEncodeError one exotic console away from ending an install mid-write.
#
# Colour goes on the MARKER, never the line, and is off whenever the output is
# not a terminal, when NO_COLOR is set, or with --no-color -- so a log pasted
# into a bug report stays readable.
# --------------------------------------------------------------------------

RESET = "\033[0m"
STYLES = {"green": "\033[32m", "red": "\033[31m", "yellow": "\033[33m",
          "bold": "\033[1m", "dim": "\033[2m"}

MARKERS = {"ok": ("ok", "green"), "warn": ("!!", "yellow"),
           "fail": ("xx", "red"), "busy": ("..", "dim")}
MARK_W = 6                     # len("[ ok ]") -- every marker is this wide
WIDTH = 78

_ANSI_RE = re.compile(r"\033\[[0-9;]*m")


def strip_ansi(text):
    return _ANSI_RE.sub("", str(text))


def _enable_windows_ansi():
    """Modern Windows consoles do ANSI once the mode bit is set."""
    try:
        import ctypes
        k = ctypes.windll.kernel32
        k.SetConsoleMode(k.GetStdHandle(-11), 7)
    except Exception:
        pass


def want_colour(stream, disable=False):
    if disable or os.environ.get("NO_COLOR") is not None:
        return False
    if os.environ.get("TERM") == "dumb":
        return False
    try:
        if not stream.isatty():
            return False
    except Exception:
        return False
    if platform.system() == "Windows":
        _enable_windows_ansi()
    return True


def _wrap(text, width):
    # A line that already fits comes back untouched, spacing and all: status
    # lines separate their columns with two spaces, and re-joining on single
    # spaces would quietly undo that.
    text = str(text)
    if len(text) <= width and "\n" not in text:
        return [text]
    out, line = [], ""
    for word in text.split():
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = (line + " " + word) if line else word
    if line:
        out.append(line)
    return out or [""]


class Out(object):
    """Everything this script prints.  Collected so tests can read it back.

    Every outcome goes through mark(), so the output reads as one column of
    markers with a few words beside each.  Prose survives in exactly two
    places, the confirmation and the jumper steps, and both go through say().

    The collected copy has its colour stripped, so a test asserts on words
    rather than on escape sequences.
    """

    def __init__(self, stream=None, colour=None):
        self.stream = stream if stream is not None else sys.stdout
        self.lines = []
        # The in-place progress line uses \r, which is only meaningful on a
        # terminal.  Redirected to a file or a test buffer it would leave one
        # unreadable 4 KB line, so it is simply not emitted there.
        try:
            self.tty = bool(self.stream.isatty())
        except Exception:
            self.tty = False
        self.colour = want_colour(self.stream) if colour is None else bool(colour)
        self._busy = False

    def no_colour(self):
        """--no-color.  One way only: nothing can turn colour back on."""
        self.colour = False

    # ---- painting --------------------------------------------------------
    def paint(self, text, *styles):
        if not self.colour or not styles:
            return text
        return "".join(STYLES[s] for s in styles if s in STYLES) + text + RESET

    def b(self, text):
        return self.paint(text, "bold")

    def d(self, text):
        return self.paint(text, "dim")

    # ---- lines -----------------------------------------------------------
    def raw(self, text=""):
        self._wipe()
        for line in str(text).split("\n"):
            self.lines.append(strip_ansi(line))
        self.stream.write(str(text) + "\n")
        try:
            self.stream.flush()
        except Exception:
            pass

    def say(self, text="", indent=0):
        """A plain line at the left margin.  Wrapped: this is where the two
        surviving pieces of prose live."""
        if not str(text).strip():
            self.raw("")
            return
        for line in _wrap(text, WIDTH - indent):
            self.raw(" " * indent + line)

    def dim(self, text):
        for line in _wrap(text, WIDTH):
            self.raw(self.d(line))

    def gap(self):
        """One blank line, and never two -- some blocks end with one already."""
        if self.lines and self.lines[-1].strip():
            self.raw("")

    # ---- the status line -------------------------------------------------
    def mark(self, kind, text, detail=()):
        """`[ ok ] what happened`, plus any detail indented underneath it.

        Detail is for the failure case, where the failing check's own words are
        the diagnosis.  On the happy path nothing passes any.
        """
        glyph, style = MARKERS[kind]
        pad = " " * (MARK_W + 1)
        wrapped = _wrap(text, WIDTH - MARK_W - 1)
        self.raw(self.paint("[ %s ]" % glyph, style) + " " + wrapped[0])
        for line in wrapped[1:]:
            self.raw(pad + line)
        self.detail(detail)
        self._said = text
        self._said_end = len(self.lines)

    def detail(self, lines):
        """Dimmed lines under the marker they belong to."""
        for d in lines:
            for line in _wrap(d, WIDTH - MARK_W - 1):
                self.raw(" " * (MARK_W + 1) + self.d(line))

    def restating(self, text):
        """True if `text` is what the last line printed already said, so
        abort() can write underneath it instead of repeating it."""
        return (getattr(self, "_said", None) == text
                and getattr(self, "_said_end", -1) == len(self.lines))

    def ok(self, text, detail=()):
        self.mark("ok", text, detail)

    def warn(self, text, detail=()):
        self.mark("warn", text, detail)

    def fail(self, text, detail=()):
        self.mark("fail", text, detail)

    def busy(self, text):
        """The one line a slow operation is allowed, rewritten in place.  It is
        replaced by a single ok line when the operation finishes."""
        if not self.tty:
            return
        line = ("[ %s ] %s" % (MARKERS["busy"][0], text))[:WIDTH]
        self.stream.write("\r" + self.d(line.ljust(WIDTH)))
        try:
            self.stream.flush()
        except Exception:
            pass
        self._busy = True

    def _wipe(self):
        if not self._busy:
            return
        self._busy = False
        self.stream.write("\r" + " " * WIDTH + "\r")
        try:
            self.stream.flush()
        except Exception:
            pass

    def step(self, n, text):
        """A numbered instruction, for the jumper block."""
        for i, line in enumerate(_wrap(text, WIDTH - 5)):
            self.raw(("  %d. " % n if i == 0 else "     ") + line)

    def done(self, text):
        """The one line the whole run exists to print."""
        self.raw(self.b(text))

    def text(self):
        return "\n".join(self.lines)

    def contains(self, needle):
        """Whitespace-insensitive search: everything here is wrapped to fit a
        terminal, so a raw-text search would assert where the wrap fell."""
        flat = " ".join(self.text().split())
        return " ".join(str(needle).split()) in flat


class Check(object):
    __slots__ = ("ok", "label", "detail")

    def __init__(self, ok, label, detail=""):
        self.ok = bool(ok)
        self.label = label
        self.detail = detail


SHOW_CHECKS = False     # --show-checks: print passing gates as well as failing
VERBOSE = False         # --verbose: show wchisp's own output even on success


def report(out, name, checks, pass_line=None):
    """Run a block of checks and print the outcome.  Returns the failures.

    ONE HEADLINE PER FAILURE.  A block that fails is always followed by an
    abort saying the same thing in plainer words, so this prints NOTHING on
    failure -- the abort is the headline, and `failed_of()` gives it the count.

    Every check runs either way; this only decides what is printed.  A block
    where nothing failed is one ok line -- `pass_line`, or nothing at all where
    another line already says the same thing.  --show-checks prints every
    check, hex detail included, for a bug report; that is not the default path,
    where they are hex nobody can act on.
    """
    bad = [c for c in checks if not c.ok]
    if SHOW_CHECKS:
        out.dim("%s: %d checked, %d failed" % (name, len(checks), len(bad)))
        for c in checks:
            out.mark("ok" if c.ok else "fail",
                     c.label + (("  %s" % c.detail) if c.detail else ""))
    elif not bad and pass_line:
        out.ok(pass_line)
    return bad


def failed_of(bad, *blocks):
    """`3 of 21 checks failed` -- the whole of what a failed block says by
    default.  Which checks, and their hex, live behind --show-checks."""
    total = sum(len(b) for b in blocks)
    return "%d of %d checks failed" % (len(bad), total)


def show_checks_hint():
    """Where to see which checks failed -- unless the reader already asked,
    in which case they are on the screen and this would be noise."""
    if SHOW_CHECKS:
        return []
    return ["--show-checks names them: python3 %s --show-checks"
            % pathshow(os.path.abspath(__file__))]


def restore_bullet(me, backup_path, verified, lead="Put it back the way it was"):
    """The way back, worded for the backup the caller actually holds.  An
    unverified backup makes --restore refuse, so name the flag that works
    rather than hand out a command that will not run."""
    line = ("%s:\n  python3 %s --restore %s"
            % (lead, me, pathshow(backup_path)))
    if not verified:
        line += ("\n  That backup did not verify, so --restore will refuse it: "
                 "add --restore-unverified to force it.")
    return line


def pathshow(p):
    """A path fit to print: relative to the cwd when that is not absurd."""
    if p is None:
        return "(not found)"
    try:
        rel = os.path.relpath(p)
    except ValueError:
        return os.path.basename(p)
    if rel.startswith("..") or len(rel) > len(p):
        return os.path.basename(p)
    return rel


def commas(n):
    return "{:,}".format(n)


def human_size(n):
    try:
        n = int(n)
    except (TypeError, ValueError):
        return "size unknown"
    if n >= 1024 * 1024:
        return "%.1f MB" % (n / (1024.0 * 1024.0))
    if n >= 1024:
        return "%.0f KB" % (n / 1024.0)
    return "%d bytes" % n


def plain_time(seconds):
    """A duration a person would say out loud."""
    seconds = int(round(seconds))
    if seconds < 60:
        return "%d s" % seconds
    return "%d min %02d s" % (seconds // 60, seconds % 60)


# --------------------------------------------------------------------------
# Image knowledge.  Everything here is offline: it takes bytes and decides
# whether they are acceptable, using the same gates the device applies.
# --------------------------------------------------------------------------

def bootinfo(app):
    """Parse a boot-info record from the front of an application image."""
    if len(app) < 14:
        return None
    marker = struct.unpack("<H", app[0:2])[0]
    return {
        "marker": marker,
        "tag": bytes(app[2:6]),
        "appcrc": struct.unpack("<H", app[6:8])[0],
        "applen": struct.unpack("<I", app[8:12])[0],
        "hdrcrc": struct.unpack("<H", app[12:14])[0],
        "hdrcrc_ok": crc16(app[0:12]) == struct.unpack("<H", app[12:14])[0],
    }


def bootloader_gates(bl):
    """What a bootloader image must look like before it goes anywhere near flash.

    Same set as tools/build_composite.py bl_valid().
    """
    ck = []
    ck.append(Check(len(bl) <= BOOTINFO_BASE, "fits the 0x3E00 budget",
                    "%s bytes, %s free" % (commas(len(bl)),
                                           commas(BOOTINFO_BASE - len(bl)))))
    if len(bl) < 0x90:
        ck.append(Check(False, "long enough to hold a vector table",
                        "%d bytes" % len(bl)))
        return ck
    ck.append(Check(len(bl) >= 0xC0, "long enough to hold a vector table",
                    "%d bytes" % len(bl)))
    sp = struct.unpack("<I", bl[0:4])[0]
    ck.append(Check((sp & 0x2FFE0000) == 0x20000000,
                    "bootloader initial SP is SRAM-shaped", "0x%08X" % sp))
    vecs = struct.unpack("<36I", bl[0:0x90])
    ck.append(Check(all(v & 1 for v in vecs[1:]),
                    "every vector 1..35 is non-zero with the Thumb bit set"))
    ck.append(Check(all(v < len(bl) for v in vecs[1:]),
                    "every vector 1..35 points inside the bootloader image"))
    ck.append(Check((vecs[1] & ~1) < len(bl),
                    "reset vector points into the bootloader",
                    "0x%08X" % vecs[1]))
    return ck


def app_gates(app):
    """bl_app_valid()'s gates, run offline, plus the two stricter ones.

    `app` starts at the boot-info record -- i.e. what lands at flash 0x3E00.
    The two extra gates (the 0xFFFF/0x5555 marker and the erased remainder of
    the boot-info page) are STRICTER than the device: they can only refuse an
    image the bootloader would accept, never wave one through.  They are here
    because a whole-flash ISP write is what carries this image, and an
    unfamiliar marker means the input is not the stock layout.
    """
    ck = []
    if len(app) <= PAYLOAD_OFF:
        ck.append(Check(False, "image is larger than the 0x200 boot-info page",
                        "%d bytes" % len(app)))
        return ck
    ck.append(Check(True, "image is larger than the 0x200 boot-info page",
                    "%s bytes" % commas(len(app))))
    h = bootinfo(app)
    ck.append(Check(h["marker"] in (0xFFFF, 0x5555),
                    "boot-info marker is 0xFFFF or 0x5555 "
                    "(stricter than bl_app_valid)", "0x%04X" % h["marker"]))
    ck.append(Check(h["tag"] == HDR_TAG, "magic at 0x02 is 'LFBG'",
                    repr(h["tag"])))
    ck.append(Check(h["hdrcrc_ok"], "header CRC over bytes 0x00..0x0B",
                    "stored 0x%04X, computed 0x%04X"
                    % (h["hdrcrc"], crc16(app[0:12]))))
    length = h["applen"]
    ck.append(Check(BL_APP_MIN_LEN <= length <= BL_APP_MAX_LEN,
                    "length is within [0x%X, 0x%X]"
                    % (BL_APP_MIN_LEN, BL_APP_MAX_LEN), "0x%X" % length))
    ck.append(Check(length == len(app) - PAYLOAD_OFF,
                    "length field matches the payload actually present",
                    "field 0x%X, present 0x%X" % (length, len(app) - PAYLOAD_OFF)))
    got = crc16(app[PAYLOAD_OFF:PAYLOAD_OFF + length])
    ck.append(Check(got == h["appcrc"], "payload CRC over the whole application",
                    "stored 0x%04X, computed 0x%04X" % (h["appcrc"], got)))
    if len(app) >= PAYLOAD_OFF + 8:
        sp = struct.unpack("<I", app[PAYLOAD_OFF:PAYLOAD_OFF + 4])[0]
        pc = struct.unpack("<I", app[PAYLOAD_OFF + 4:PAYLOAD_OFF + 8])[0]
        ck.append(Check((sp & 0x2FFE0000) == 0x20000000,
                        "application initial SP is SRAM-shaped", "0x%08X" % sp))
        ck.append(Check(pc & 1, "application reset vector has the Thumb bit set",
                        "0x%08X" % pc))
        ck.append(Check(APP_BASE <= (pc & ~1) < APP_BASE + length,
                        "application reset vector is inside [0x%04X, 0x%X)"
                        % (APP_BASE, APP_BASE + length), "0x%08X" % pc))
    ck.append(Check(app[14:PAYLOAD_OFF] == bytes([FILL]) * (PAYLOAD_OFF - 14),
                    "rest of the boot-info page is erased (0xFF) "
                    "(stricter than bl_app_valid)"))
    return ck


LOW_SHAPE_TEXT = {
    "bootloader": "code from 0x0000 then erased fill -- a bootloader is "
                  "installed, and reset enters it",
    "stock": "a vector table at 0x0000..0x00B7 and ZEROS to 0x3DFF -- the "
             "factory layout, where reset enters the application directly",
    "erased": "all 0xFF. Reset would fetch 0xFFFFFFFF for both SP and PC, so "
              "this image does not come up at all",
    "unrecognised": "neither a bootloader, nor the factory layout, nor "
                    "erased -- what a dump corrupted on the wire looks like. "
                    "Writing it would very likely produce a dark board",
}


def classify_low(image):
    """What shape is flash 0x0000..0x3DFF, and could a part boot from it?

    Three shapes exist on a real device and everything else is refused:

      "bootloader"  code from 0x0000, then erased (0xFF) fill up to 0x3E00.
      "stock"       the application's vector table at 0x0000..0x00B7 and ZEROS
                    to 0x3DFF -- the factory bootloader-less layout this
                    project exists for; reset enters the application directly.
      "erased"      the whole region is 0xFF.  Nothing boots from this.

    `prefix` is the length of the programmed part of a bootloader shape, i.e.
    the image with its erased tail trimmed off.
    """
    low = bytes(image[:BOOTINFO_BASE])
    res = {"shape": "unrecognised", "prefix": 0, "gap": 0}
    if len(low) < BOOTINFO_BASE:
        return res
    if all(b == FILL for b in low):
        res["shape"] = "erased"
        return res
    if all(b == 0x00 for b in low[REGION_LO:]):
        res["shape"] = "stock"
        return res

    # Everything else has to earn the name "bootloader" rather than being
    # given it by elimination.  Without the SP/reset test a region of uniform
    # garbage reads as "a bootloader is installed"; without the fill-run test a
    # few dirty bytes near 0x3DFF pull the trim point up to meet them and ~8 KB
    # of erased flash in the middle counts as bootloader.  The shipping build's
    # longest internal run of 0xFF is 3 bytes and the threshold is a whole
    # 512-byte sector, so this cannot fire on real code.
    n = BOOTINFO_BASE
    while n > 0 and low[n - 1] == FILL:
        n -= 1
    if not (REGION_LO < n <= BOOTINFO_BASE):
        return res
    sp = struct.unpack("<I", low[0:4])[0]
    reset = struct.unpack("<I", low[4:8])[0]
    if (sp & 0x2FFE0000) != 0x20000000:
        return res
    if not (reset & 1) or (reset & ~1) >= n:
        return res
    run = best = 0
    for b in low[:n]:
        run = run + 1 if b == FILL else 0
        if run > best:
            best = run
    if best >= SECTOR:
        res["gap"] = best
        return res
    res["shape"] = "bootloader"
    res["prefix"] = n
    res["gap"] = best
    return res


def low_gates(image, app):
    """Gates over flash 0x0000..0x3DFF.

    These 15,872 bytes decide whether the part comes up at all -- the CH579
    fetches SP from 0x0000 and the reset vector from 0x0004 before any code
    runs -- and nothing else covers them: the boot-info CRC spans the
    application only.  Without these gates a word garbled during the dump goes
    unnoticed and the file meant to be the way back kills the device.

    What they cannot do: a bootloader carries no checksum over itself, so a
    flipped byte inside bootloader CODE is not detectable from the image alone.
    The published build is caught by the sha256; any other build is not.
    """
    ck = []
    low = bytes(image[:BOOTINFO_BASE])
    if len(low) < BOOTINFO_BASE:
        ck.append(Check(False, "image reaches 0x3E00",
                        "%s bytes" % commas(len(low))))
        return ck
    cls = classify_low(low)
    shape = cls["shape"]
    ck.append(Check(shape in ("bootloader", "stock"),
                    "flash 0x0000..0x3DFF has a shape a device can boot from",
                    LOW_SHAPE_TEXT[shape]))
    if shape not in ("bootloader", "stock"):
        return ck

    sp = struct.unpack("<I", low[0:4])[0]
    reset = struct.unpack("<I", low[4:8])[0]
    ck.append(Check((sp & 0x2FFE0000) == 0x20000000,
                    "initial SP at 0x0000 is SRAM-shaped", "0x%08X" % sp))

    if shape == "bootloader":
        n = cls["prefix"]
        digest = hashlib.sha256(low[:n]).hexdigest()
        if BL_SHA256 is None:
            verdict = ""
        elif digest == BL_SHA256:
            verdict = " (the published build)"
        else:
            verdict = (" (NOT the published build -- a flipped byte inside "
                       "code this script has never seen cannot be detected)")
        ck.append(Check(all(b == FILL for b in low[n:]),
                        "0x%04X..0x3DFF is erased (0xFF) fill" % n,
                        "%s bytes; the bootloader itself is %s bytes, sha256 "
                        "%s%s" % (commas(BOOTINFO_BASE - n), commas(n), digest,
                                  verdict)))
        ck.extend(bootloader_gates(low[:n]))
        ck.append(Check((reset & 1) and (reset & ~1) < n,
                        "reset vector at 0x0004 enters the bootloader",
                        "0x%08X -- %s" % (reset, describe_reset(reset))))
        return ck

    h = bootinfo(app)
    applen = h["applen"] if (h and h["tag"] == HDR_TAG) else 0
    top = APP_BASE + applen
    vecs = struct.unpack("<36I", low[:0x90])
    bad = [i for i, v in enumerate(vecs[1:], 1)
           if v != 0 and not ((v & 1) and APP_BASE <= (v & ~1) < top)]
    ck.append(Check(not bad,
                    "every vector in 0x0004..0x008F is either zero or points "
                    "into the application with the Thumb bit set",
                    "all %d accounted for" % (len(vecs) - 1) if not bad
                    else "%d do not: %s" % (len(bad), ", ".join(
                        "index %d = 0x%08X" % (i, vecs[i]) for i in bad[:6]))))
    ck.append(Check((reset & 1) and APP_BASE <= (reset & ~1) < top,
                    "reset vector at 0x0004 enters the application, inside "
                    "[0x%04X, 0x%X)" % (APP_BASE, top),
                    "0x%08X -- %s" % (reset, describe_reset(reset))))
    return ck


def backup_gates(image):
    """Is this file a CodeFlash image you could actually restore a device from?

    This is the gate that decides whether the install may proceed, and the gate
    --restore obeys.  It is deliberately more than "the read finished": a
    truncated or garbled dump is exactly the thing that looks like a backup and
    is not one.  It covers BOTH halves of the image -- the application from
    0x3E00 up, and the boot region below it, which is the half a device
    actually starts executing.
    """
    ck = []
    minimum = BOOTINFO_BASE + PAYLOAD_OFF + BL_APP_MIN_LEN
    ck.append(Check(len(image) >= minimum,
                    "long enough to be a full CodeFlash dump",
                    "%s bytes; a dump is at least %s"
                    % (commas(len(image)), commas(minimum))))
    if len(image) < minimum:
        return ck
    ck.append(Check(len(image) % 4 == 0,
                    "length is a whole number of 32-bit flash words",
                    "%s bytes" % commas(len(image))))
    ck.append(Check(len(image) <= CODEFLASH_END,
                    "length does not exceed the 250 KB CodeFlash array",
                    "%s bytes" % commas(len(image))))
    ck.append(Check(image[BOOTINFO_BASE + 2:BOOTINFO_BASE + 6] == HDR_TAG,
                    "boot-info tag 'LFBG' is at 0x3E02",
                    repr(bytes(image[BOOTINFO_BASE + 2:BOOTINFO_BASE + 6]))))

    app, ntrim = trim_erased_tail(image[BOOTINFO_BASE:])
    if ntrim:
        ck.append(Check(True, "erased tail past the end of the application",
                        "%s trailing 0xFF byte(s) ignored (an --all dump reads "
                        "the whole array)" % commas(ntrim)))
    ck.extend(app_gates(app))
    ck.extend(low_gates(image, app))
    return ck


def describe_reset(reset):
    """What the word at 0x0004 means.  Never guess in this function's favour:
    a value that cannot boot must be described as a value that cannot boot."""
    if reset in (0x00000000, 0xFFFFFFFF):
        return ("not a usable vector -- the part faults at reset and the "
                "board stays dark")
    if not reset & 1:
        return ("no Thumb bit -- the part faults at reset and the board stays "
                "dark")
    t = reset & ~1
    if t < BOOTINFO_BASE:
        return "a bootloader is installed"
    if t >= APP_BASE:
        return "the application's own vectors, i.e. no bootloader"
    return "somewhere unexpected"


def trim_erased_tail(app):
    """Drop erased flash past the end of the application image.

    A `backup-codeflash.py --all` dump is the application followed by ~200 KB of
    0xFF.  That is the MORE thorough of the two backups, so it must not be the
    one that gets rejected.  The tail is dropped only when it is genuinely
    erased; anything else in it is real content and the caller is told.
    """
    h = bootinfo(app)
    if h is None or h["tag"] != HDR_TAG:
        return app, 0
    n = h["applen"]
    if not (BL_APP_MIN_LEN <= n <= BL_APP_MAX_LEN):
        return app, 0
    end = PAYLOAD_OFF + n
    if len(app) <= end:
        return app, 0
    tail = app[end:]
    if any(b != FILL for b in tail):
        return app, 0        # not erased: leave it, app_gates() will object
    return app[:end], len(tail)


def build_composite(bl, app):
    """bootloader | 0xFF fill | application.  Nothing is invented or recomputed.

    In particular 0x0000 is NOT synthesised from the application's vector
    table.  General-purpose full-image builders do that -- a bootloader-less
    GBFlash has no other way to get vectors to address 0 -- and it is precisely
    wrong here: the application's reset vector would land at 0x0004 and the
    bootloader would never run.
    """
    if len(bl) > BOOTINFO_BASE:
        raise ValueError("bootloader is larger than the 0x3E00 region")
    return bytes(bl) + bytes([FILL]) * (BOOTINFO_BASE - len(bl)) + bytes(app)


def composite_gates(img, bl, app, baseline):
    """The image is about to be written to a device that works.  Prove it can't
    change anything at or above 0x3E00."""
    ck = []
    ck.append(Check(len(img) == BOOTINFO_BASE + len(app),
                    "composite length is 0x3E00 + len(app)",
                    "%s bytes (0x%X)" % (commas(len(img)), len(img))))
    ck.append(Check(img[:len(bl)] == bl,
                    "bootloader region is the bootloader, unmodified"))
    ck.append(Check(img[len(bl):BOOTINFO_BASE] ==
                    bytes([FILL]) * (BOOTINFO_BASE - len(bl)),
                    "the gap up to 0x3E00 is erased (0xFF) fill"))
    ck.append(Check(img[BOOTINFO_BASE:] == app,
                    "application region is the application, unmodified"))
    ck.append(Check(img[:0xB8] != app[PAYLOAD_OFF:PAYLOAD_OFF + 0xB8],
                    "0x0000 is NOT a copy of the application's vector table",
                    "this is the general-purpose-builder behaviour that must "
                    "not be used"))
    bl_reset = struct.unpack("<I", bl[4:8])[0]
    app_reset = struct.unpack("<I", app[PAYLOAD_OFF + 4:PAYLOAD_OFF + 8])[0]
    ck.append(Check(struct.unpack("<I", img[4:8])[0] == bl_reset,
                    "reset vector at 0x0004 is the bootloader's",
                    "0x%08X (the application's would be 0x%08X)"
                    % (bl_reset, app_reset)))

    if baseline is None:
        ck.append(Check(False, "composite differs from the device image ONLY "
                               "below 0x3E00",
                        "no baseline image was supplied, so nothing has "
                        "checked this against the flash on your device"))
        return ck

    base, _ = trim_erased_tail_full(baseline)
    ck.append(Check(len(img) == len(base),
                    "composite is the same length as the device image",
                    "%s vs %s bytes" % (commas(len(img)), commas(len(base)))))
    n = min(len(img), len(base))
    diffs = [i for i in range(n) if img[i] != base[i]]
    above = [i for i in diffs if i >= BOOTINFO_BASE]
    if diffs and not above:
        detail = ("%s bytes differ, all in [0x0000, 0x3E00); first 0x%04X, "
                  "last 0x%04X" % (commas(len(diffs)), diffs[0], diffs[-1]))
    elif not diffs:
        detail = "identical"
    else:
        detail = ("%s byte(s) differ AT OR ABOVE 0x3E00, first at 0x%04X -- "
                  "this would change your firmware" % (commas(len(above)),
                                                       above[0]))
    ck.append(Check(not above,
                    "composite differs from your device image ONLY below 0x3E00",
                    detail))
    ck.append(Check(crc16(img[BOOTINFO_BASE + PAYLOAD_OFF:]) ==
                    crc16(base[BOOTINFO_BASE + PAYLOAD_OFF:]),
                    "application payload CRC is unchanged from the device image",
                    "0x%04X" % crc16(img[BOOTINFO_BASE + PAYLOAD_OFF:])))
    return ck


def trim_erased_tail_full(image):
    """trim_erased_tail() applied to a whole CodeFlash image."""
    if len(image) <= BOOTINFO_BASE:
        return image, 0
    app, n = trim_erased_tail(image[BOOTINFO_BASE:])
    return image[:BOOTINFO_BASE] + app, n


def synth_fw(applen=0x7520):
    """A structurally valid fw.bin, for the simulator only.

    Byte-for-byte the same construction as host/make_synthetic_fw.py build();
    host/test_install.py asserts the two agree, so this copy cannot drift.
    """
    if applen < BL_APP_MIN_LEN or applen % 4:
        raise ValueError("applen must be >= 0x90 and a multiple of 4")
    reset = (APP_BASE + 0x90) | 1
    app = bytearray(struct.pack("<II", 0x20008000, reset))
    while len(app) < 0x90:
        app += struct.pack("<I", reset)
    i = 0
    while len(app) < applen:
        app += struct.pack("<I", 0xA5A50000 | (i & 0xFFFF))
        i += 1
    app = bytes(app[:applen])
    hdr = bytearray(b"\xFF" * PAYLOAD_OFF)
    hdr[0x00:0x02] = struct.pack("<H", 0xFFFF)
    hdr[0x02:0x06] = HDR_TAG
    hdr[0x06:0x08] = struct.pack("<H", crc16(app))
    hdr[0x08:0x0C] = struct.pack("<I", applen)
    hdr[0x0C:0x0E] = struct.pack("<H", crc16(bytes(hdr[0x00:0x0C])))
    return bytes(hdr) + app


# --------------------------------------------------------------------------
# The link to a running application.  Two implementations: a real serial port,
# and a simulated one backed by a bytes image.  Everything above the link is
# identical in both, which is what makes --dry-run worth having.
# --------------------------------------------------------------------------

class LinkError(Exception):
    pass


class MissingDependency(LinkError):
    """pyserial is not installed, so this host cannot open a serial port.

    A subclass rather than a flag: every other LinkError is a statement about
    the DEVICE, this one is about this COMPUTER.  Conflating the two once made
    --restore report a successful rescue as a failure and advise erasing the
    board again.
    """


LINK_SETTLE = 0.15   # after opening the port; gbflash_info.py waits the same


class SerialLink(object):
    """GET_VARIABLE reads against the running GBFlash application.

    Exactly two opcodes are ever sent, 0xA1 and 0xAD, and both are read-only.
    Nothing here writes, resets the device, or touches DataFlash.
    """

    def __init__(self, port):
        try:
            import serial
        except ImportError:
            raise MissingDependency(
                "this step needs pyserial\n"
                "(--restore does not need it to WRITE your image; only the "
                "read-back afterwards does.)")
        self.port = port
        try:
            self.dev = serial.Serial(port, BAUD, timeout=2)
        except Exception as e:
            raise LinkError("cannot open %s: %s\nIs FlashGBX still open? Only "
                            "one process can hold the port." % (port, e))
        time.sleep(LINK_SETTLE)

    def close(self):
        try:
            self.dev.close()
        except Exception:
            pass

    def _read_exact(self, n, what):
        buf = self.dev.read(n)
        if len(buf) != n:
            raise LinkError("short read on %s: wanted %d bytes, got %d (%s).\n"
                            "Is FlashGBX still open, or is this not a GBFlash?"
                            % (what, n, len(buf), buf.hex() or "nothing"))
        return buf

    def query_fw(self):
        """One QUERY_FW_INFO exchange.  Layout is eight bytes
        `>cHBI` -- cfw_id, fw_ver, pcb_ver, fw_ts -- then from
        fw_ver 12 a length-prefixed PCB name and two flag bytes, read so the
        port is left empty.  Same decode as gbflash_info.py and FlashGBX's
        hw_GBFlash.py."""
        self.dev.reset_input_buffer()
        self.dev.reset_output_buffer()
        self.dev.write(bytes([QUERY_FW_INFO]))
        self.dev.flush()
        size = self._read_exact(1, "length byte")[0]
        if size != 8:
            raise LinkError("expected a length byte of 8, got %d. This does "
                            "not look like GBFlash firmware." % size)
        info = self._read_exact(8, "info block")
        fw = {"cfw_id": chr(info[0]),
              "fw_ver": int.from_bytes(info[1:3], "big"),
              "pcb_ver": info[3],
              "fw_ts": int.from_bytes(info[4:8], "big"),
              "pcb_name": None}
        if fw["cfw_id"] == "L" and fw["fw_ver"] >= 12:
            n = self._read_exact(1, "name length")[0]
            fw["pcb_name"] = self._read_exact(n, "name").decode(
                "utf-8", "replace").strip("\x00").strip()
            self._read_exact(1, "capability byte")
            self._read_exact(1, "flags byte")
        return fw

    def read_word(self, addr):
        if addr & 3:
            raise ValueError("0x%08X is not 4-byte aligned" % addr)
        index = ((addr - BASE32) // 4) & 0xFFFFFFFF
        if index == 0xFF:
            raise ValueError("index 0xFF is reserved by the firmware")
        self.dev.reset_input_buffer()
        self.dev.write(bytes([GET_VARIABLE, 4]) + struct.pack(">I", index))
        self.dev.flush()
        value = int.from_bytes(self._read_exact(4, "GET_VARIABLE reply"), "big")
        return struct.pack("<I", value)


class SimLink(object):
    """A simulated running application, reading out of a bytes image."""

    def __init__(self, image, fw, port="/dev/sim-gbflash"):
        self.image = image
        self.fw = fw
        self.port = port
        self.closed = False

    def close(self):
        self.closed = True

    def query_fw(self):
        return dict(self.fw)

    def read_word(self, addr):
        if addr & 3:
            raise ValueError("0x%08X is not 4-byte aligned" % addr)
        if addr + 4 <= len(self.image):
            return bytes(self.image[addr:addr + 4])
        return b"\xFF\xFF\xFF\xFF"


BAR_WIDTH = 24


def _bar(pct):
    filled = int(BAR_WIDTH * pct / 100)
    return "#" * filled + "." * (BAR_WIDTH - filled)


def read_range(link, lo, hi, out, label, sink=None):
    """Read [lo, hi) a word at a time.  Optionally stream to a file as it goes,
    so an interrupted read leaves a short file rather than nothing at all.

    This takes minutes, so it gets the one line allowed to rewrite itself -- a
    frozen-looking installer is one people unplug halfway through.  It prints
    nothing when it finishes; the caller owns the ok line that replaces it.
    """
    buf = bytearray()
    total = hi - lo
    t0 = time.time()
    last = -1
    for addr in range(lo, hi, 4):
        word = link.read_word(addr)
        buf += word
        if sink is not None:
            sink.write(word)
        done = addr - lo
        if out.tty and total and done % 0x800 == 0:
            pct = 100 * done // total
            if pct != last:
                last = pct
                el = time.time() - t0
                rate = (done / el) if el > 0.001 else 0
                eta = ((total - done) / rate) if rate > 0 else 0
                out.busy("%s  %s %3d%%  %s left"
                         % (label, _bar(pct), pct, plain_time(eta)))
    if sink is not None:
        sink.flush()
    return bytes(buf)


# --------------------------------------------------------------------------
# Hardware: everything that touches the world outside this process.  The
# simulated implementation below is the whole reason the safety logic can be
# tested without a device.
# --------------------------------------------------------------------------

# wchisp is the tool: prebuilt binaries for every platform this script runs
# on, so nobody needs a compiler, and it is what the hardware was done with.
# A tuple because the lookup iterates it and the tests strip it to simulate a
# bare machine.
ISP_TOOLS = ("wchisp",)

WCHISP_HOME = "https://github.com/ch32-rs/wchisp"


def isp_argv(isp, image_path):
    """The one command this script ever runs.

    `wchisp flash FILE` erases, writes, verifies and resets in a single
    command, and does not touch the configuration registers at all -- those are
    a separate `config` subcommand, which this script never issues.  See
    isp_argv_guard, which makes sure of it.
    """
    return [isp, "flash", image_path]


def isp_argv_guard(argv):
    """THE USER CONFIGURATION WORD IS NEVER WRITTEN.

    CFG_BOOT_EN in that word is what makes shorting H1 enter the ROM ISP.  Lose
    it and there is no recovery path left, on any device, ever.  So every ISP
    command this script issues passes through here and a forbidden option
    raises.

    `config` is wchisp's subcommand for those registers.  The long/short forms
    are the same switches on other CH5xx ISP tools, listed so that --isp
    pointed at one of those still refuses.
    """
    forbidden = {"--user-config", "-u",
                 "--write-protection-config", "-w",
                 "config"}
    for a in argv[1:]:
        head = a.split("=", 1)[0]
        if head in forbidden:
            raise RuntimeError(
                "refusing to run an ISP command carrying %r.\n"
                "That option writes the CH579 user configuration word, and "
                "CFG_BOOT_EN in it\nis what makes H1 recovery work. This "
                "script never writes it. This is a bug\nin install.py if you "
                "did not ask for it." % a)
    return argv


# --------------------------------------------------------------------------
# Fetching wchisp.  Strictly opt-in: the prompt is one line, the answer has to
# be yes, and nothing is downloaded when a tool is already here.
# --------------------------------------------------------------------------

WCHISP_VERSION = "v0.3.0"
WCHISP_RELEASES = "https://github.com/ch32-rs/wchisp/releases"
WCHISP_BASE = "%s/download/%s/" % (WCHISP_RELEASES, WCHISP_VERSION)


def wchisp_asset():
    """The prebuilt wchisp for this machine, or None if there isn't one."""
    system = platform.system()
    machine = platform.machine().lower()
    arm = machine in ("arm64", "aarch64")
    intel = machine in ("x86_64", "amd64", "x64")
    if system == "Darwin" and (arm or intel):
        return "wchisp-%s-macos-%s.tar.gz" % (WCHISP_VERSION,
                                              "arm64" if arm else "x64")
    if system == "Linux" and (arm or intel):
        return "wchisp-%s-linux-%s.tar.gz" % (WCHISP_VERSION,
                                              "aarch64" if arm else "x64")
    if system == "Windows" and intel:
        return "wchisp-%s-win-x64.zip" % WCHISP_VERSION
    return None


def unpack_wchisp(data, is_zip, want):
    """Pull one named file out of the archive in memory.

    Nothing is extracted by the archive's own paths, so a hostile member name
    has nowhere to land.
    """
    import io
    if is_zip:
        import zipfile
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for name in z.namelist():
                if os.path.basename(name).lower() == want.lower():
                    return z.read(name)
        return None
    import tarfile
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
        for m in t.getmembers():
            if m.isfile() and os.path.basename(m.name).lower() == want.lower():
                f = t.extractfile(m)
                return f.read() if f is not None else None
    return None


def download_wchisp(hw, out):
    """Fetch wchisp and drop it next to this script.  Returns a path or None.

    Only ever reached after the user has answered yes.
    """
    asset = wchisp_asset()
    if asset is None:
        out.fail("no prebuilt wchisp for %s %s"
                 % (platform.system(), platform.machine()),
                 ["build it from source: %s" % WCHISP_HOME])
        return None

    url = WCHISP_BASE + asset
    # Downloading names what it is downloading. This is the only line in the
    # script that reports something about to happen rather than something that
    # has, and it earns that: it is the only time anything crosses the network.
    data, err = hw.fetch(
        url, lambda size: out.dim("fetching %s (%s)"
                                  % (url, human_size(size))))
    if data is None:
        out.fail("download failed -- %s" % err,
                 ["get it from %s, then --isp /path/to/wchisp"
                  % WCHISP_RELEASES])
        return None

    name = "wchisp.exe" if asset.endswith(".zip") else "wchisp"
    try:
        blob = unpack_wchisp(data, asset.endswith(".zip"), name)
    except Exception as e:
        out.fail("the download would not unpack -- %s" % e,
                 ["get it from %s instead" % WCHISP_RELEASES])
        return None
    if not blob:
        out.fail("the download held no wchisp binary",
                 ["get it from %s instead" % WCHISP_RELEASES])
        return None

    dest = os.path.join(HERE, name)
    try:
        with open(dest, "wb") as f:
            f.write(blob)
        os.chmod(dest, 0o755)
    except OSError as e:
        out.fail("could not save wchisp -- %s" % e,
                 ["run this from a directory you can write to, or get wchisp "
                  "from %s" % WCHISP_RELEASES])
        return None
    out.ok("wchisp %s downloaded  %s" % (WCHISP_VERSION, pathshow(dest)))
    return dest


def ensure_isp_tool(hw, out, tools):
    """No tool that can write to the chip?  Offer to fetch one.

    Returns True if there is one now.  Never downloads without a yes, and
    never downloads when a tool is already here.
    """
    if tools.isp:
        return True
    ans = hw.ask(out, "wchisp not found. Download it? (y/N)",
                 tag="download-wchisp", default="n")
    if ans.strip().lower() not in ("y", "yes"):
        return False
    path = download_wchisp(hw, out)
    if not path:
        return False
    tools.isp = path
    return True


def no_isp_tool(out, device_state):
    """The same stop, whichever mode asked for a tool it has not got."""
    return abort(out, "wchisp not found -- nothing here can write to the chip",
                 device_state,
                 ["Run this again and answer y to the download offer.",
                  "Or get it from %s and put it next to this script."
                  % WCHISP_RELEASES,
                  "Already have it? --isp /path/to/wchisp",
                  "On Linux it needs USB permission for %04X:%04X -- a udev "
                  "rule, or sudo." % (VID_ISP, PID_ISP)], EXIT_REFUSED)


class Hardware(object):
    """Interface only."""

    # Set by list_app_ports() when the port scan could not import pyserial.
    # detect() reads it so "nothing found" is not reported as an absent device
    # when it is really an absent dependency.
    no_pyserial = False

    def list_app_ports(self):
        raise NotImplementedError

    def open_app(self, port):
        raise NotImplementedError

    def isp_present(self):
        """True, False, or None for 'cannot tell on this platform'."""
        raise NotImplementedError

    def run(self, argv, cwd=None):
        raise NotImplementedError

    def run_isp(self, isp, image_path):
        raise NotImplementedError

    def fetch(self, url, on_start=None):
        """Download a URL.  Returns (bytes, None) or (None, why-it-failed)."""
        raise NotImplementedError

    def ask(self, out, prompt, tag=None, default="y"):
        raise NotImplementedError

    def wait(self, seconds):
        raise NotImplementedError

    def monotonic(self):
        raise NotImplementedError


class RealHardware(Hardware):

    def __init__(self, assume_yes=False):
        self.assume_yes = assume_yes
        self.isp_calls = []

    # ---- serial ----------------------------------------------------------
    def list_app_ports(self):
        ports = []
        try:
            import serial.tools.list_ports
            allp = list(serial.tools.list_ports.comports())
            ports = [p.device for p in allp
                     if p.vid == VID_APP and p.pid == PID_APP]
        except ImportError:
            # REMEMBERED, NOT SWALLOWED.  The glob below cannot see a Windows
            # COM port at all, so on Windows a missing pyserial produced an
            # empty list, which reads as "no device on USB at all" -- the
            # diagnosis abort_no_pyserial() exists to prevent, on the platform
            # that needs it most.  The caller checks this flag.
            self.no_pyserial = True
        except Exception:
            pass
        if not ports:
            ports = sorted(glob.glob("/dev/cu.usbserial*")
                           + glob.glob("/dev/cu.wchusbserial*")
                           + glob.glob("/dev/ttyUSB*"))
        return ports

    def open_app(self, port):
        return SerialLink(port)

    # ---- USB -------------------------------------------------------------
    def isp_present(self):
        """Is 4348:55E0 on the bus?

        Tried in order of cost.  None means 'this platform could not be asked',
        and the caller then falls back to asking the human, rather than
        declaring the device absent on the strength of a missing tool.
        """
        try:
            import usb.core
            if usb.core.find(idVendor=VID_ISP, idProduct=PID_ISP) is not None:
                return True
            return False
        except Exception:
            pass

        system = platform.system()
        if system == "Linux":
            found = False
            any_seen = False
            for vpath in glob.glob("/sys/bus/usb/devices/*/idVendor"):
                any_seen = True
                try:
                    with open(vpath) as f:
                        vid = f.read().strip()
                    with open(vpath.replace("idVendor", "idProduct")) as f:
                        pid = f.read().strip()
                except OSError:
                    continue
                if int(vid, 16) == VID_ISP and int(pid, 16) == PID_ISP:
                    found = True
            if any_seen:
                return found
            rc, text = self.run(["lsusb"])
            if rc == 0:
                return ("%04x:%04x" % (VID_ISP, PID_ISP)) in text.lower()
            return None

        if system == "Darwin":
            for argv in (["ioreg", "-p", "IOUSB", "-l", "-w0"],
                         ["ioreg", "-rlc", "IOUSBHostDevice", "-w0"]):
                rc, text = self.run(argv)
                if rc != 0 or not text.strip():
                    continue
                vids = set(int(m) for m in
                           re.findall(r'"idVendor"\s*=\s*(\d+)', text))
                pids = set(int(m) for m in
                           re.findall(r'"idProduct"\s*=\s*(\d+)', text))
                if not vids:
                    continue
                # Pairing is what matters, but ioreg lists them per node; look
                # for a node that carries both.
                for node in text.split("+-o "):
                    if ('"idVendor" = %d' % VID_ISP) in node and \
                       ('"idProduct" = %d' % PID_ISP) in node:
                        return True
                if VID_ISP in vids and PID_ISP in pids:
                    return True
                return False
            return None

        return None

    # ---- subprocesses ----------------------------------------------------
    def run(self, argv, cwd=None):
        try:
            p = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        except OSError as e:
            return 127, "%s: %s" % (argv[0], e)
        return p.returncode, p.stdout.decode("utf-8", "replace")

    def run_isp(self, isp, image_path):
        argv = isp_argv_guard(isp_argv(isp, image_path))
        self.isp_calls.append(list(argv))
        return self.run(argv)

    # ---- the network -----------------------------------------------------
    def fetch(self, url, on_start=None):
        """The only network access this script ever makes, and only after the
        user has said yes to it."""
        import urllib.request
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                if on_start is not None:
                    on_start(r.headers.get("Content-Length"))
                return r.read(), None
        except Exception as e:
            return None, str(e)

    # ---- the human -------------------------------------------------------
    def ask(self, out, prompt, tag=None, default="y"):
        """Ask the human.

        `default` is what --yes answers with, and NOTHING ELSE.  An empty line
        is returned as an empty line: confirm() wants a typed word, and a bare
        Enter must never be able to erase somebody's flash because it happened
        to fall through to the default.  The same goes for end-of-input.
        """
        if self.assume_yes:
            out.raw(("%s %s" % (prompt, default)).rstrip()
                    + out.d("  (--yes)"))
            return default
        try:
            ans = input(prompt + " ").strip()
        except (EOFError, KeyboardInterrupt):
            out.raw("")
            return ""
        out.lines.append(strip_ansi(prompt) + " " + ans)
        return ans

    def wait(self, seconds):
        time.sleep(seconds)

    def monotonic(self):
        return time.monotonic()


# The UID the simulated wchisp reports.  Not a placeholder: it has to look
# like the thing being redacted, or the redaction is never really tested.
SIM_CHIP_UID = "A3-7F-21-0C-5E-11-9B-44"


class SimHardware(Hardware):
    """A device you can hold in a variable.

    `mode` is one of:
        "app"     running the application; a CH340 serial port is present
        "isp"     in the ROM ISP; 4348:55E0 on the bus, no serial port
        "absent"  nothing plugged in
        "dead"    powered but enumerating as nothing (a bad application, and
                  the bootloader in update mode is not being modelled as a
                  serial port -- from this script's point of view the two are
                  the same: the application does not answer)

    Everything that would touch hardware is intercepted.  Everything else --
    notably running tools/build_composite.py for its independent second opinion
    -- is executed for real, because that is the thing under test.
    """

    def __init__(self, mode="app", flash=None, fw=None,
                 isp_appears=True, isp_rc=0, answers=None,
                 ports=None, no_pyserial=False):
        self.mode = mode
        # More than one candidate port, and a host with no pyserial: neither is
        # about the DEVICE, and both have been got wrong -- one wrote the wrong
        # board, the other called a successful rescue a failure.
        self.ports = list(ports) if ports else None
        self.no_pyserial = no_pyserial
        self.flash = bytearray(flash) if flash is not None else bytearray()
        self.fw = fw or {"cfw_id": "L", "fw_ver": 15, "pcb_ver": 13,
                         "fw_ts": 1780000000, "pcb_name": "GBFlash"}
        self.isp_appears = isp_appears
        self.isp_rc = isp_rc
        self.answers = list(answers or [])
        self.asked = []
        self.isp_calls = []
        self.serial_opens = 0
        self.clock = 0.0

    # ---- construction helpers -------------------------------------------
    @staticmethod
    def stock_flash(bootloader=None, app=None, zero_region=True):
        """A CodeFlash image shaped like a shipping GBFlash.

        With `zero_region` (the default) 0x00B8..0x3DFF is ZEROS and 0x0000 is a
        verbatim copy of the application's vector table -- the exact state this
        whole project exists for.  With a `bootloader`, it is a device that has
        one already.
        """
        app = app if app is not None else synth_fw()
        img = bytearray(BOOTINFO_BASE)
        if bootloader is not None:
            img[0:len(bootloader)] = bootloader
            for i in range(len(bootloader), BOOTINFO_BASE):
                img[i] = FILL
        else:
            img[0:0xB8] = app[PAYLOAD_OFF:PAYLOAD_OFF + 0xB8]
            for i in range(0xB8, BOOTINFO_BASE):
                img[i] = 0x00 if zero_region else FILL
        return bytes(img) + bytes(app)

    # ---- Hardware --------------------------------------------------------
    def list_app_ports(self):
        if self.ports is not None:
            return list(self.ports)
        return ["/dev/sim-gbflash"] if self.mode == "app" else []

    def open_app(self, port):
        if self.no_pyserial:
            raise MissingDependency(
                "this step needs pyserial\n"
                "(--restore does not need it to WRITE your image; only the "
                "read-back afterwards does.)")
        if self.mode != "app":
            raise LinkError("no device on %s (simulated mode=%s)"
                            % (port, self.mode))
        self.serial_opens += 1
        return SimLink(self.flash, self.fw, port)

    def isp_present(self):
        return self.mode == "isp"

    def run(self, argv, cwd=None):
        try:
            p = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        except OSError as e:
            return 127, "%s: %s" % (argv[0], e)
        return p.returncode, p.stdout.decode("utf-8", "replace")

    def run_isp(self, isp, image_path):
        argv = isp_argv_guard(isp_argv(isp, image_path))
        self.isp_calls.append(list(argv))
        if self.mode != "isp":
            return 1, ("Device not found. Is it in ISP mode?\n"
                       "(simulated: device is in %s mode)" % self.mode)
        if self.isp_rc == 0:
            with open(image_path, "rb") as f:
                data = f.read()
            # wchisp erases ALL of CodeFlash and writes from address 0.
            self.flash = bytearray(data)
        # Roughly what wchisp prints. It is captured, not echoed, so being
        # verbose is the point: a test can tell suppressed from absent.
        if self.isp_rc != 0:
            # wchisp identifies the chip before it fails, so the failure path
            # is where a UID would really leak.
            return self.isp_rc, ("INFO  Chip UID: %s\n"
                                 "Chip is hosed. Reset or power cycle it.\n"
                                 % SIM_CHIP_UID)
        return 0, ("INFO  Chip: CH579[0x7913] (Code Flash: 250KiB)\n"
                   "INFO  Chip UID: %s\n"
                   "INFO  Current config registers: 4d[...]\n"
                   "INFO  Erased code flash\n"
                   "INFO  %d bytes written\n"
                   "INFO  Verify OK\n"
                   "INFO  Now reset device and skip\n"
                   % (SIM_CHIP_UID, os.path.getsize(image_path)))

    def fetch(self, url, on_start=None):
        return None, "a simulated run does not use the network"

    def ask(self, out, prompt, tag=None, default="y"):
        self.asked.append((tag, prompt))
        if self.answers:
            ans = self.answers.pop(0)
        else:
            ans = default
        out.raw(("%s %s" % (prompt, ans)).rstrip()
                + out.d("  (simulated)"))
        self._apply(tag)
        return ans

    def _apply(self, tag):
        """A prompt is where the device physically changes state."""
        if tag == "enter-isp":
            self.mode = "isp" if self.isp_appears else "absent"
        elif tag == "power-cycle":
            self.mode = "app" if self._boots() else "dead"

    def _boots(self):
        """Would this flash come up answering QUERY_FW_INFO?

        The part fetches SP from 0x0000 and PC from 0x0004 before it executes
        anything, so those two words are modelled first and strictly: a
        simulator that boots images a real part would HardFault on cannot test
        the gates that exist to prevent a dark board.

        If there is no bootloader, reset goes straight into the application.
        If there is one, it hands off only when bl_app_valid() accepts.
        """
        if len(self.flash) <= BOOTINFO_BASE + PAYLOAD_OFF:
            return False
        low = bytes(self.flash[:BOOTINFO_BASE])
        if len(low) < BOOTINFO_BASE:
            return False
        app, _ = trim_erased_tail(bytes(self.flash[BOOTINFO_BASE:]))
        sp = struct.unpack("<I", low[0:4])[0]
        reset = struct.unpack("<I", low[4:8])[0]
        if (sp & 0x2FFE0000) != 0x20000000:
            return False                    # no stack; the part faults
        if not reset & 1:
            return False                    # no Thumb bit; the part faults
        target = reset & ~1
        if target >= APP_BASE:
            # No bootloader: the application owns reset. It only runs if the
            # vector actually lands inside it.
            h = bootinfo(app)
            n = h["applen"] if (h and h["tag"] == HDR_TAG) else 0
            return APP_BASE <= target < APP_BASE + n
        cls = classify_low(low)
        if cls["shape"] != "bootloader" or target >= cls["prefix"]:
            return False                    # points into nothing
        return all(c.ok for c in app_gates(app))

    def wait(self, seconds):
        self.clock += seconds

    def monotonic(self):
        self.clock += 0.001
        return self.clock


# --------------------------------------------------------------------------
# Finding the pieces.  A source checkout and a flat release directory are both
# first-class; nothing assumes anybody's layout.
# --------------------------------------------------------------------------

class Tools(object):

    def __init__(self):
        self.bootloader = None
        self.build_composite = None
        self.isp = None
        self.notes = []


def _first_file(cands):
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


def _find_isp(name, cwd):
    """Look for wchisp: beside this script, in tools/, here, on PATH."""
    names = [name, name + ".exe"]
    cands = []
    for n in names:
        cands += [os.path.join(HERE, n), os.path.join(HERE, "tools", n),
                  os.path.join(cwd, n)]
    return _first_file(cands) or shutil.which(name)


def locate(args, out):
    """Find the pieces.  Says nothing; the modes print what they need to."""
    t = Tools()
    cwd = os.getcwd()

    t.bootloader = _first_file([
        args.bootloader,
        os.path.join(HERE, "bootloader.bin"),          # flat release
        os.path.join(HERE, "build", "bootloader.bin"),  # source checkout
        os.path.join(cwd, "bootloader.bin"),
        os.path.join(cwd, "build", "bootloader.bin"),
    ])

    t.build_composite = _first_file([
        os.path.join(HERE, "build_composite.py"),
        os.path.join(HERE, "tools", "build_composite.py"),
        os.path.join(cwd, "build_composite.py"),
        os.path.join(cwd, "tools", "build_composite.py"),
    ])

    if args.isp:
        t.isp = args.isp
    else:
        for name in ISP_TOOLS:
            found = _find_isp(name, cwd)
            if found:
                t.isp = found
                break

    # Only a RELEASE carries a digest to compare against (see BL_SHA256).  In a
    # checkout there is nothing to say, and saying it anyway libelled every
    # from-source build.
    if t.bootloader and BL_SHA256 is not None:
        with open(t.bootloader, "rb") as f:
            data = f.read()
        if hashlib.sha256(data).hexdigest() != BL_SHA256:
            t.notes.append(
                "bootloader.bin is not the one published with this install.py "
                "-- expected if you built it yourself")
    return t


def describe_isp(out, tools):
    """One line.  The path only earns its place when it is not just the tool's
    own name -- "wchisp (wchisp)" tells nobody anything."""
    if not tools.isp:
        return
    shown = pathshow(tools.isp)
    out.ok("wchisp" if shown in ("wchisp", "wchisp.exe")
           else "wchisp  %s" % shown)


# --------------------------------------------------------------------------
# Aborts.  "Aborting." on its own is a failure of this script's whole purpose,
# so there is one function and it will not let you skip the explanation.
# --------------------------------------------------------------------------

def abort(out, why, device_state, next_steps, code=EXIT_REFUSED, extra=()):
    """Every stop says the same three things: what failed, what state the
    device is in, and what to do about it.  Nothing stops without all three.

    Three things, three shapes: the marker line is what failed, the line under
    it is the device, and the list is the actions.  No headings -- the shape
    already says which is which.
    """
    if out.restating(why):
        out.detail([device_state] + list(extra))
    else:
        out.gap()
        out.fail(why, [device_state] + list(extra))
    for s in next_steps:
        first = True
        for line in str(s).split("\n"):
            for wrapped in _wrap(line, WIDTH - 8):
                out.raw(("  - " if first else "    ") + wrapped)
                first = False
    out.raw("")
    return code


# --------------------------------------------------------------------------
# Steps.
# --------------------------------------------------------------------------

def detect(hw, out, args, quiet=False, isp_ok=False):
    """Which of the two USB identities is on the bus, if either.

    `quiet` drops the line for the ordinary "found it on that port" case, for
    the second look after a power-cycle: the port was named the first time and
    the firmware line under it says the device is answering.  Everything
    surprising is still printed.

    `isp_ok` says the caller WANTS a device in ISP mode -- only --restore does.
    Without it, ISP mode is what the caller is about to refuse over, and a tick
    beside it reads as approval of the thing that stops the run one line later.
    Silence here; the refusal does the talking.
    """
    ports = [args.port] if args.port else hw.list_app_ports()
    isp = hw.isp_present()

    if ports:
        # MORE THAN ONE CANDIDATE IS A REFUSAL, NOT A COIN TOSS -- see
        # abort_ambiguous().
        if len(ports) > 1:
            out.warn(ambiguous_line(ports))
            return "ambiguous", ports
        if not quiet:
            out.ok("GBFlash on %s" % ports[0])
        return "app", ports[0]

    if isp is True:
        if isp_ok:
            out.ok("GBFlash in ISP mode -- no LEDs light there; that is "
                   "correct")
        return "isp", None

    if isp is None:
        out.warn("no GBFlash found -- this computer cannot be asked about USB, "
                 "so say so at the prompt if yours is in ISP mode")
        return "none", None

    out.warn("no GBFlash found")
    return "none", None


def ambiguous_line(ports):
    """One wording, used by the warn and by the stop, so the stop can be
    written underneath the warn instead of restating it."""
    return "%d devices could each be a GBFlash  %s" % (len(ports),
                                                       ", ".join(ports))


def abort_ambiguous(out, ports, device_state=None, code=EXIT_REFUSED):
    """More than one board could be the GBFlash.

    The backup is read over a serial port and the write goes to whichever board
    answers on USB as the CH579 ROM ISP; with two attached those can differ, so
    device A's firmware lands on device B with a backup only of A.  Nothing in
    the ISP protocol lets the write be aimed, so insist on one candidate.
    """
    return abort(out, ambiguous_line(ports),
                 device_state or "Untouched -- nothing read, nothing written.",
                 ["Unplug the others, leaving one GBFlash, and run this again.",
                  "Or name it: --port %s" % ports[0]],
                 code)


# `pip install pyserial` is the advice everywhere, and on a PEP 668 host --
# Debian 12+, Ubuntu 23.04+, Fedora, Homebrew python -- it exits with
# "externally-managed-environment" and installs nothing.  Naming the way out in
# the same breath costs one line and saves a search.
#
# `python3` is the wrong launcher on Windows: it is an App Execution Alias that
# opens the Microsoft Store rather than running Python, so telling a Windows
# user to type it sends them shopping.
PY_LAUNCHER = "py" if sys.platform == "win32" else "python3"

PYSERIAL_STEPS = [
    "%s -m pip install pyserial" % PY_LAUNCHER,
    "If that says \"externally-managed-environment\", use your system package "
    "instead -- on Debian/Ubuntu: sudo apt install python3-serial",
]


def abort_no_pyserial(out, device_state, code=EXIT_REFUSED):
    """This computer is missing pyserial.  The DEVICE is fine.

    MissingDependency subclasses LinkError, and every LinkError handler but one
    reported it as "plugged in, but it would not open" with EXIT_NODEV and told
    the user to unplug a device that was never the problem.  Naming the real
    cause is the whole fix; EXIT_REFUSED because nothing was attempted.
    """
    return abort(out, "this needs pyserial, which is not installed",
                 device_state, list(PYSERIAL_STEPS), code)


def no_device_is_really_no_pyserial(hw):
    """Did the port scan come up empty only because pyserial is missing?

    The scan falls back to globbing /dev/cu.usbserial* and friends, which finds
    nothing on Windows however healthy the device -- a COM port has no such
    name.  So on the one platform where the fallback cannot work, an empty list
    was being reported as "no GBFlash found -- Not on USB at all", which is the
    misdiagnosis abort_no_pyserial() exists to stop.

    Checked by the three modes that need a live device, and deliberately NOT by
    --restore, which writes over the ISP and is documented to need neither
    pyserial nor a device that answers.
    """
    return bool(getattr(hw, "no_pyserial", False))


def fw_line(fw):
    """The one line that identifies the running firmware.

    pcb_ver is the only part of this not baked into the firmware image -- the
    firmware answers it out of RAM at 0x200000AC, filled in by a start-up
    probe.  One real run printed two different values; see the open bug in
    docs/RELEASE-NOTES.md.
    """
    return "firmware %s%d, PCB %d" % (fw["cfw_id"], fw["fw_ver"],
                                      fw["pcb_ver"])


def fw_banner(out, fw):
    out.ok(fw_line(fw))


def check_region(link, out, quick=False):
    """Read 0x0000..0x3DFF and decide what is there.  Read-only."""
    head = read_range(link, VECTORS_LO, REGION_LO + 0, out, "reading")
    sp = struct.unpack("<I", head[0:4])[0]
    reset = struct.unpack("<I", head[4:8])[0]

    lo = REGION_LO - (REGION_LO % 4)
    if quick:
        body = bytearray()
        body += link.read_word(lo)
        addr = (REGION_LO + SECTOR) & ~(SECTOR - 1)
        while addr < BOOTINFO_BASE:
            body += link.read_word(addr)
            addr += SECTOR
        body = bytes(body)
        coverage = "sampled"
    else:
        body = read_range(link, lo, BOOTINFO_BASE, out, "reading")
        coverage = "read in full"

    nonzero = sum(1 for b in body if b != 0x00)
    nonff = sum(1 for b in body if b != FILL)

    # The verdict printed immediately below says everything a person needs.
    # The byte census and the reset address are for a bug report, so they go
    # where the rest of the machinery went.
    if SHOW_CHECKS:
        out.dim("bootloader area (%s): %s of %s bytes are non-zero; reset "
                "vector 0x%08X" % (coverage, commas(nonzero),
                                   commas(len(body)), reset))

    if nonzero == 0:
        verdict = "empty"
    elif nonff == 0:
        verdict = "erased"
    elif (reset & ~1) < BOOTINFO_BASE:
        verdict = "present"
    else:
        verdict = "unexpected"
    return {"verdict": verdict, "sp": sp, "reset": reset,
            "nonzero": nonzero, "head": head, "body": body}


# One line per verdict: the marker, then what is there and what it means.
# "ok" is the state this script exists to change; "!!" is anything else.
VERDICT_TEXT = {
    "empty": ("ok", "no bootloader -- that is why firmware updates fail"),
    "erased": ("warn", "bootloader area is erased -- something wiped it"),
    "present": ("warn",
                "bootloader already present -- updates may already work"),
    "unexpected": ("warn", "bootloader area holds something unrecognised"),
}


def say_verdict(out, res):
    kind, text = VERDICT_TEXT[res["verdict"]]
    out.mark(kind, text)


def take_backup(link, out, path):
    """Full CodeFlash dump, streamed to disk as it is read."""
    rec = bytearray()
    for addr in range(BOOTINFO_BASE, BOOTINFO_BASE + 0x10, 4):
        rec += link.read_word(addr)
    h = bootinfo(bytes(rec))

    if h["tag"] != HDR_TAG:
        raise LinkError(
            "the device did not describe its own firmware recognisably, so "
            "there is no knowing how much to read. Try --check, and read "
            "docs/RECOVERY.md.")
    if not (BL_APP_MIN_LEN <= h["applen"] <= BL_APP_MAX_LEN):
        raise LinkError(
            "the device reports a firmware size that cannot be right "
            "(0x%X bytes)" % h["applen"])

    end = APP_BASE + h["applen"]
    if end & 3:
        end = (end + 3) & ~3

    outdir = os.path.dirname(os.path.abspath(path))
    try:
        if outdir:
            os.makedirs(outdir, exist_ok=True)
        fp = open(path, "wb")
    except OSError as e:
        raise LinkError("could not create %s -- %s" % (pathshow(path), e))
    try:
        with fp:
            image = read_range(link, 0, end, out, "backing up", sink=fp)
    except OSError as e:
        # A full disk or a removed stick partway through the dump. Nothing has
        # been written to the DEVICE, and the caller's abort block says so --
        # but it has to be an abort block, not a bare traceback.
        raise LinkError("could not finish writing %s -- %s"
                        % (pathshow(path), e))
    return image


def wait_for_isp(hw, out, timeout=180):
    """Poll for 4348:55E0 after the jumper step."""
    t0 = hw.monotonic()
    first = hw.isp_present()
    if first is None:
        return None
    while hw.monotonic() - t0 < timeout:
        if hw.isp_present():
            out.ok("device in ISP mode")
            return True
        out.busy("waiting for ISP mode  %ds" % int(hw.monotonic() - t0))
        hw.wait(1.0)
    return False


# Flat, imperative, and in the order the hands do them.  Every line here has
# stopped a real mistake: the other board gets written instead, the short is
# released before power arrives, or the U22 button is pressed for minutes.
JUMPER_STEPS = [
    "Unplug any other CH579 board",
    "Unplug USB",
    "Short the two H1 pads together -- H1, not the U22 button",
    "Plug USB back in while shorted",
    "Release the short",
]


def guide_jumper(hw, out):
    out.gap()
    out.say("Fit the H1 jumper:")
    for i, s in enumerate(JUMPER_STEPS, 1):
        out.step(i, s)
    out.say()
    # This is the line that stops someone unplugging a correctly jumpered
    # board because they think a dark board means it failed.
    out.say("No LEDs light in ISP mode; that is correct.")
    hw.ask(out, "Press Enter when plugged back in:", tag="enter-isp",
           default="")
    out.say()
    return wait_for_isp(hw, out)


def run_isp_flash(hw, out, tools, image_path):
    """Exactly one ISP command per power-on.  That is a ROM limitation, not a
    style choice: a second command in the same session answers 'Chip is
    hosed.'

    wchisp narrates ~35 lines, one of them the chip's UID.  None of it is the
    user's business while it is working, so it is captured; when it FAILS it is
    the only diagnostic there is and the caller prints it, with the UID taken
    out by isp_log() and isp_said().
    """
    out.busy("writing")
    rc, text = hw.run_isp(tools.isp, image_path)
    if rc == 0:
        out.ok("written and verified")
    if VERBOSE:
        isp_log(out, text)
    return rc, text


UID_LINE = re.compile(r"(?i)^(.*\b(?:chip\s+uid|unique\s+id|uid)\b\s*[:=]?\s*)"
                      r".*$")
UID_BYTES = re.compile(r"(?i)\b(?:[0-9a-f]{2}-){3,}[0-9a-f]{2}\b")


def redact_uid(text):
    """Take the chip's serial number out of wchisp's log.

    Two rules, because a bug report is pasted whole: a line naming a UID keeps
    its label and loses its value, and a bare run of hyphen-separated bytes
    goes too, so a wchisp that labels it differently still cannot leak it.
    Everything that prints captured tool output goes through here.
    """
    out = []
    for line in text.split("\n"):
        out.append(UID_BYTES.sub("(withheld)",
                                 UID_LINE.sub(r"\1(withheld)", line)))
    return "\n".join(out)


def isp_log(out, text):
    """wchisp's own output, indented under the line that referred to it."""
    for line in redact_uid(text).rstrip("\n").split("\n"):
        out.say(line, indent=MARK_W + 1)


def isp_said(text, limit=12):
    """wchisp's last words, for a stop block. Attributed, because an
    unlabelled line of somebody else's log reads as this script's own."""
    lines = [l for l in redact_uid(text).rstrip("\n").split("\n") if l.strip()]
    return ["wchisp: " + l for l in lines[-limit:]]


def cross_check_composite(hw, out, tools, bl_path, backup_path, img):
    """Second opinion from tools/build_composite.py, if it is here.

    The two implementations are independent, so agreement byte for byte is
    worth more than either one's own gates.
    """
    if not tools.build_composite:
        if SHOW_CHECKS:
            out.dim("second opinion skipped: build_composite.py is not here")
        return True
    tmp = tempfile.mkdtemp(prefix="gbflash-xcheck-")
    other = os.path.join(tmp, "composite-crosscheck.bin")
    argv = [sys.executable, tools.build_composite,
            "--bootloader", bl_path, "--backup", backup_path, "--out", other]
    rc, text = hw.run(argv)
    ok = (rc == 0)
    if ok and os.path.isfile(other):
        with open(other, "rb") as f:
            theirs = f.read()
        same = (theirs == img)
        ok = same
        if same:
            # Silent on success: the check exists for the case below.
            if SHOW_CHECKS:
                out.ok("second builder agrees, byte for byte")
        else:
            out.fail("a second builder produced a DIFFERENT image")
    else:
        ok = False
        out.fail("the second-opinion build did not run",
                 text.rstrip("\n").split("\n")[-12:])
    shutil.rmtree(tmp, ignore_errors=True)
    return ok


def confirm(hw, out, lines, word, tag=None):
    """A destructive step.  Says plainly what is about to happen and how to
    undo it, then wants the word typed -- not a shrug."""
    out.gap()
    for line in lines:
        out.say(line)
    out.say()
    ans = hw.ask(out, "Type %s to continue:" % word.upper(), tag=tag,
                 default=word)
    return ans.strip().lower() == word.lower()


# --------------------------------------------------------------------------
# Modes.
# --------------------------------------------------------------------------

def cmd_check(hw, out, args, tools):
    state, port = detect(hw, out, args)
    if state == "ambiguous":
        return abort_ambiguous(out, port)
    if state == "isp":
        return abort(out, "device is in ISP mode; this check needs it running",
                     "In ISP mode. Nothing has been written to it.",
                     ["Unplug it, clear anything shorting H1, plug it back in.",
                      "Then run this again."])
    if state != "app":
        if no_device_is_really_no_pyserial(hw):
            return abort_no_pyserial(out, "Not looked at -- this computer has "
                                          "no way to open a serial port.")
        return abort(out, "no GBFlash found", "Not on USB at all.",
                     ["Plug it in.",
                      "Close FlashGBX -- one program at a time.",
                      "On Linux you may need to be in the dialout group.",
                      "Unusual port name? --port NAME"],
                     EXIT_NODEV)

    try:
        link = hw.open_app(port)
    except MissingDependency:
        return abort_no_pyserial(out, "Plugged in and fine; this computer "
                                      "cannot read from it. Nothing written.")
    except LinkError as e:
        return abort(out, str(e), "Plugged in, but it would not open.",
                     ["Close FlashGBX and any other serial tool.",
                      "Unplug and replug the device."], EXIT_NODEV)
    try:
        fw = link.query_fw()
        fw_banner(out, fw)
        if fw["fw_ver"] < MIN_FW_VER:
            return abort(out, "firmware %s%d is too old to read flash from"
                              % (fw["cfw_id"], fw["fw_ver"]),
                         "Running and untouched.",
                         ["Update the firmware with FlashGBX first."])
        res = check_region(link, out, quick=bool(args.quick))
    except LinkError as e:
        return abort(out, str(e),
                     "Plugged in; the read did not finish. Nothing written.",
                     ["Close FlashGBX and try again."], EXIT_FAILED)
    finally:
        link.close()

    say_verdict(out, res)
    out.say()
    if res["verdict"] == "empty":
        out.say("Firmware updates will not work until the bootloader is "
                "installed.")
        out.say("Install it: python3 %s"
                % pathshow(os.path.abspath(__file__)))
    elif res["verdict"] == "present":
        out.say("Firmware updates should already work. It may not be this "
                "exact bootloader.")
    else:
        out.say("Do not install anything until you know what put that there. "
                "docs/RECOVERY.md has the next steps.")
    return EXIT_OK


def do_backup(hw, out, args, port, path, quiet_fw=False, overridden=False):
    """Shared by --backup and the install.

    Returns (image|None, exit code, verified).  `verified` is the gate the
    install obeys: no verified backup, no install, unless the caller was given
    --allow-unverified-backup.

    `quiet_fw` suppresses the firmware line, for the install, which has already
    printed it.  `overridden` says the caller holds --allow-unverified-backup
    and will carry on regardless, so a failing backup must warn rather than
    print a stop block that then does not happen.
    """
    try:
        link = hw.open_app(port)
    except MissingDependency:
        return None, abort_no_pyserial(
            out, "Plugged in and fine; this computer cannot read from it. "
                 "Nothing written."), False
    except LinkError as e:
        return None, abort(out, str(e),
                           "Plugged in, but it would not open. Nothing written.",
                           ["Close FlashGBX and any other serial tool.",
                            "Unplug and replug the device."],
                           EXIT_NODEV), False
    try:
        fw = link.query_fw()
        if not quiet_fw:
            fw_banner(out, fw)
        image = take_backup(link, out, path)
    except LinkError as e:
        return None, abort(out, str(e),
                           "Plugged in and unchanged -- this step only reads.",
                           ["Close FlashGBX, unplug and replug, try again.",
                            "If it keeps failing, stop. The install will not "
                            "run without a backup it trusts."],
                           EXIT_FAILED), False
    finally:
        link.close()

    out.ok("backup  %s  %s bytes" % (pathshow(path), commas(len(image))))

    # READ THE FILE BACK BEFORE JUDGING IT.  The thing that gets restored is
    # the FILE, and a short write or a full disk leaves one that differs from
    # what the device said, so the gates below run on the file's own bytes.
    try:
        with open(path, "rb") as f:
            ondisk = f.read()
    except OSError as e:
        return image, abort(
            out, "the backup saved but would not read back -- %s" % e,
            "Plugged in and unchanged. This step only reads.",
            ["Check the file and the free space at %s." % pathshow(path),
             "Save it somewhere else: --backup-out FILE"], EXIT_FAILED), False

    checks = [
        Check(len(ondisk) == len(image),
              "the file on disk is as long as what was read",
              "%s bytes on disk, %s read from the device"
              % (commas(len(ondisk)), commas(len(image)))),
        Check(hashlib.sha256(ondisk).hexdigest() ==
              hashlib.sha256(image).hexdigest(),
              "the file on disk is byte for byte what came off the device"),
    ]
    checks += backup_gates(ondisk)
    bad = report(out, "backup", checks, "backup verified")
    if bad and overridden:
        # NOTHING HERE MAY CLAIM A STOP THAT IS NOT HAPPENING.  The caller is
        # going to carry on, so the stop block -- "nothing will be written" --
        # would be a flat lie three lines before the write.  One warn, carrying
        # what is being accepted and what it costs.
        out.warn("backup does not verify -- %s; carrying on anyway"
                 % failed_of(bad, checks),
                 ["--allow-unverified-backup: the write goes ahead, and %s "
                  "may not put the device back if it goes wrong"
                  % pathshow(path)] + show_checks_hint())
        return ondisk, EXIT_OK, False
    if bad:
        return ondisk, abort(
            out, "backup does not verify -- %s" % failed_of(bad, checks),
            "Plugged in and unchanged. Nothing written to it, and nothing "
            "will be.",
            ["Close FlashGBX and any other serial tool, then run this again.",
             "Try a different USB cable or port.",
             "Delete %s so it is not mistaken for a real backup later."
             % pathshow(path)] + show_checks_hint(),
            EXIT_REFUSED), False
    # The FILE is what was verified, so the file is what the caller gets: the
    # composite is built from exactly the bytes the user would later restore.
    return ondisk, EXIT_OK, True


def cmd_backup(hw, out, args, tools):
    state, port = detect(hw, out, args)
    if state == "ambiguous":
        return abort_ambiguous(out, port)
    if state != "app":
        if no_device_is_really_no_pyserial(hw):
            return abort_no_pyserial(out, "Not looked at -- this computer has "
                                          "no way to open a serial port.")
        return abort(out, "only a device that is running normally can be copied",
                     "In ISP mode -- unplug it and plug it back in with "
                     "nothing shorting H1." if state == "isp"
                     else "Not on USB at all.",
                     ["Plug the device in and let it start up.",
                      "Close FlashGBX first.",
                      "If it will not start up there is nothing to copy. Use "
                      "--restore with an older backup, if you have one."],
                     EXIT_NODEV)
    _, rc, _ = do_backup(hw, out, args, port, args.backup)
    if rc == EXIT_OK:
        out.say()
        out.done("Backed up. Keep it somewhere other than the device.")
        out.say("Undo anything with: python3 %s --restore %s"
                % (pathshow(os.path.abspath(__file__)),
                   pathshow(args.backup)))
    return rc


def cmd_restore(hw, out, args, tools):
    """Write a backup image back over the ROM ISP.

    THIS PATH MUST WORK ON A DEVICE THAT WILL NOT BOOT.  Everything up to and
    including the write therefore touches no serial port, imports no pyserial
    and asks the application nothing -- there may not be one.  The ROM ISP is
    mask ROM, reachable by shorting H1 at power-on whatever is in CodeFlash.

    AFTER the write the device gets a chance to prove it came back, which does
    want pyserial.  That is confirmation, not rescue: if it cannot run, the
    restore is still reported as the success it was.
    """
    path = args.restore

    if not os.path.isfile(path):
        return abort(out, "no such file: %s" % pathshow(path),
                     "Untouched. Nothing has been written to it.",
                     ["Check the path.",
                      "Backups from this script are called "
                      "gbflash-backup-<date>.bin, wherever you ran it from."],
                     EXIT_USAGE)
    try:
        with open(path, "rb") as f:
            image = f.read()
    except OSError as e:
        return abort(out, "cannot read %s -- %s" % (pathshow(path), e),
                     "Untouched.", ["Check permissions on the file."],
                     EXIT_USAGE)

    out.ok("%s  %s bytes" % (pathshow(path), commas(len(image))))

    gates = backup_gates(image)
    bad = report(out, "restore image", gates, "image verified")
    if bad and not args.restore_unverified:
        return abort(out, "that file is not a good backup -- %s"
                          % failed_of(bad, gates),
                     "Untouched. Nothing has been written.",
                     ["Find a backup that passes these checks and use that.",
                      "Only copy, and the device is already dead? Force it "
                      "with --restore-unverified."]
                     + show_checks_hint()
                     + ["docs/RECOVERY.md has more."], EXIT_REFUSED)
    if bad:
        out.warn("--restore-unverified: writing a file that failed %d check(s) "
                 "-- H1 will still work afterwards, but the device may not "
                 "start up" % len(bad))

    if not ensure_isp_tool(hw, out, tools):
        return no_isp_tool(out, "Untouched, and no jumper has been asked for.")
    describe_isp(out, tools)

    state, port = detect(hw, out, args, isp_ok=True)
    if state == "ambiguous":
        return abort_ambiguous(out, port)
    if state == "app":
        out.warn("device is running -- restoring replaces everything on it")
        # A LIVE DEVICE PLUS A FAILING IMAGE IS THE ONE COMBINATION THAT TURNS
        # A RESCUE INTO A CASUALTY.  --restore-unverified argues the device is
        # already dead and cannot get worse; a board answering on a serial port
        # is not dead, and writing a failing image over it destroys the only
        # good copy of the firmware in existence.
        if bad and not args.restore_over_a_working_device:
            return abort(
                out, "the device is working and that file failed %d check(s)"
                     % len(bad),
                "Plugged in, answering on %s, and unchanged." % port,
                ["Back up the device in front of you FIRST. It is alive, so "
                 "this still works:\n  python3 %s --backup my-device.bin"
                 % pathshow(os.path.abspath(__file__)),
                 "Then restore that, or find a backup that passes its checks.",
                 "To overwrite a working device with a file you know is "
                 "broken, add --restore-over-a-working-device too.",
                 "docs/RECOVERY.md has more."], EXIT_REFUSED)
        if bad:
            out.warn("--restore-over-a-working-device: the firmware on it now "
                     "is about to stop existing anywhere")
        out.warn("back THIS device up first if you have not -- it is running, "
                 "so you still can: python3 %s --backup my-device.bin"
                 % pathshow(os.path.abspath(__file__)))

    prose = ["About to erase and rewrite the device with %s (%s bytes)."
             % (pathshow(path), commas(len(image)))]
    if state == "isp":
        prose.append("Already in ISP mode; no jumper needed.")
    else:
        prose.append("The H1 jumper is needed for the write.")
    if args.dry_run:
        prose.append("Dry run: nothing physical is touched.")

    if not confirm(hw, out, prose, "restore", tag="confirm-restore"):
        return abort(out, "not confirmed, so nothing happened",
                     "Untouched. Nothing has been written.",
                     ["Run the same command again when you are ready."],
                     EXIT_REFUSED)

    if state != "isp":
        got = guide_jumper(hw, out)
        if got is False:
            return abort(out, "the device never appeared in ISP mode",
                         "Not in ISP mode, and untouched.",
                         ["The short has to be held as power arrives. Unplug, "
                          "short H1, plug in -- in that order.",
                          "H1 is the ISP strap, not the U22 button.",
                          "No LEDs light in ISP mode.",
                          "Then run the same command again.",
                          "If it never works, read docs/RECOVERY.md."],
                         EXIT_FAILED)

    rc, text = run_isp_flash(hw, out, tools, path)
    if rc != 0:
        return abort(out, "the write failed",
                     "In ISP mode, possibly half-erased.",
                     ["Unplug, redo the H1 steps, run the same command again.",
                      "On Linux, check USB permissions for %04X:%04X."
                      % (VID_ISP, PID_ISP),
                      "docs/RECOVERY.md has more."],
                     EXIT_FAILED, extra=isp_said(text))

    out.say()
    out.say("Unplug, clear anything shorting H1, plug back in.")
    hw.ask(out, "Press Enter:", tag="power-cycle", default="")
    out.say()

    state, port = detect(hw, out, args, quiet=True)
    if state == "ambiguous":
        return abort_ambiguous(
            out, port,
            "carrying the image you just wrote -- the ISP verified it as it "
            "went. It has not been checked further, because with several "
            "candidate ports present this script cannot tell which one is it.",
            EXIT_FAILED)
    # STILL IN ISP MODE IS THE SHORT, NOT A BAD WRITE.  Left to the generic
    # "not answering" stop below it would send someone hunting another backup
    # over a device that is fine.
    if state == "isp":
        return abort(out, "still in ISP mode -- H1 is still shorted",
                     "Carrying the file you restored, verified as it was "
                     "written.",
                     ["Unplug, take the short off H1, plug back in.",
                      "Then: %s %s --check"
                      % (PY_LAUNCHER,
                         pathshow(os.path.abspath(__file__)))], EXIT_FAILED)
    if state == "app":
        # THE WRITE HAS ALREADY LANDED AND THE ISP VERIFIED IT AS IT WENT.
        # Everything from here is confirmation, so a problem HERE is a problem
        # with this computer, not with the board -- and must never be reported
        # as "the restore failed" or answered with "try another backup".
        try:
            link = hw.open_app(port)
        except MissingDependency as e:
            out.warn("came back on %s; not double-checked from here" % port,
                     str(e).split("\n"))
            out.say()
            out.done("Restored. Device is running again.")
            out.say("Check it later with: %s -m pip install pyserial, "
                    "then %s %s --check"
                    % (PY_LAUNCHER, PY_LAUNCHER,
                       pathshow(os.path.abspath(__file__))))
            return EXIT_OK
        except LinkError as e:
            # SAME RULE AS THE BRANCH ABOVE, AND FOR THE SAME REASON.  The ISP
            # verified the write as it went and the board came back on USB; a
            # port this computer cannot open -- FlashGBX holding it, no dialout
            # group -- says nothing about the flash.  Exiting non-zero here
            # announced a restore that WORKED as a failure, and the natural
            # response to that is to erase the board and write it again.
            # docs/RECOVERY.md promises both halves of this; only one was true.
            out.warn("came back on %s, but would not open to check -- %s"
                     % (port, e))
            out.detail(["Close FlashGBX and any other serial tool; one "
                        "program at a time.",
                        "On Linux you may need to be in the dialout group.",
                        "Then check it: %s %s --check"
                        % (PY_LAUNCHER, pathshow(os.path.abspath(__file__))),
                        "Do NOT restore again because of this. Nothing here "
                        "says the write was wrong."])
            out.say()
            out.done("Restored. The write was verified as it was made.")
            return EXIT_OK
        try:
            fw = link.query_fw()
            fw_banner(out, fw)
            back = None
            if fw["fw_ver"] >= MIN_FW_VER:
                back = read_range(link, 0, len(image), out, "checking")
        except LinkError as e:
            out.warn("the read-back did not finish -- %s" % e)
            back = None
        finally:
            link.close()

        if back is not None:
            bad2 = report(out, "read-back",
                          [Check(back == image,
                                 "flash matches your image, byte for byte",
                                 "%s bytes compared" % commas(len(image)))],
                          "device matches the file, byte for byte")
            if bad2:
                return abort(out, "came back holding something other than the "
                                  "file that was restored",
                             "Running and readable, but holding something "
                             "else.",
                             ["Unplug, redo the H1 steps, run the same command "
                              "again.",
                              "Same result twice: try another backup.",
                              "docs/RECOVERY.md has more."], EXIT_FAILED)
        out.say()
        out.done("Restored. Device is running again.")
        return EXIT_OK

    return abort(out, "the file was written, but the device is not answering",
                 "Holding the file you restored, verified as it was written.",
                 ["ACT LED blinking fast and continuously: a bootloader is "
                  "running. Install a stock fw.bin with any updater.",
                  "Board completely dark: try another backup.",
                  "docs/RECOVERY.md has more."], EXIT_FAILED)


def cmd_install(hw, out, args, tools):
    # ---- what is here ----------------------------------------------------
    if not tools.bootloader:
        return abort(out, "bootloader.bin not found",
                     "Untouched. Nothing has been written.",
                     ["From a release: bootloader.bin sits next to this script.",
                      "From a source checkout: run `make` first.",
                      "Or name it: --bootloader /path/to/bootloader.bin"],
                     EXIT_REFUSED)
    with open(tools.bootloader, "rb") as f:
        bl = f.read()
    gates = bootloader_gates(bl)
    bad = report(out, "bootloader.bin", gates)
    if bad:
        return abort(out, "bootloader.bin is not usable -- %s"
                          % failed_of(bad, gates),
                     "Untouched. Nothing has been written.",
                     ["Download it again, or rebuild it with `make`.",
                      "A bootloader image is at most %s bytes."
                      % commas(BOOTINFO_BASE)]
                     + show_checks_hint(),
                     EXIT_REFUSED)
    out.ok("%s  %s bytes" % (pathshow(tools.bootloader), commas(len(bl))))
    for n in tools.notes:
        out.warn(n)

    # THE ISP TOOL IS SORTED OUT NOW, NOT LATER.  Discovering it is missing
    # after someone has been told to short H1 and power-cycle their board would
    # be a gratuitous scare.
    if not tools.isp:
        if not ensure_isp_tool(hw, out, tools):
            return no_isp_tool(out,
                               "Untouched, and no jumper has been asked for.")
    describe_isp(out, tools)

    # ---- what is the device doing ----------------------------------------
    state, port = detect(hw, out, args)
    if state == "ambiguous":
        return abort_ambiguous(out, port)
    if state == "isp":
        return abort(out, "the device is in ISP mode; this needs it running",
                     "In ISP mode. Nothing has been written to it.",
                     ["Unplug, clear anything shorting H1, plug in, run this "
                      "again.",
                      "If it does not start up: python3 %s --restore BACKUP.bin"
                      % pathshow(os.path.abspath(__file__))])
    if state != "app":
        if no_device_is_really_no_pyserial(hw):
            return abort_no_pyserial(out, "Not looked at, and nothing written "
                                          "-- this computer has no way to open "
                                          "a serial port.")
        return abort(out, "no GBFlash found", "Not on USB at all.",
                     ["Plug it in and let it start up.",
                      "Close FlashGBX -- one program at a time.",
                      "On Linux you may need to be in the dialout group.",
                      "Unusual port name? --port NAME",
                      "Plugged in but completely dark and never showing up? "
                      "That is a rescue, not an install: see --restore and "
                      "docs/RECOVERY.md."], EXIT_NODEV)

    # ---- is the region empty ---------------------------------------------
    try:
        link = hw.open_app(port)
    except MissingDependency:
        return abort_no_pyserial(out, "Plugged in and fine; this computer "
                                      "cannot read from it. Nothing written.")
    except LinkError as e:
        return abort(out, str(e),
                     "Plugged in, but it would not open. Nothing written.",
                     ["Close FlashGBX and any other serial tool.",
                      "Unplug and replug the device."], EXIT_NODEV)
    try:
        fw = link.query_fw()
        fw_banner(out, fw)
        if fw["fw_ver"] < MIN_FW_VER:
            link.close()
            return abort(out, "firmware %s%d is too old to read flash from"
                              % (fw["cfw_id"], fw["fw_ver"]),
                         "Running and untouched.",
                         ["Update the firmware with FlashGBX first, then run "
                          "this again."])
        res = check_region(link, out, quick=False)
    except LinkError as e:
        link.close()
        return abort(out, str(e),
                     "Plugged in and unchanged -- that step only reads.",
                     ["Close FlashGBX and try again."], EXIT_FAILED)
    say_verdict(out, res)

    if res["verdict"] != "empty":
        if not args.overwrite_existing_bootloader:
            link.close()
            me = pathshow(os.path.abspath(__file__))
            # The verdict line is already on screen, word for word, so this
            # reuses it: abort() then writes underneath it instead of saying
            # the same thing twice under a second marker.
            why = VERDICT_TEXT[res["verdict"]][1]
            if res["verdict"] == "present":
                extra = ["If firmware updates already work, you do not need "
                         "this at all.",
                         "Whatever is there might not be this bootloader. To "
                         "replace it anyway: --overwrite-existing-bootloader",
                         "Either way, back up first: python3 %s --backup "
                         "my-device.bin" % me]
            else:
                extra = ["Back up before anything else: python3 %s --backup "
                         "my-device.bin" % me,
                         "Then read docs/RECOVERY.md.",
                         "If you do know what is there: "
                         "--overwrite-existing-bootloader"]
            return abort(out, why,
                         "Plugged in, running normally, and unchanged. This "
                         "step only reads.", extra)
        out.warn("--overwrite-existing-bootloader: whatever is there now will "
                 "be erased")

    # ---- backup -----------------------------------------------------------
    link.close()
    backup_path = args.backup_out or default_backup_name()
    image, rc, verified = do_backup(hw, out, args, port, backup_path,
                                    quiet_fw=True,
                                    overridden=args.allow_unverified_backup)
    if image is None:
        return rc
    if not verified and not args.allow_unverified_backup:
        return rc

    # ---- composite --------------------------------------------------------
    base, _ = trim_erased_tail_full(image)
    app = base[BOOTINFO_BASE:]
    img = build_composite(bl, app)

    # These are the same checks the backup ran, so on a pass they print nothing
    # here.  On a failure the abort below is the only headline.
    ag = app_gates(app)
    cg = composite_gates(img, bl, app, base)
    bad = report(out, "firmware", ag)
    bad += report(out, "install image", cg)
    ok_x = cross_check_composite(hw, out, tools, tools.bootloader,
                                 backup_path, img)
    if bad or not ok_x:
        return abort(out, "the install image did not come out right -- %s"
                          % failed_of(bad, ag, cg),
                     "Plugged in, running normally, and unchanged. Your backup "
                     "at %s is %s." % (pathshow(backup_path),
                                       "good" if verified else "the one above"),
                     ["Download bootloader.bin again, then run this again.",
                      "Do not flash anything by hand to get past it."]
                     + show_checks_hint(),
                     EXIT_REFUSED)

    comp_path = args.composite or default_composite_name()
    try:
        with open(comp_path, "wb") as f:
            f.write(img)
    except OSError as e:
        return abort(out, "could not write %s -- %s" % (pathshow(comp_path), e),
                     "Plugged in and unchanged.",
                     ["Pick somewhere you can write to: --composite FILE"],
                     EXIT_FAILED)
    out.ok("install image built  %s bytes" % commas(len(img)))

    # ---- jumper and write -------------------------------------------------
    me = pathshow(os.path.abspath(__file__))
    prose = ["About to erase and rewrite the device. Your firmware is "
             "preserved.",
             # The unverified backup is one --restore refuses, so the undo
             # line has to carry the flag that actually writes it.
             "Undo: python3 %s --restore %s%s"
             % (me, pathshow(backup_path),
                "" if verified else " --restore-unverified"),
             "The H1 jumper is needed for the write."]
    if args.dry_run:
        prose.append("Dry run: nothing physical is touched.")

    if not confirm(hw, out, prose, "install", tag="confirm-install"):
        return abort(out, "not confirmed, so nothing happened",
                     "Plugged in, running normally, and unchanged. Your backup "
                     "at %s is still there." % pathshow(backup_path),
                     ["Run this again when you are ready."], EXIT_REFUSED)

    got = guide_jumper(hw, out)
    if got is False:
        return abort(out, "the device never appeared in ISP mode",
                     "Not in ISP mode, and unchanged.",
                     ["The short has to be held as power arrives. Unplug, "
                      "short H1, plug in -- in that order.",
                      "H1 is the ISP strap, not the U22 button.",
                      "No LEDs light in ISP mode.",
                      "Then run this again.",
                      "If it never works, read docs/RECOVERY.md."],
                     EXIT_FAILED)

    rc, text = run_isp_flash(hw, out, tools, comp_path)
    if rc != 0:
        return abort(out, "the write failed",
                     "In ISP mode, possibly half-erased; it may not start up "
                     "until this is finished.",
                     ["Unplug, redo the H1 steps, run this again.",
                      restore_bullet(me, backup_path, verified,
                                     "Or put it back the way it was"),
                      "On Linux, check USB permissions for %04X:%04X."
                      % (VID_ISP, PID_ISP)], EXIT_FAILED,
                     extra=isp_said(text))

    # ---- verify -----------------------------------------------------------
    out.say()
    out.say("Unplug, remove the jumper, plug back in.")
    hw.ask(out, "Press Enter:", tag="power-cycle", default="")
    out.say()

    state, port = detect(hw, out, args, quiet=True)
    if state == "ambiguous":
        return abort_ambiguous(
            out, port,
            "Holding the image that was written and verified as it went, and "
            "not checked further.",
            EXIT_FAILED)
    # STILL IN ISP MODE IS THE JUMPER, NOT A BAD INSTALL.  The generic stop
    # below would talk about restoring a backup over a device that is fine.
    if state == "isp":
        return abort(out, "still in ISP mode -- H1 is still shorted",
                     "Holding the image that was written and verified as it "
                     "went.",
                     ["Unplug, take the short off H1, plug back in.",
                      "Then: %s %s --check"
                      % (PY_LAUNCHER, me)], EXIT_FAILED)
    if state != "app":
        return abort(out, "the device is not answering after the install",
                     "Holding the image that was written and verified as it "
                     "went.",
                     ["ACT LED blinking fast and continuously: the bootloader "
                      "is running. Install a stock fw.bin with any updater.",
                      restore_bullet(me, backup_path, verified,
                                     "Board completely dark: put it back the "
                                     "way it was"),
                      "Close FlashGBX if it is open -- it holds the device.",
                      "docs/RECOVERY.md has more."], EXIT_FAILED)

    try:
        link = hw.open_app(port)
        fw2 = link.query_fw()
        fw_banner(out, fw2)
        back = read_range(link, 0, len(img), out, "checking")
        link.close()
    except LinkError as e:
        return abort(out, "started up, but would not read back -- %s" % e,
                     "Running, carrying the image that was verified as it was "
                     "written.",
                     ["Close FlashGBX and run: %s %s --check"
                      % (PY_LAUNCHER, me)],
                     EXIT_FAILED)

    # Two readings of the same board in one run once reported different PCB
    # versions. The cause is unknown and device-side -- see the open bug in
    # docs/RELEASE-NOTES.md -- so this reports the disagreement rather than
    # picking a winner. A warn, not a Check: the byte-for-byte comparison
    # below already proves the write landed, and failing here would tell the
    # user to restore a good device over a cosmetic field.
    if fw2.get("pcb_ver") != fw.get("pcb_ver"):
        out.warn("PCB reads %s now and %s before -- one of the two is stale"
                 % (fw2.get("pcb_ver"), fw.get("pcb_ver")))

    reset = struct.unpack("<I", back[4:8])[0]
    checks = [
        Check(back == img,
              "flash matches the image that was written, byte for byte",
              "%s bytes compared" % commas(len(img))),
        Check((reset & ~1) < BOOTINFO_BASE,
              "your device now starts in the bootloader",
              "0x%08X -- %s" % (reset, describe_reset(reset))),
        Check(sum(1 for b in back[REGION_LO:BOOTINFO_BASE] if b != 0) > 0,
              "the bootloader area is no longer empty"),
        Check(back[BOOTINFO_BASE:] == base[BOOTINFO_BASE:],
              "your firmware is identical to your backup",
              "%s bytes compared" % commas(len(base) - BOOTINFO_BASE)),
        Check(fw2.get("fw_ver") == fw.get("fw_ver") and
              fw2.get("fw_ts") == fw.get("fw_ts"),
              "your device reports the same firmware version as before"),
    ]
    bad = report(out, "read-back", checks, "device matches, byte for byte")
    if bad:
        return abort(out, "the device does not match what was written -- %s"
                          % failed_of(bad, checks),
                     "Running and readable. Your backup at %s is untouched, %s."
                     % (pathshow(backup_path),
                        "and still good" if verified
                        else "but it is the one that did not verify above"),
                     [restore_bullet(me, backup_path, verified),
                      "Do not run the install again over the top."]
                     + show_checks_hint(),
                     EXIT_FAILED)

    out.say()
    out.done("Done. Firmware updates now work -- use FlashGBX as normal.")
    if verified:
        out.say("Keep %s to undo this." % pathshow(backup_path))
    else:
        out.say("Keep %s -- it is the only way back, though it did not verify."
                % pathshow(backup_path))
    return EXIT_OK


def default_backup_name():
    return "gbflash-backup-%s.bin" % time.strftime("%Y%m%d-%H%M%S")


def default_composite_name():
    return "gbflash-install-%s.bin" % time.strftime("%Y%m%d-%H%M%S")


# --------------------------------------------------------------------------
# Simulated scenarios for --dry-run.  Each is a device in a state the install
# has to handle; running the real code against them is how the safety logic
# executes without hardware.
# --------------------------------------------------------------------------

SCENARIOS = {
    "empty": "a normal device with no bootloader -- the ordinary install",
    "installed": "a device that already has a bootloader",
    "erased": "a device whose bootloader area has been wiped",
    "isp": "a device already in ISP mode",
    "absent": "nothing plugged in",
    "bad-backup": "a device whose flash comes back corrupted as it is read",
    "no-isp-entry": "a device that never enters ISP mode after the jumper step",
    "isp-fails": "a device where the write itself fails",
    "no-isp-tool": "a computer with no tool that can write to the chip",
    "bad-bootloader": "a damaged bootloader.bin",
    "two-devices": "two things plugged in that could each be a GBFlash",
    "dead": "a device that will not start up -- the one --restore is for",
    "corrupt-boot-region": "a device whose start-up area reads back garbled, "
                           "while its firmware reads back perfectly",
}

# Every scenario except no-isp-tool gets a stand-in for the ISP binary in a dry
# run, so the guided install can be rehearsed end to end on a machine that has
# never had wchisp on it.  The stand-in is never executed: SimHardware
# intercepts run_isp().
SIM_ISP = "(simulated wchisp)"


def make_sim(name, bootloader):
    if name == "absent":
        return SimHardware(mode="absent")
    if name == "isp":
        return SimHardware(mode="isp", flash=SimHardware.stock_flash())
    if name == "installed":
        return SimHardware(mode="app",
                           flash=SimHardware.stock_flash(bootloader=bootloader))
    if name == "erased":
        return SimHardware(mode="app",
                           flash=SimHardware.stock_flash(zero_region=False))
    if name == "bad-backup":
        # One byte of the application payload comes back wrong, so the payload
        # CRC in the boot-info record no longer recomputes.  That is what a
        # dropped byte on the wire looks like, and it must stop the install.
        f = bytearray(SimHardware.stock_flash())
        f[APP_BASE + 0x40] ^= 0xFF
        return SimHardware(mode="app", flash=bytes(f))
    if name == "no-isp-entry":
        return SimHardware(mode="app", flash=SimHardware.stock_flash(),
                           isp_appears=False)
    if name == "isp-fails":
        return SimHardware(mode="app", flash=SimHardware.stock_flash(),
                           isp_rc=1)
    if name == "two-devices":
        return SimHardware(mode="app", flash=SimHardware.stock_flash(),
                           ports=["/dev/sim-gbflash-a", "/dev/sim-gbflash-b"])
    if name == "dead":
        return SimHardware(mode="dead", flash=bytearray())
    if name == "corrupt-boot-region":
        # A word garbled on the wire inside the vector table. The application
        # half still passes every gate it has -- the boot-info CRC covers the
        # application only -- so this is caught by the boot-region gates or it
        # is not caught at all, and the "backup" would restore a dark board.
        f = bytearray(SimHardware.stock_flash())
        f[0x40:0x44] = struct.pack("<I", 0x12345678)
        return SimHardware(mode="app", flash=bytes(f))
    return SimHardware(mode="app", flash=SimHardware.stock_flash())


def corrupt_bootloader(bl):
    """A bootloader image that must not reach flash: vector 2 is zeroed, so it
    has neither the Thumb bit nor a target inside the image."""
    bad = bytearray(bl if bl else b"\x00\x80\x00\x20" + b"\x01" * 0x100)
    bad[8:12] = b"\x00\x00\x00\x00"
    return bytes(bad)


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def build_parser():
    ap = argparse.ArgumentParser(
        prog="install.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Installs the GBFlash bootloader, so firmware updates "
                    "work.",
        epilog="""\
what it does
  Run it with no options and it walks you through the whole thing: it looks
  at your device, backs it up, writes the bootloader, and checks it worked.
  It asks before it changes anything, and the backup means you can always
  put your device back.

the other things it can do
  --check            just tell me whether I need this
  --backup FILE      just save a copy of my device
  --restore FILE     put a saved copy back -- works even if it won't start up
  --dry-run          practise on a pretend device; nothing real is touched

what you need
  pyserial (python3 -m pip install pyserial), and wchisp, which writes to the chip.
  If wchisp is not here, this offers to download it.

The install needs a jumper on H1 for one step. Nothing else does, and
firmware updates afterwards never do.""")

    ap.add_argument("--check", action="store_true",
                    help="just check whether the bootloader is there")
    ap.add_argument("--backup", metavar="FILE",
                    help="just save a copy of your device to FILE, and check "
                         "you could restore from it")
    ap.add_argument("--restore", metavar="FILE",
                    help="write a saved copy back to your device")
    ap.add_argument("--backup-out", metavar="FILE",
                    help="where the install puts its backup (default: "
                         "gbflash-backup-<date>.bin, here)")
    ap.add_argument("--composite", metavar="FILE",
                    help="where to put the image it builds")
    ap.add_argument("--port", metavar="DEV",
                    help="your device's serial port, if it is not found "
                         "automatically")
    ap.add_argument("--bootloader", metavar="FILE",
                    help="where bootloader.bin is, if it is not next to this "
                         "script")
    ap.add_argument("--isp", metavar="PATH",
                    help="the wchisp binary to use, if it is not found "
                         "automatically")
    ap.add_argument("--no-color", action="store_true",
                    help="plain output, no colour (also honours NO_COLOR)")
    ap.add_argument("--quick", action="store_true",
                    help="--check only: take a faster look instead of reading "
                         "the whole bootloader area")
    ap.add_argument("--show-checks", action="store_true",
                    help="show every check, not just the ones that failed")
    ap.add_argument("--verbose", action="store_true",
                    help="show wchisp's own output even when it succeeds")

    ap.add_argument("--dry-run", action="store_true",
                    help="practise against a pretend device; nothing real is "
                         "touched")
    ap.add_argument("--sim", metavar="NAME", default="empty",
                    choices=sorted(SCENARIOS),
                    help="--dry-run only: which pretend device. One of: "
                         + ", ".join(sorted(SCENARIOS)))
    ap.add_argument("--list-sims", action="store_true",
                    help="list the pretend devices and exit")

    ap.add_argument("--yes", action="store_true",
                    help="answer every prompt with its default, for scripts. "
                         "It does not skip a single safety check, and it will "
                         "not download anything.")
    ap.add_argument("--overwrite-existing-bootloader", action="store_true",
                    help="install even though something is already in the "
                         "bootloader area. CONSEQUENCE: whatever is there now "
                         "is erased, and it might be what makes your device "
                         "work.")
    ap.add_argument("--allow-unverified-backup", action="store_true",
                    help="install even though the backup failed its checks. "
                         "CONSEQUENCE: if this goes wrong, the file you are "
                         "relying on may not put your device back. It does not "
                         "relax the checks on the image itself, so a genuinely "
                         "bad read still stops the install.")
    ap.add_argument("--restore-unverified", action="store_true",
                    help="--restore only: write a file that failed its checks. "
                         "CONSEQUENCE: your device may not start up "
                         "afterwards. H1 still works, so you can always write "
                         "a better copy later. On its own this will NOT write "
                         "over a device that is still running -- for that, see "
                         "--restore-over-a-working-device.")
    ap.add_argument("--restore-over-a-working-device", action="store_true",
                    help="--restore only, alongside --restore-unverified: "
                         "write a failing file over a device that is working. "
                         "CONSEQUENCE: the firmware on it right now stops "
                         "existing anywhere. If the reason you want this is "
                         "not having a good backup, take one first with "
                         "--backup; the device is alive, so that still works.")
    return ap


def main(argv=None, out=None, hw=None):
    global SHOW_CHECKS, VERBOSE
    ap = build_parser()
    args = ap.parse_args(argv)
    out = out or Out()
    if args.no_color:
        out.no_colour()
    SHOW_CHECKS = bool(args.show_checks)
    VERBOSE = bool(args.verbose)

    if args.list_sims:
        out.raw("Pretend devices for --dry-run --sim NAME:")
        for k in sorted(SCENARIOS):
            out.raw("  %-20s %s" % (k, SCENARIOS[k]))
        return EXIT_OK

    chosen = [n for n, v in (("--check", args.check),
                             ("--backup", args.backup),
                             ("--restore", args.restore)) if v]
    if len(chosen) > 1:
        ap.error("%s are different modes; pick one." % " and ".join(chosen))

    out.raw(out.b("GBFlash bootloader installer"))
    if SHOW_CHECKS:
        out.dim("python %s on %s %s" % (platform.python_version(),
                                        platform.system(),
                                        platform.machine()))
    out.say()
    tools = locate(args, out)

    tmpdir = None
    if hw is None:
        if args.dry_run:
            bl = None
            if tools.bootloader:
                with open(tools.bootloader, "rb") as f:
                    bl = f.read()
            hw = make_sim(args.sim, bl)
            out.dim("dry run (sim: %s) -- nothing real is touched, and "
                    "every prompt answers itself" % args.sim)
            out.say()
            tmpdir = tempfile.mkdtemp(prefix="gbflash-dryrun-")
            if args.sim == "no-isp-tool":
                # The scenario IS "no ISP tool installed", so it has to hold
                # even on a machine that has wchisp next to this script.
                tools.isp = None
            elif not tools.isp:
                tools.isp = SIM_ISP
            if args.sim == "bad-bootloader" and bl:
                tools.bootloader = os.path.join(tmpdir, "bootloader-bad.bin")
                with open(tools.bootloader, "wb") as f:
                    f.write(corrupt_bootloader(bl))
            # NOTHING the user can see is written in a dry run.  The backup
            # and the composite are real files -- writing them is part of the
            # rehearsal -- but they go into the temporary directory above.
            args.backup_out = os.path.join(tmpdir, "backup.bin")
            args.composite = os.path.join(tmpdir, "install.bin")
            if args.backup:
                args.backup = os.path.join(tmpdir, "backup.bin")
        else:
            hw = RealHardware(assume_yes=args.yes)

    try:
        if args.check:
            rc = cmd_check(hw, out, args, tools)
        elif args.restore:
            rc = cmd_restore(hw, out, args, tools)
        elif args.backup:
            rc = cmd_backup(hw, out, args, tools)
        else:
            rc = cmd_install(hw, out, args, tools)
    except KeyboardInterrupt:
        rc = abort(out, "interrupted",
                   "However the last finished step left it. If the write had "
                   "not started, nothing was written at all.",
                   ["Run this again.",
                    "If the write had started: unplug, clear H1, plug in, and "
                    "see whether it starts up. If it does not, restore your "
                    "backup."], EXIT_FAILED)
    finally:
        if tmpdir:
            out.gap()
            out.dim("dry run over; temporary files removed")
            shutil.rmtree(tmpdir, ignore_errors=True)

    return rc


if __name__ == "__main__":
    sys.exit(main())
