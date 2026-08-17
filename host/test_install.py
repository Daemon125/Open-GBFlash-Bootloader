#!/usr/bin/env python3
"""Offline test suite for install.py -- the guided bootloader installer.

The install path cannot be exercised on hardware from a test suite, and a
guided installer whose safety logic has never executed is not obviously better
than the checklist it replaces.  So install.py keeps everything that touches
the world behind one object (`Hardware`), and this file drives the REAL install
logic against simulated devices in each state that matters:

    application mode, region zero-filled      the normal install
    application mode, bootloader present      must refuse
    application mode, region erased to 0xFF   must refuse
    ROM ISP mode                              must refuse, point at --restore
    nothing connected                         must refuse
    flash that reads back corrupt             must refuse before any write
    no ISP tool installed                     must refuse BEFORE the jumper step
    a corrupt bootloader.bin                  composite must never be written
    the jumper never taking                   must stop, nothing written
    an ISP write that fails                   must stop and say how to recover
    an ISP write that lands wrong             must be caught by the read-back

and, for --restore, a device with nothing in flash at all and no serial port.

WHAT IS AND IS NOT SIMULATED.  Only `Hardware` is: the serial link, the USB
presence probe, the ISP tool, the download, and the human at the prompts.
Everything above it -- every gate, every branch, every refusal, the composite
construction, and the independent cross-check that shells out to
tools/build_composite.py -- is the shipping code, running for real.

NOTHING HERE TOUCHES THE NETWORK.  The wchisp download is exercised against a
stubbed fetch that hands back an archive built in memory, so the unpack, the
chmod and the hand-off to the install all run for real without a single byte
crossing the wire.

    python3 test_install.py            (or: make -C host test)
"""

import atexit
import hashlib
import importlib.util
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INSTALL_PY = os.path.join(ROOT, "install.py")

_spec = importlib.util.spec_from_file_location("gbflash_install", INSTALL_PY)
inst = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(inst)

_msyn = importlib.util.spec_from_file_location(
    "make_synthetic_fw", os.path.join(HERE, "make_synthetic_fw.py"))
msyn = importlib.util.module_from_spec(_msyn)
_msyn.loader.exec_module(msyn)


# --------------------------------------------------------------------------
# Assertions
# --------------------------------------------------------------------------

CHECKS = 0
FAILS = []
_ALL_ISP_CALLS = []


def ck(cond, label, detail=""):
    global CHECKS
    CHECKS += 1
    if cond:
        return True
    FAILS.append(label)
    sys.stdout.write("  FAIL %s%s\n" % (label, ("  [%s]" % detail)
                                        if detail else ""))
    return False


def section(name):
    sys.stdout.write("%s\n" % name)


class Sink(object):
    """A stream Out() can write to without anything reaching the terminal."""

    tty = False
    encoding = "utf-8"

    def __init__(self):
        self.buf = []

    def write(self, s):
        self.buf.append(s)

    def flush(self):
        pass

    def isatty(self):
        return self.tty

    def text(self):
        return "".join(self.buf)


class TtySink(Sink):
    """The same, but claiming to be a terminal, so colour is switched on."""

    tty = True


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------

def fake_bootloader(size=7056):
    """A structurally valid bootloader image.

    The suite must run in a plain checkout on a machine with no ARM toolchain,
    so it does not depend on build/bootloader.bin existing.  The real binary is
    checked separately, when it is there.
    """
    entry = 0xBD                       # inside the image, Thumb bit set below
    vec = [0x20008000] + [entry | 1] * 35
    img = bytearray(struct.pack("<36I", *vec))
    img += bytes(range(256)) * ((size - len(img)) // 256 + 1)
    return bytes(img[:size])


def corrupt_bootloader(bl):
    return inst.corrupt_bootloader(bl)


_SPAWN_BL = []


def spawned_bootloader():
    """A bootloader.bin path for the tests that spawn install.py as a process.

    Those runs used to rely on build/bootloader.bin being in the tree, so the
    whole suite failed in a plain checkout -- which is exactly how ci.yml's
    host-tests job runs it: no cross toolchain, no `make all`, and build/ is
    gitignored. Ten checks failed on every push. The file header promises this
    suite runs "on a machine with no ARM toolchain"; this is what makes that
    true for the subprocess tests as well as the in-process ones.

    cwd stays at ROOT for those spawns, so install.py can still show its own
    path relatively and the no-home-directory-in-output assertions hold.
    """
    if not _SPAWN_BL:
        d = tempfile.mkdtemp(prefix="gbflash-spawn-bl-")
        p = os.path.join(d, "bootloader.bin")
        with open(p, "wb") as f:
            f.write(fake_bootloader())
        _SPAWN_BL.append(p)
        atexit.register(shutil.rmtree, d, True)
    return _SPAWN_BL[0]


def stock(**kw):
    return inst.SimHardware.stock_flash(**kw)


class Env(object):
    """A temporary directory plus the argv every test shares."""

    def __init__(self, bootloader=None, isp="fake-wchisp"):
        self.dir = tempfile.mkdtemp(prefix="gbflash-test-")
        self.bl_path = os.path.join(self.dir, "bootloader.bin")
        with open(self.bl_path, "wb") as f:
            f.write(bootloader if bootloader is not None else fake_bootloader())
        self.backup = os.path.join(self.dir, "backup.bin")
        self.composite = os.path.join(self.dir, "install.bin")
        self.isp = isp

    def argv(self, *extra):
        a = ["--bootloader", self.bl_path,
             "--backup-out", self.backup,
             "--composite", self.composite]
        if self.isp:
            a += ["--isp", self.isp]
        return a + list(extra)

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def run(argv, hw):
    """Run install.py's real main() against a simulated device."""
    sink = Sink()
    out = inst.Out(stream=sink)
    rc = inst.main(argv=list(argv), out=out, hw=hw)
    _ALL_ISP_CALLS.extend(hw.isp_calls)
    return rc, out


# --------------------------------------------------------------------------
# 1. The offline image gates
# --------------------------------------------------------------------------

def test_gates():
    section("image gates")
    good = inst.synth_fw()
    ck(all(c.ok for c in inst.app_gates(good)),
       "a synthetic fw.bin passes every application gate")

    for name, mutate in (
            ("a flipped payload byte fails the payload CRC",
             lambda a: a[:0x400] + bytes([a[0x400] ^ 0xFF]) + a[0x401:]),
            ("a truncated image fails the length gate",
             lambda a: a[:-4]),
            ("a non-SRAM initial SP is refused",
             lambda a: a[:0x200] + struct.pack("<I", 0x08000000) + a[0x204:]),
            ("a reset vector without the Thumb bit is refused",
             lambda a: a[:0x204] + struct.pack("<I", 0x00004090) + a[0x208:]),
            ("a reset vector outside the image is refused",
             lambda a: a[:0x204] + struct.pack("<I", 0x20000001) + a[0x208:]),
            ("a wrong boot-info marker is refused",
             lambda a: struct.pack("<H", 0x1234) + a[2:]),
            ("a corrupted 'LFBG' magic is refused",
             lambda a: a[:2] + b"XXXX" + a[6:]),
            ("dirt in the boot-info page is refused",
             lambda a: a[:0x20] + b"\x00" + a[0x21:]),
    ):
        ck(not all(c.ok for c in inst.app_gates(mutate(good))), name)

    bl = fake_bootloader()
    ck(all(c.ok for c in inst.bootloader_gates(bl)),
       "a structurally valid bootloader passes its gates")
    ck(not all(c.ok for c in inst.bootloader_gates(corrupt_bootloader(bl))),
       "a bootloader with a zeroed vector is refused")
    ck(not all(c.ok for c in inst.bootloader_gates(b"\x00\x80\x00\x20" * 100)),
       "a bootloader whose vectors have no Thumb bit is refused")
    ck(not all(c.ok for c in inst.bootloader_gates(
            fake_bootloader(inst.BOOTINFO_BASE + 4))),
       "a bootloader larger than the 0x3E00 budget is refused")

    full = stock()
    ck(all(c.ok for c in inst.backup_gates(full)),
       "a stock-shaped CodeFlash dump verifies as a backup")
    ck(not all(c.ok for c in inst.backup_gates(full[:20000])),
       "a truncated dump does NOT verify as a backup")
    ck(not all(c.ok for c in inst.backup_gates(full[:inst.BOOTINFO_BASE])),
       "a bootloader-region-only dump does NOT verify as a backup")
    bad = bytearray(full)
    bad[inst.APP_BASE + 0x40] ^= 0xFF
    ck(not all(c.ok for c in inst.backup_gates(bytes(bad))),
       "a dump whose payload CRC does not recompute does NOT verify")

    # An --all dump is the application followed by ~200 KB of erased flash.
    # That is the MORE thorough backup, so it must not be the one rejected.
    ck(all(c.ok for c in inst.backup_gates(
            full + b"\xFF" * 4096)),
       "an --all-shaped dump with an erased tail still verifies")
    ck(not all(c.ok for c in inst.backup_gates(full + b"\x00" * 16)),
       "a dump with NON-erased bytes past the application does not verify")


def test_composite_construction():
    section("composite construction")
    bl = fake_bootloader()
    app = inst.synth_fw()
    img = inst.build_composite(bl, app)
    ck(len(img) == inst.BOOTINFO_BASE + len(app),
       "composite is 0x3E00 + len(app)")
    ck(img[:len(bl)] == bl, "the bootloader lands at 0x0000 unmodified")
    ck(img[len(bl):inst.BOOTINFO_BASE] ==
       b"\xFF" * (inst.BOOTINFO_BASE - len(bl)),
       "the gap to 0x3E00 is erased fill")
    ck(img[inst.BOOTINFO_BASE:] == app,
       "the application lands at 0x3E00 unmodified")
    ck(img[:0xB8] != app[0x200:0x200 + 0xB8],
       "0x0000 is NOT a copy of the application's vector table")
    ck(struct.unpack("<I", img[4:8])[0] == struct.unpack("<I", bl[4:8])[0],
       "the reset vector at 0x0004 is the bootloader's")

    base = stock()
    ck(all(c.ok for c in inst.composite_gates(
        inst.build_composite(bl, base[inst.BOOTINFO_BASE:]), bl,
        base[inst.BOOTINFO_BASE:], base)),
       "a composite built from a device's own dump passes every gate")
    ck(not all(c.ok for c in inst.composite_gates(
        inst.build_composite(bl, inst.synth_fw(0x7514)), bl,
        inst.synth_fw(0x7514), base)),
       "a composite whose application differs from the device is refused")


def test_synth_matches_make_synthetic_fw():
    section("synthetic image builder")
    for n in (0x90, 0x200, 0x204, 0x7514, 0x7520, 0x7600):
        ck(inst.synth_fw(n) == msyn.build(n),
           "install.py synth_fw(0x%X) matches host/make_synthetic_fw.py" % n)


def test_dist_stamps_the_bootloader_digest():
    """`make dist` must produce a payload that vouches for its own binary.

    install.py ships with BL_SHA256 = None so a from-source build is not called
    a forgery, and so the pinned constant cannot fail the release job on a
    different toolchain. That safety is only worth anything if the stamp really
    lands in the staged copy, so this runs the real staging tool.
    """
    section("make dist: the payload is stamped and self-consistent")
    stage = os.path.join(ROOT, "tools", "stage_release.py")
    if not os.path.isfile(stage):
        ck(False, "tools/stage_release.py exists")
        return
    d = tempfile.mkdtemp(prefix="gbflash-dist-")
    try:
        bl = os.path.join(d, "bootloader.bin")
        with open(bl, "wb") as f:
            f.write(fake_bootloader())
        out = os.path.join(d, "dist")
        p = subprocess.run(
            [sys.executable, stage, "--bootloader", bl, "--out", out,
             "--repo-url", "https://example.invalid/o/r"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        ck(p.returncode == 0, "stage_release.py succeeds",
           p.stdout.decode("utf-8", "replace")[-400:])
        if p.returncode != 0:
            return

        want = sorted(["install.py", "requirements.txt", "build_composite.py",
                       "backup-codeflash.py", "check-bootloader-region.py",
                       "LICENSE", "README.txt", "bootloader.bin",
                       "SHA256SUMS"])
        # THE STAGING DIRECTORY IS THE ASSET LIST, and it must hold exactly the
        # zip. Publishing the same nine files loose beside it put thirteen rows
        # on the release page and buried the one download.
        staged = sorted(os.listdir(out))
        ck(staged == ["gbflash-bootloader.zip"],
           "the release publishes one asset and one only", ", ".join(staged))
        ck(all(os.path.isfile(os.path.join(out, n)) for n in staged),
           "and it is a regular file, not a directory release.yml would trip on")

        # The zip is the primary download, so it is what gets unpacked here and
        # checked from now on -- the same bytes a person would end up with.
        zp = os.path.join(out, "gbflash-bootloader.zip")
        ck(os.path.isfile(zp), "the zip was written")
        payload = os.path.join(d, "extracted")
        with zipfile.ZipFile(zp) as z:
            names = sorted(n.split("/", 1)[1] for n in z.namelist())
            ck(names == want, "the zip holds exactly the published set",
               ", ".join(names))
            ck(all(n.startswith("gbflash-bootloader/") for n in z.namelist()),
               "and extracts into a folder rather than over the cwd")
            z.extractall(payload)
        payload = os.path.join(payload, "gbflash-bootloader")

        # The stamp, which is the whole point.
        digest = hashlib.sha256(fake_bootloader()).hexdigest()
        with open(os.path.join(payload, "install.py")) as f:
            staged = f.read()
        ck('BL_SHA256 = "%s"' % digest in staged,
           "the staged install.py carries the staged bootloader's digest")
        with open(os.path.join(ROOT, "install.py")) as f:
            ck("BL_SHA256 = None" in f.read(),
               "and the repository's own copy still carries None")

        # A stamped install.py must actually USE it: load the staged file as a
        # module and run its own discovery against a mismatched binary.
        sp = importlib.util.spec_from_file_location(
            "staged_install", os.path.join(payload, "install.py"))
        staged_mod = importlib.util.module_from_spec(sp)
        sp.loader.exec_module(staged_mod)
        ck(staged_mod.BL_SHA256 == digest,
           "the staged module reports that digest when imported")
        with open(os.path.join(payload, "bootloader.bin"), "r+b") as f:
            f.seek(0x600)
            b = f.read(1)
            f.seek(0x600)
            f.write(bytes([b[0] ^ 0xFF]))
        args = staged_mod.build_parser().parse_args(["--dry-run"])
        t = staged_mod.locate(args, staged_mod.Out(stream=Sink()))
        ck(any("not the one published" in n for n in t.notes),
           "and a swapped bootloader.bin is called out", "; ".join(t.notes))

        # SHA256SUMS IS WHAT A DOWNLOADER RUNS, and it lives INSIDE the zip
        # so it works after extraction. It must therefore name the payload
        # files and not the zip itself -- a row for a file that is not in the
        # folder ends `sha256sum -c SHA256SUMS` with "FAILED open or read" and
        # exit 1, as the first command someone runs before erasing CodeFlash.
        with open(os.path.join(payload, "SHA256SUMS")) as f:
            rows = [l.split() for l in f if l.strip()]
        named = sorted(r[1] for r in rows)
        ck(named == sorted(n for n in want if n != "SHA256SUMS"),
           "the payload's SHA256SUMS names every file beside it, and nothing "
           "else", ", ".join(named))
        bad = [n for h, n in rows
               if hashlib.sha256(
                   open(os.path.join(payload, n), "rb").read()
               ).hexdigest() != h
               # bootloader.bin was mutated above on purpose.
               and n != "bootloader.bin"]
        ck(not bad, "and every row in it is true", ", ".join(bad))

        with open(os.path.join(payload, "README.txt")) as f:
            rt = f.read()
        ck("@REPO_URL@" not in rt, "the README placeholder was substituted")
        ck("https://example.invalid/o/r" in rt, "with the URL passed in")
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_real_bootloader():
    section("the shipping bootloader binary")
    # THE CHECK COUNT MUST NOT DEPEND ON THE ENVIRONMENT.  This ran two ck()s
    # when build/bootloader.bin existed and none when it did not, so the suite
    # reported 521 in a built tree and 519 in a plain checkout -- and
    # check_docs_quote_the_real_tally() cannot police a number that is two
    # different numbers.  So the gates always run, against the real binary when
    # there is one and a synthetic stand-in otherwise.
    path = os.path.join(ROOT, "build", "bootloader.bin")
    real = os.path.isfile(path)
    if real:
        with open(path, "rb") as f:
            data = f.read()
    else:
        data = fake_bootloader()
        sys.stdout.write("  (build/bootloader.bin absent -- gates run against a "
                         "synthetic image; run `make` to check the real one)\n")

    # NEITHER SIZE NOR DIGEST IS ASSERTED, and for the same reason: both are
    # outputs of the compiler, not properties of the source.  Asserting the
    # digest would have failed the release job -- which runs `make all` before
    # `make -C host test` -- on any toolchain but the one that wrote the
    # constant.  Asserting the exact size has precisely the same defect, and
    # `objcopy -O binary` of an -Os link is under no obligation to be 7,056
    # bytes on Ubuntu's gcc.  What actually matters is that it fits the region,
    # which is a property of the linker script and is asserted.
    ck(len(data) <= inst.BOOTINFO_BASE,
       "the bootloader fits the 0x3E00 budget",
       "%s bytes of %s" % (inst.commas(len(data)), inst.commas(inst.BOOTINFO_BASE)))
    ck(all(c.ok for c in inst.bootloader_gates(data)),
       "it passes install.py's bootloader gates")
    if real:
        sys.stdout.write("  build/bootloader.bin  %s bytes  sha256 %s\n"
                         % (inst.commas(len(data)), hashlib.sha256(data).hexdigest()))


# --------------------------------------------------------------------------
# 2. The install path
# --------------------------------------------------------------------------

def test_install_happy():
    section("install: a device that needs the bootloader")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock())
    before = bytes(hw.flash)
    # The cross-check against tools/build_composite.py no longer announces
    # itself in the output -- a user gains nothing from learning there are two
    # builders -- so watch for the subprocess instead.  That is the stronger
    # assertion anyway: it sees the check RUN, not a sentence about it.
    subprocesses = []
    _real_run = hw.run

    def _watched_run(argv, cwd=None):
        subprocesses.append(list(argv))
        return _real_run(argv, cwd=cwd)

    hw.run = _watched_run
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_OK, "install succeeds", "rc=%d" % rc)
    ck(len(hw.isp_calls) == 1, "exactly one ISP command was issued",
       "%d" % len(hw.isp_calls))
    if hw.isp_calls:
        ck(hw.isp_calls[0][1] == "flash",
           "the ISP command is `wchisp flash`")
        ck(hw.isp_calls[0][2] == e.composite,
           "the ISP command was given the composite, not bootloader.bin")
    ck(os.path.isfile(e.backup), "a backup file was written")
    with open(e.backup, "rb") as f:
        backup = f.read()
    ck(backup == before, "the backup is the flash as it was before the install")
    with open(e.composite, "rb") as f:
        comp = f.read()
    ck(bytes(hw.flash) == comp, "the device now holds exactly the composite")
    ck(comp[inst.BOOTINFO_BASE:] == before[inst.BOOTINFO_BASE:],
       "the firmware at and above 0x3E00 is untouched")
    ck(struct.unpack("<I", bytes(hw.flash[4:8]))[0] < inst.BOOTINFO_BASE,
       "the reset vector at 0x0004 now points into the bootloader")
    ck(any("build_composite" in a for argv in subprocesses for a in argv),
       "the independent build_composite.py cross-check really ran")
    # It ran, and a disagreement aborts before any write, so reaching EXIT_OK
    # with a written device is the agreement.
    ck(not out.contains("byte-for-byte identical"),
       "and it said nothing about itself on the way past")
    ck(out.contains("Done. Firmware updates now work"),
       "the run ends by saying so")
    ck(out.contains("--restore %s" % os.path.basename(e.backup)),
       "the closing advice names the exact restore command for this backup")
    e.close()


def test_install_refuses_existing_bootloader():
    section("install: a bootloader is already present")
    e = Env()
    hw = inst.SimHardware(mode="app",
                          flash=stock(bootloader=fake_bootloader()))
    before = bytes(hw.flash)
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_REFUSED, "install refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(bytes(hw.flash) == before, "the device was not modified")
    ck(not os.path.exists(e.composite), "no composite was written")
    ck(out.contains("--overwrite-existing-bootloader"),
       "the refusal names the override flag")
    ck(out.contains("bootloader already present"), "the verdict is reported")
    ck(not os.path.exists(e.backup),
       "it stops at the region check, before spending minutes on a backup")

    # ... and the override really does override.
    e2 = Env()
    hw2 = inst.SimHardware(mode="app",
                           flash=stock(bootloader=fake_bootloader()))
    rc2, out2 = run(e2.argv("--overwrite-existing-bootloader"), hw2)
    ck(rc2 == inst.EXIT_OK, "--overwrite-existing-bootloader installs anyway",
       "rc=%d" % rc2)
    ck(len(hw2.isp_calls) == 1, "the override issues exactly one ISP command")
    e.close()
    e2.close()


def test_install_refuses_erased_region():
    section("install: the region is erased, not zero-filled")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock(zero_region=False))
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_REFUSED, "install refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(out.contains("bootloader area is erased"), "the verdict is reported")
    ck(out.contains("RECOVERY.md"), "the refusal points somewhere useful")
    e.close()


def test_install_no_device():
    section("install: nothing connected")
    e = Env()
    hw = inst.SimHardware(mode="absent")
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_NODEV, "install reports no device", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(not os.path.exists(e.backup), "no backup file was left behind")
    e.close()


def test_install_device_in_isp():
    section("install: the device is already in ROM ISP mode")
    e = Env()
    hw = inst.SimHardware(mode="isp", flash=stock())
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_REFUSED, "install refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(out.contains("--restore"),
       "the refusal points at --restore for a device that will not boot")
    # The raw USB vendor:product id is gone from the output -- it named the
    # device to nobody.  What has to survive is that it SAYS the device is in
    # ISP mode.
    ck(out.contains("in ISP mode"), "it says the device is in ISP mode")
    # AND THAT IT SAYS IT ONCE, AS A REFUSAL.  ISP mode is correct for
    # --restore and wrong here; printing the reassuring "that is correct" line
    # from the shared detection step put a tick beside the very thing the next
    # line refuses over.
    ck(not out.contains("that is correct"),
       "and does not tick it off as correct one line before refusing over it")
    e.close()


class ShortLeftOnSim(inst.SimHardware):
    """The H1 short is still fitted at the power-cycle prompt.

    The ordinary simulator boots whatever it holds; this one comes back in ISP
    mode however good the write was, which is what happens when someone plugs
    in without taking the jumper off.
    """

    def _apply(self, tag):
        inst.SimHardware._apply(self, tag)
        if tag == "power-cycle":
            self.mode = "isp"


def test_install_short_left_on_after_the_write():
    section("install: H1 is still shorted at the power-cycle")
    e = Env()
    hw = ShortLeftOnSim(mode="app", flash=stock())
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_FAILED, "install stops", "rc=%d" % rc)
    ck(len(hw.isp_calls) == 1, "the write itself went through",
       "%d" % len(hw.isp_calls))
    # A JUMPER LEFT ON IS NOT A BAD INSTALL.  Read as "not answering" it sends
    # someone restoring a backup over a device that is fine.
    ck(out.contains("H1 is still shorted"), "it names the actual cause")
    ck(not out.contains("not answering after the install"),
       "and does not call a device it can see unresponsive")
    ck(not out.contains("Board completely dark"),
       "and does not offer the dark-board advice, or the restore it carries")
    e.close()


def test_restore_short_left_on_after_the_write():
    section("--restore: H1 is still shorted at the power-cycle")
    e = Env()
    img = stock()
    path = os.path.join(e.dir, "good-backup.bin")
    with open(path, "wb") as f:
        f.write(img)
    hw = ShortLeftOnSim(mode="isp", flash=bytearray())
    rc, out = run(e.argv("--restore", path), hw)
    ck(rc == inst.EXIT_FAILED, "the restore stops", "rc=%d" % rc)
    ck(bytes(hw.flash) == img, "the image was written all the same")
    ck(out.contains("H1 is still shorted"), "it names the actual cause")
    ck(not out.contains("try another backup"),
       "and does not blame the backup for a jumper")
    # ISP mode is what --restore WANTS at the start, so the tick belongs there
    # and only there.
    ck(out.contains("no LEDs light there; that is correct"),
       "the reassurance still appears where ISP mode is the wanted state")
    e.close()


def test_missing_pyserial_is_not_a_broken_device():
    section("a host with no pyserial: check, backup and install")
    # MissingDependency subclasses LinkError, and only --restore caught it
    # separately. The other three reported a healthy device as one that "would
    # not open", told the user to unplug it, and exited EXIT_NODEV -- sending
    # someone hunting a hardware fault over a missing pip package.
    for label, extra in (("--check", ["--check"]),
                         ("--backup", ["--backup"]),
                         ("install", [])):
        e = Env()
        hw = inst.SimHardware(mode="app", flash=stock(), no_pyserial=True)
        argv = e.argv(*(extra if label != "--backup"
                        else ["--backup", os.path.join(e.dir, "b.bin")]))
        rc, out = run(argv, hw)
        ck(rc == inst.EXIT_REFUSED,
           "%s: a missing dependency is a refusal, not a missing device"
           % label, "rc=%d" % rc)
        ck(out.contains("pyserial, which is not installed"),
           "%s: it names pyserial" % label)
        ck(not out.contains("would not open"),
           "%s: and does not blame the device" % label)
        ck(not out.contains("Unplug and replug"),
           "%s: nor tell the user to unplug a working board" % label)
        # PEP 668 hosts -- Debian 12+, Ubuntu 23.04+, Fedora, Homebrew python
        # -- refuse a bare `pip install`, so the escape hatch has to be there.
        ck(out.contains("externally-managed-environment"),
           "%s: and names the way out on a PEP 668 host" % label)
        ck(not hw.isp_calls, "%s: nothing was written" % label)
        e.close()


class WindowsNoPyserialSim(inst.SimHardware):
    """A healthy device on a computer with no pyserial, seen as Windows sees it.

    RealHardware.list_app_ports() falls back to globbing /dev/cu.usbserial* when
    the pyserial import fails. On Windows that glob cannot match a COM port
    however healthy the board, so the list comes back empty and the run used to
    say "no GBFlash found -- Not on USB at all" on the one platform where the
    fallback is guaranteed useless. SimHardware always returns a port, so the
    suite could not see it.
    """

    def list_app_ports(self):
        self.no_pyserial = True
        return []

    def isp_present(self):
        return None          # what RealHardware answers off Linux/Darwin


def test_no_pyserial_on_windows_is_not_a_missing_device():
    section("no pyserial, and no port names to fall back on (Windows)")
    for label, extra in (("--check", ["--check"]),
                         ("--backup", ["--backup"]),
                         ("install", [])):
        e = Env()
        hw = WindowsNoPyserialSim(mode="app", flash=stock(), no_pyserial=True)
        argv = e.argv(*(extra if label != "--backup"
                        else ["--backup", os.path.join(e.dir, "b.bin")]))
        rc, out = run(argv, hw)
        ck(out.contains("pyserial, which is not installed"),
           "%s: the missing dependency is named" % label)
        ck(not out.contains("Not on USB at all"),
           "%s: and a healthy device is not declared absent" % label)
        ck(rc == inst.EXIT_REFUSED, "%s: reported as a refusal" % label,
           "rc=%d" % rc)
        ck(not hw.isp_calls, "%s: nothing was written" % label)
        e.close()


class HeldPortSim(inst.SimHardware):
    """The device comes back, but something else is holding the port."""

    def open_app(self, port):
        raise inst.LinkError("cannot open %s: [Errno 16] Resource busy\n"
                             "Is FlashGBX still open? Only one process can "
                             "hold the port." % port)


def test_restore_with_the_port_held_is_still_a_success():
    section("--restore: the port is held open afterwards")
    # The ISP verified the write as it was made and the board came back on USB.
    # A port this computer cannot open says nothing about the flash, and
    # exiting non-zero over it invites the user to erase a device that is fine
    # and write it again. docs/RECOVERY.md promises this for both a missing
    # pyserial and a held port; only the first was true.
    e = Env()
    img = stock()
    path = os.path.join(e.dir, "good-backup.bin")
    with open(path, "wb") as f:
        f.write(img)
    hw = HeldPortSim(mode="isp", flash=bytearray())
    rc, out = run(e.argv("--restore", path), hw)
    ck(rc == inst.EXIT_OK,
       "a restore whose read-back cannot run is not called a failure",
       "rc=%d" % rc)
    ck(bytes(hw.flash) == img, "the image really was written")
    ck(out.contains("Do NOT restore again"),
       "and the user is told not to write the board again over it")
    ck(out.contains("Close FlashGBX"), "with the actual cause named")
    e.close()


def test_install_refuses_bad_backup():
    section("install: the backup does not verify")
    e = Env()
    f = bytearray(stock())
    f[inst.APP_BASE + 0x40] ^= 0xFF          # a dropped byte on the wire
    hw = inst.SimHardware(mode="app", flash=bytes(f))
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_REFUSED, "install refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(not os.path.exists(e.composite), "no composite was written")
    ck(out.contains("[ xx ] backup does not verify"),
       "one line says what failed")
    ck(out.contains("Nothing written to it, and nothing will be"),
       "and the stop says what that means for the install")
    ck(out.text().count("backup does not verify") == 1,
       "and says it exactly once -- no headline twice")
    # WHERE it stops matters as much as that it stops.  The composite gates
    # would also refuse this image, so "nothing was written" alone cannot tell
    # a working no-backup-no-install rule from one that has been removed and is
    # being covered for by the next barrier.  It has to stop at step 4.
    ck(not out.contains("install image built"),
       "it stops at the backup, before building anything -- not later by luck")

    # The override lets the run PAST the backup gate -- and the composite gate,
    # which is not overridable, then stops it anyway.  That is the intended
    # arrangement: no flag can put an image the bootloader would reject onto a
    # device.
    e2 = Env()
    hw2 = inst.SimHardware(mode="app", flash=bytes(f))
    rc2, out2 = run(e2.argv("--allow-unverified-backup"), hw2)
    ck(rc2 == inst.EXIT_REFUSED,
       "--allow-unverified-backup still refuses: the composite gate is not "
       "overridable", "rc=%d" % rc2)
    ck(not hw2.isp_calls,
       "no ISP command was issued even with the override")
    ck(not os.path.exists(e2.composite),
       "no composite was written even with the override")
    ck(out2.contains("the install image did not come out right"),
       "the composite gate is the one that stopped it")
    ck(out2.contains("--allow-unverified-backup"),
       "the override really did carry the run past the backup gate")
    # The override means the run CONTINUES, so nothing may print a stop.
    ck(not out2.contains("nothing will be"),
       "and the overridden backup warns rather than claiming a stop that is "
       "not happening")
    e.close()
    e2.close()


def test_unverified_backup_never_claims_a_stop():
    """--allow-unverified-backup writes, so it must not say it will not.

    The flag exists so someone can proceed deliberately.  The refusal block it
    used to print -- "nothing written to it, and nothing will be", "do not skip
    past this" -- was on screen a few lines above the write that then happened.
    The two statements cannot both be true, and this pins that they cannot both
    appear.
    """
    section("--allow-unverified-backup: the words match what happens")
    e = Env()
    # A garbled word in the boot region: the backup gates refuse it (they are
    # the only thing covering 0x0000..0x3DFF), while the firmware half and the
    # composite are untouched -- so with the override the run reaches the write.
    f = bytearray(stock())
    f[0x2000:0x2004] = b"\xA5\xA5\xA5\xA5"
    hw = inst.SimHardware(mode="app", flash=bytes(f))
    rc, out = run(e.argv("--allow-unverified-backup",
                         "--overwrite-existing-bootloader"), hw)
    ck(rc == inst.EXIT_OK, "the install runs to the end", "rc=%d" % rc)
    ck(len(hw.isp_calls) == 1, "and the device really was written")
    ck(out.contains("[ !! ] backup does not verify"),
       "the unverified backup is warned about, not aborted over")
    ck(out.contains("--allow-unverified-backup: the write goes ahead"),
       "and the warning says what is being accepted")
    ck(out.contains("may not put the device back"),
       "and what it costs")
    for lie in ("nothing will be", "Nothing written to it",
                "Do not skip past this", "[ xx ]"):
        ck(not out.contains(lie),
           "a run that writes never says %r" % lie)
    e.close()


class _BitrotAfterWrite(inst.SimHardware):
    """A device that comes back holding one byte other than what was sent.

    Models the read-back mismatch only -- it says nothing about why real flash
    would do that.  It exists to drive what the script SAYS at the one moment
    the backup is the only way back.
    """

    def run_isp(self, isp, image_path):
        rc, text = inst.SimHardware.run_isp(self, isp, image_path)
        if rc == 0 and len(self.flash) > 0x40:
            self.flash[0x40] ^= 0xFF
        return rc, text


def test_unverified_backup_is_never_called_good():
    """A backup that did not verify must not be called good later on.

    The composite-gate abort already says "the one above".  The read-back
    abort said "untouched and still good" unconditionally, and handed out a
    --restore command that --restore itself refuses -- at the exact moment
    recovery matters.
    """
    section("--allow-unverified-backup: the backup is not promoted to good")
    e = Env()
    f = bytearray(stock())
    f[0x2000:0x2004] = b"\xA5\xA5\xA5\xA5"
    hw = _BitrotAfterWrite(mode="app", flash=bytes(f))
    rc, out = run(e.argv("--allow-unverified-backup",
                         "--overwrite-existing-bootloader"), hw)
    ck(rc == inst.EXIT_FAILED, "the read-back mismatch is a failure",
       "rc=%d" % rc)
    ck(out.contains("backup does not verify"),
       "the run did say the backup does not verify")
    ck(out.contains("the device does not match what was written"),
       "and then hit the read-back mismatch")
    ck(not out.contains("still good"),
       "and never calls that same backup good")
    ck(out.contains("the one that did not verify above"),
       "it names it as the one that did not verify")
    ck(out.contains("--restore-unverified"),
       "and offers the flag that will actually write it")

    # The same path with a backup that DID verify keeps the plain wording, so
    # nobody 'fixes' this by hedging every backup.
    e2 = Env()
    hw2 = _BitrotAfterWrite(mode="app", flash=stock())
    rc2, out2 = run(e2.argv(), hw2)
    ck(rc2 == inst.EXIT_FAILED, "a verified backup still fails read-back",
       "rc=%d" % rc2)
    ck(out2.contains("untouched, and still good"),
       "and a verified backup is still called good")
    ck(not out2.contains("--restore-unverified"),
       "with no unverified-restore hint it does not need")
    e.close()
    e2.close()


def test_release_phrases():
    """.github/workflows/release.yml greps the staged installer's output for
    these two phrases.  Pinned here so an output rewrite fails this suite
    rather than the release job."""
    section("phrases the release workflow greps for")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock())
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_OK, "a clean install succeeds", "rc=%d" % rc)
    ck(out.contains("Done. Firmware updates now work"),
       "the success phrase release.yml greps for is printed")

    e2 = Env()
    hw2 = inst.SimHardware(mode="app",
                           flash=inst.SimHardware.stock_flash(
                               bootloader=fake_bootloader()))
    rc2, out2 = run(e2.argv(), hw2)
    ck(rc2 == inst.EXIT_REFUSED, "an installed device is refused",
       "rc=%d" % rc2)
    ck(out2.contains("bootloader already present"),
       "the refusal phrase release.yml greps for is printed")

    wf = os.path.join(ROOT, ".github", "workflows", "release.yml")
    if os.path.exists(wf):
        with open(wf) as f:
            text = f.read()
        ck('grep -q "Done. Firmware updates now work"' in text,
           "release.yml greps for the phrase this test pins")
        ck('grep -q "bootloader already present"' in text,
           "release.yml greps for the refusal phrase this test pins")
    e.close()
    e2.close()


def test_a_bare_enter_is_not_a_yes():
    section("install: pressing Enter at the confirmation")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock(), answers=[""])
    before = bytes(hw.flash)
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_REFUSED, "an empty answer stops the install",
       "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(bytes(hw.flash) == before, "the device was not modified")
    ck(out.contains("not confirmed, so nothing happened"),
       "and it says why it stopped")
    ck(out.contains("Run this again when you are ready"),
       "and says what to do next, without reassurance")
    # The same question, answered properly.
    e2 = Env()
    hw2 = inst.SimHardware(mode="app", flash=stock(),
                           answers=["install", "", ""])
    rc2, _ = run(e2.argv(), hw2)
    ck(rc2 == inst.EXIT_OK, "typing the word does continue", "rc=%d" % rc2)
    e.close()
    e2.close()


def _without_isp_tools(fn):
    """Run fn() on a machine that has no wchisp.

    locate() looks for it beside install.py and in the working directory.  A
    developer machine that has actually flashed a device has one there, and
    then the no-tool scenario cannot happen -- so drop exactly those
    candidates and leave every other lookup alone.
    """
    which, first = inst.shutil.which, inst._first_file
    inst.shutil.which = lambda name: None
    inst._first_file = lambda paths: first(
        [p for p in paths if p and
         os.path.basename(p).lower().split(".")[0] not in inst.ISP_TOOLS])
    try:
        return fn()
    finally:
        inst.shutil.which, inst._first_file = which, first


def test_install_needs_isp_tool_before_the_jumper():
    section("install: no ISP tool installed, and the download declined")
    e = Env(isp=None)
    hw = inst.SimHardware(mode="app", flash=stock(), answers=["n"])
    rc, out = _without_isp_tools(lambda: run(e.argv(), hw))
    ck(rc == inst.EXIT_REFUSED, "install refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck([t for t, _ in hw.asked if t == "download-wchisp"],
       "it offered to download wchisp")
    ck(out.contains("wchisp not found. Download it? (y/N)"),
       "the offer is the one line, and nothing more")
    ck(not [t for t, _ in hw.asked if t == "enter-isp"],
       "the user was NOT asked to fit the jumper first")
    ck(not [t for t, _ in hw.asked if t == "confirm-install"],
       "the user was not asked to confirm a write that could not happen")
    ck(out.contains("ch32-rs/wchisp"),
       "it says where to get wchisp by hand")
    ck(out.contains("[ xx ]"), "answering no stops cleanly, marked as a stop")
    e.close()

    # A bare Enter is not a yes here either.
    e2 = Env(isp=None)
    hw2 = inst.SimHardware(mode="app", flash=stock(), answers=[""])
    rc2, out2 = _without_isp_tools(lambda: run(e2.argv(), hw2))
    ck(rc2 == inst.EXIT_REFUSED, "a bare Enter declines the download too",
       "rc=%d" % rc2)
    ck(not hw2.isp_calls, "and nothing was written")
    e2.close()


def test_install_refuses_bad_bootloader():
    section("install: bootloader.bin is corrupt")
    e = Env(bootloader=corrupt_bootloader(fake_bootloader()))
    hw = inst.SimHardware(mode="app", flash=stock())
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_REFUSED, "install refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(not os.path.exists(e.composite),
       "a composite that would fail validation was never written")
    e.close()


def test_install_jumper_never_takes():
    section("install: 4348:55E0 never appears")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock(), isp_appears=False)
    before = bytes(hw.flash)
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_FAILED, "install stops", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(bytes(hw.flash) == before, "the device was not modified")
    ck(out.contains("No LEDs light in ISP mode"),
       "the abort still states the fact that stops a dark board being read as "
       "a failure -- flatly, with no reassuring tail")
    ck(not out.contains("evidence of failure"),
       "and does not explain it back at the reader")
    ck(os.path.isfile(e.backup), "the backup is still on disk")
    e.close()


def test_install_isp_write_fails():
    section("install: the ISP write fails")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock(), isp_rc=1)
    before = bytes(hw.flash)
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_FAILED, "install stops", "rc=%d" % rc)
    ck(len(hw.isp_calls) == 1, "the write was attempted once")
    ck(bytes(hw.flash) == before, "the simulated flash is unchanged")
    ck(out.contains("Chip is hosed"),
       "wchisp's own words are printed, because there they are the diagnosis")
    ck(out.contains("--restore %s" % os.path.basename(e.backup)),
       "the recovery block names the exact restore command for this backup")
    ck(out.contains("In ISP mode, possibly half-erased"),
       "and says what state the device is in")
    # The stop block is the whole of what a bug report carries, and wchisp puts
    # the chip UID inside it: it identifies the chip and THEN fails.
    ck(inst.SIM_CHIP_UID not in out.text(),
       "and the chip UID wchisp printed is not in the stop block")
    ck(out.contains("Chip UID: (withheld)"),
       "the line survives with its value taken out, so the log still reads")
    e.close()


class MisflashingSim(inst.SimHardware):
    """An ISP write that reports success and lands one byte wrong.

    The point of reading the whole of flash back afterwards is to catch exactly
    this, so it has to be something the suite can produce.
    """

    def run_isp(self, isp, image_path):
        rc, text = inst.SimHardware.run_isp(self, isp, image_path)
        if rc == 0 and len(self.flash) > 0x1000:
            self.flash[0x1000] ^= 0xFF
        return rc, text


def test_install_readback_catches_a_bad_write():
    section("install: the read-back catches a write that landed wrong")
    e = Env()
    hw = MisflashingSim(mode="app", flash=stock())
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_FAILED, "install reports failure", "rc=%d" % rc)
    ck(out.contains("the device does not match what was written"),
       "the read-back comparison ran, and it failed")
    ck(out.contains("of 5 checks failed"),
       "the count of failing checks is the whole of what is said by default")
    ck(not out.contains("bytes compared"),
       "and the byte-level detail stays behind --show-checks")
    ck(out.contains("[ xx ]"), "and the stop carries the failure marker")
    ck(out.contains("--restore"), "the failure explains how to get back")
    e.close()


# --------------------------------------------------------------------------
# 2b. Which ISP tool gets used, and fetching one that is not there
# --------------------------------------------------------------------------

def _locate_with(names, isp=None):
    """Run install.py's tool discovery in a directory holding `names`."""
    d = tempfile.mkdtemp(prefix="gbflash-tools-")
    here, which, cwd = inst.HERE, inst.shutil.which, os.getcwd()
    inst.HERE = d
    inst.shutil.which = lambda n: None
    try:
        for n in names:
            p = os.path.join(d, n)
            with open(p, "wb") as f:
                f.write(b"#!/bin/sh\nexit 0\n")
            os.chmod(p, 0o755)
        os.chdir(d)
        argv = ["--isp", isp] if isp else []
        return inst.locate(inst.build_parser().parse_args(argv),
                           inst.Out(stream=Sink()))
    finally:
        inst.HERE, inst.shutil.which = here, which
        os.chdir(cwd)
        shutil.rmtree(d, ignore_errors=True)


def test_isp_tool_choice():
    """wchisp is the tool, and it is driven one way only.

    isp55e0 support was removed: wchisp ships prebuilt for every platform this
    runs on, so the second option bought nothing but a branch in every message.
    What must NOT have gone with it is the guard on the configuration word.
    """
    section("finding and driving wchisp")
    t = _locate_with(["wchisp"])
    ck(t.isp is not None and os.path.basename(t.isp) == "wchisp",
       "wchisp beside the script is found", str(t.isp))
    t = _locate_with([])
    ck(t.isp is None, "with no wchisp present, no tool is claimed")
    t = _locate_with([], isp="/somewhere/wchisp")
    ck(t.isp == "/somewhere/wchisp", "--isp names the binary to use")
    ck(not hasattr(t, "isp_kind"),
       "and there is no tool-kind to branch on any more")

    ck(inst.isp_argv("W", "i.bin") == ["W", "flash", "i.bin"],
       "wchisp is driven with `flash FILE` -- it erases, verifies and resets")
    ck(inst.ISP_TOOLS == ("wchisp",), "wchisp is the only tool looked for",
       str(inst.ISP_TOOLS))

    for bad in (["wchisp", "config", "set", "x"], ["wchisp", "config"]):
        raised = False
        try:
            inst.isp_argv_guard(bad)
        except RuntimeError:
            raised = True
        ck(raised, "isp_argv_guard refuses wchisp's %r subcommand" % bad[1])


def test_install_uses_wchisp_when_it_is_there():
    section("install: wchisp is the tool that gets used")
    e = Env(isp="/somewhere/wchisp")
    hw = inst.SimHardware(mode="app", flash=stock())
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_OK, "the install completes on wchisp", "rc=%d" % rc)
    ck(len(hw.isp_calls) == 1, "exactly one ISP command was issued")
    if hw.isp_calls:
        ck(hw.isp_calls[0] == ["/somewhere/wchisp", "flash", e.composite],
           "and it is `wchisp flash <composite>`, nothing else",
           " ".join(hw.isp_calls[0]))
    ck(out.contains("[ ok ] wchisp"),
       "it names the tool in one line, with a marker")
    with open(e.composite, "rb") as f:
        ck(bytes(hw.flash) == f.read(),
           "the post-write read-back still applies unchanged")
    e.close()


def _wchisp_archive(zip_=False, name=None):
    """A release archive built in memory.  Nothing here touches the network."""
    payload = b"#!/bin/sh\necho wchisp\n"
    member = name or ("wchisp.exe" if zip_ else "wchisp")
    buf = io.BytesIO()
    if zip_:
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("wchisp-v0.3.0-win-x64/" + member, payload)
    else:
        with tarfile.open(fileobj=buf, mode="w:gz") as t:
            info = tarfile.TarInfo("wchisp-v0.3.0-linux-x64/" + member)
            info.size = len(payload)
            t.addfile(info, io.BytesIO(payload))
    return buf.getvalue(), payload


class Downloader(inst.SimHardware):
    """A device whose host can 'download' -- from memory, never the wire."""

    blob = None
    urls = None

    def fetch(self, url, on_start=None):
        if self.urls is None:
            self.urls = []
        self.urls.append(url)
        if self.blob is None:
            return None, "simulated network failure"
        if on_start is not None:
            on_start(len(self.blob))
        return self.blob, None


def _with_download_dir(fn, asset="wchisp-v0.3.0-linux-x64.tar.gz"):
    """Point HERE at a scratch directory and pin the asset name, so the test
    is the same on every platform and nothing lands in the checkout."""
    d = tempfile.mkdtemp(prefix="gbflash-dl-")
    here, chooser = inst.HERE, inst.wchisp_asset
    inst.HERE = d
    inst.wchisp_asset = lambda: asset
    try:
        return fn(d)
    finally:
        inst.HERE, inst.wchisp_asset = here, chooser
        shutil.rmtree(d, ignore_errors=True)


def test_download_wchisp_when_accepted():
    """Answering yes fetches, unpacks and carries straight on.

    The fetch is stubbed; the unpack, the chmod, the hand-off into the install
    and the ISP command that results are all the shipping code.
    """
    section("install: no ISP tool, and the download accepted")
    e = Env(isp=None)
    blob, payload = _wchisp_archive()

    def go(d):
        hw = Downloader(mode="app", flash=stock(), answers=["y"])
        hw.blob = blob
        rc, out = _without_isp_tools(lambda: run(e.argv(), hw))
        return rc, out, hw, d

    rc, out, hw, d = _with_download_dir(go)
    ck(rc == inst.EXIT_OK, "the install completes on the downloaded tool",
       "rc=%d" % rc)
    ck(hw.urls == ["https://github.com/ch32-rs/wchisp/releases/download/"
                   "v0.3.0/wchisp-v0.3.0-linux-x64.tar.gz"],
       "exactly one URL was fetched, the pinned v0.3.0 release asset",
       str(hw.urls))
    ck(out.contains("fetching https://github.com/ch32-rs/wchisp"),
       "it says what it is fetching")
    ck(out.contains("(%s)" % inst.human_size(len(blob))),
       "and how big it is, on the same line")
    ck(len(hw.isp_calls) == 1 and hw.isp_calls[0][1] == "flash",
       "the downloaded tool is then driven as wchisp")
    e.close()

    # ...and the failures are one line each, not a stack trace.
    e2 = Env(isp=None)

    def go2(d):
        hw2 = Downloader(mode="app", flash=stock(), answers=["y"])
        hw2.blob = None                      # no network
        return _without_isp_tools(lambda: run(e2.argv(), hw2)) + (hw2,)

    rc2, out2, hw2 = _with_download_dir(go2)
    ck(rc2 == inst.EXIT_REFUSED, "a failed download stops cleanly",
       "rc=%d" % rc2)
    ck(not hw2.isp_calls, "and nothing was written")
    ck(out2.contains("download failed -- simulated network failure"),
       "it says what went wrong, in one line")
    e2.close()

    # An archive with no wchisp inside it is refused rather than trusted.
    e3 = Env(isp=None)
    junk, _ = _wchisp_archive(name="README")

    def go3(d):
        hw3 = Downloader(mode="app", flash=stock(), answers=["y"])
        hw3.blob = junk
        return _without_isp_tools(lambda: run(e3.argv(), hw3)) + (hw3,)

    rc3, out3, hw3 = _with_download_dir(go3)
    ck(rc3 == inst.EXIT_REFUSED, "an archive with no binary in it stops",
       "rc=%d" % rc3)
    ck(not hw3.isp_calls, "and nothing was written")
    e3.close()


def test_download_unpacks_both_archive_shapes():
    section("the downloaded archive is unpacked, not extracted by its paths")
    tgz, payload = _wchisp_archive()
    ck(inst.unpack_wchisp(tgz, False, "wchisp") == payload,
       "the tar.gz builds pull out the binary")
    zp, payload = _wchisp_archive(zip_=True)
    ck(inst.unpack_wchisp(zp, True, "wchisp.exe") == payload,
       "the Windows zip does too")
    ck(inst.unpack_wchisp(tgz, False, "nothing-like-this") is None,
       "and a member that is not there comes back as nothing")

    # The v0.3.0 asset names, which the URL is built from.
    for system, machine, want in (
            ("Darwin", "arm64", "wchisp-v0.3.0-macos-arm64.tar.gz"),
            ("Darwin", "x86_64", "wchisp-v0.3.0-macos-x64.tar.gz"),
            ("Linux", "x86_64", "wchisp-v0.3.0-linux-x64.tar.gz"),
            ("Linux", "aarch64", "wchisp-v0.3.0-linux-aarch64.tar.gz"),
            ("Windows", "AMD64", "wchisp-v0.3.0-win-x64.zip"),
            ("Linux", "mips", None),
    ):
        sysf, machf = inst.platform.system, inst.platform.machine
        inst.platform.system = lambda s=system: s
        inst.platform.machine = lambda m=machine: m
        try:
            got = inst.wchisp_asset()
        finally:
            inst.platform.system, inst.platform.machine = sysf, machf
        ck(got == want, "%s %s -> %s" % (system, machine, want), str(got))


# --------------------------------------------------------------------------
# 2c. Colour
# --------------------------------------------------------------------------

def _no_color_env(value):
    """Set or clear NO_COLOR, returning what was there."""
    old = os.environ.get("NO_COLOR")
    if value is None:
        os.environ.pop("NO_COLOR", None)
    else:
        os.environ["NO_COLOR"] = value
    return old


def test_colour():
    """Colour on a terminal, and nowhere else.

    A log someone pastes into a bug report has to be plain text, so the rule
    is checked on a real run rather than on the helper alone.
    """
    section("colour output")
    old = _no_color_env(None)
    try:
        ck(inst.want_colour(TtySink()), "a terminal gets colour")
        ck(not inst.want_colour(Sink()),
           "a pipe or a file does not -- piped output stays plain")
        ck(not inst.want_colour(TtySink(), disable=True),
           "--no-color turns it off on a terminal too")
        _no_color_env("1")
        ck(not inst.want_colour(TtySink()), "NO_COLOR turns it off")
        _no_color_env("")
        ck(not inst.want_colour(TtySink()),
           "NO_COLOR honours the empty string, as no-color.org says")
        _no_color_env(None)

        # The same install, three ways.
        for label, sink, extra, want in (
                ("a terminal", TtySink(), (), True),
                ("a pipe", Sink(), (), False),
                ("--no-color on a terminal", TtySink(), ("--no-color",), False),
        ):
            e = Env()
            hw = inst.SimHardware(mode="app", flash=stock())
            out = inst.Out(stream=sink)
            rc = inst.main(argv=e.argv(*extra), out=out, hw=hw)
            _ALL_ISP_CALLS.extend(hw.isp_calls)
            got = "\033[" in sink.text()
            ck(rc == inst.EXIT_OK, "the install still succeeds on %s" % label,
               "rc=%d" % rc)
            ck(got == want, "%s: colour is %s"
               % (label, "on" if want else "off"))
            ck("\033[" not in out.text(),
               "%s: the collected text is plain either way" % label)
            ck(out.contains("Done. Firmware updates now work"),
               "%s: and the closing line reads the same" % label)
            e.close()

        # NO_COLOR over a whole run, since that is how people actually set it.
        _no_color_env("1")
        e = Env()
        hw = inst.SimHardware(mode="app", flash=stock())
        sink = TtySink()
        rc = inst.main(argv=e.argv(), out=inst.Out(stream=sink), hw=hw)
        _ALL_ISP_CALLS.extend(hw.isp_calls)
        ck("\033[" not in sink.text(),
           "NO_COLOR: not one escape sequence in a whole install")
        e.close()
    finally:
        _no_color_env(old)

    ck(inst.strip_ansi("\033[32ma\033[0mb") == "ab",
       "strip_ansi removes the sequences and nothing else")
    # The markers are ASCII everywhere now, so no terminal can be handed a
    # character it cannot encode partway through a write.
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock())
    _, out = run(e.argv(), hw)
    ck(all(ord(c) < 128 for c in out.text()),
       "a whole install is pure ASCII -- no glyph can fail to encode")
    ck(out.contains("[ ok ]"), "and the ok marker is the bracketed pair")
    e.close()


# --------------------------------------------------------------------------
# 3. --check and --backup
# --------------------------------------------------------------------------

def test_check_mode():
    section("--check")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock())
    before = bytes(hw.flash)
    rc, out = run(e.argv("--check"), hw)
    ck(rc == inst.EXIT_OK, "--check succeeds on an empty region", "rc=%d" % rc)
    ck(not hw.isp_calls, "--check issues no ISP command")
    ck(bytes(hw.flash) == before, "--check writes nothing to the device")
    ck(not os.path.exists(e.backup) and not os.path.exists(e.composite),
       "--check writes no files")
    ck(out.contains("no bootloader"), "the verdict is reported")

    hw2 = inst.SimHardware(mode="app", flash=stock(bootloader=fake_bootloader()))
    rc2, out2 = run(e.argv("--check"), hw2)
    ck(rc2 == inst.EXIT_OK and out2.contains("bootloader already present"),
       "--check recognises an installed bootloader")

    hw3 = inst.SimHardware(mode="absent")
    rc3, _ = run(e.argv("--check"), hw3)
    ck(rc3 == inst.EXIT_NODEV, "--check reports no device", "rc=%d" % rc3)
    e.close()


def test_backup_mode():
    section("--backup")
    e = Env()
    path = os.path.join(e.dir, "explicit-backup.bin")
    hw = inst.SimHardware(mode="app", flash=stock())
    rc, out = run(e.argv("--backup", path), hw)
    ck(rc == inst.EXIT_OK, "--backup succeeds", "rc=%d" % rc)
    ck(not hw.isp_calls, "--backup issues no ISP command")
    ck(os.path.isfile(path), "the named file was written")
    with open(path, "rb") as f:
        got = f.read()
    ck(got == bytes(hw.flash), "the backup is the device's flash, exactly")
    ck(all(c.ok for c in inst.backup_gates(got)),
       "the backup passes the restore gates")

    f = bytearray(stock())
    f[inst.APP_BASE + 0x80] ^= 0xFF
    hw2 = inst.SimHardware(mode="app", flash=bytes(f))
    p2 = os.path.join(e.dir, "bad-backup.bin")
    rc2, out2 = run(e.argv("--backup", p2), hw2)
    ck(rc2 == inst.EXIT_REFUSED, "--backup fails loudly on a corrupt read",
       "rc=%d" % rc2)
    ck(out2.contains("so it is not mistaken for a real backup later"),
       "and says what to do with the unusable file")
    e.close()


# --------------------------------------------------------------------------
# 4. --restore, the path that has to work on a dead device
# --------------------------------------------------------------------------

class NoSerialSim(inst.SimHardware):
    """A device that will not boot: no serial port, ever.

    open_app() is a hard error rather than a refusal, so a restore that reached
    for the application at any point would fail the test rather than quietly
    degrade.
    """

    def list_app_ports(self):
        return []

    def open_app(self, port):
        raise AssertionError("--restore must never open a serial port")


def test_restore_on_a_dead_device():
    section("--restore: a device with nothing in flash and no serial port")
    e = Env()
    img = stock()
    path = os.path.join(e.dir, "good-backup.bin")
    with open(path, "wb") as f:
        f.write(img)
    hw = NoSerialSim(mode="absent", flash=bytearray())
    rc, out = run(e.argv("--restore", path), hw)
    ck(len(hw.isp_calls) == 1, "exactly one ISP command was issued",
       "%d" % len(hw.isp_calls))
    if hw.isp_calls:
        ck(hw.isp_calls[0][2] == path, "the image written is the file named")
    ck(bytes(hw.flash) == img, "the device now holds the backup image")
    ck(hw.serial_opens == 0, "no serial port was opened at any point")
    ck(out.contains("The H1 jumper is needed for the write"),
       "the jumper is named before the confirmation, on a dead device too")
    # This device never presents a serial port, even after the restore, so the
    # run correctly ends by saying the write landed and the device still is not
    # answering -- and by explaining both ways that can happen.
    ck(rc == inst.EXIT_FAILED,
       "a device that still does not answer afterwards is reported, not "
       "declared a success", "rc=%d" % rc)
    ck(out.contains("blinking fast and continuously"),
       "the report distinguishes a bootloader in update mode from a dark board")
    ck(out.contains("docs/RECOVERY.md"),
       "and points at the recovery document")
    e.close()


def test_restore_healthy_device():
    section("--restore: a device that comes back afterwards")
    e = Env()
    img = stock()
    path = os.path.join(e.dir, "good-backup.bin")
    with open(path, "wb") as f:
        f.write(img)
    hw = inst.SimHardware(mode="absent", flash=bytearray())
    rc, out = run(e.argv("--restore", path), hw)
    ck(rc == inst.EXIT_OK, "restore succeeds", "rc=%d" % rc)
    ck(out.contains("Restored. Device is running again"),
       "it says so plainly")
    e.close()


def test_restore_refuses_unverified():
    section("--restore: an image that does not verify")
    e = Env()
    path = os.path.join(e.dir, "truncated.bin")
    with open(path, "wb") as f:
        f.write(stock()[:20000])
    hw = inst.SimHardware(mode="absent", flash=bytearray())
    rc, out = run(e.argv("--restore", path), hw)
    ck(rc == inst.EXIT_REFUSED, "restore refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(out.contains("--restore-unverified"),
       "the refusal names the override and states its consequence")

    hw2 = inst.SimHardware(mode="absent", flash=bytearray())
    rc2, out2 = run(e.argv("--restore", path, "--restore-unverified"), hw2)
    ck(len(hw2.isp_calls) == 1,
       "--restore-unverified writes it anyway, for a device already dead")
    ck(out2.contains("H1 will still work afterwards"),
       "and says the recovery path survives")

    missing = os.path.join(e.dir, "does-not-exist.bin")
    hw3 = inst.SimHardware(mode="absent", flash=bytearray())
    rc3, out3 = run(e.argv("--restore", missing), hw3)
    ck(rc3 == inst.EXIT_USAGE, "a missing restore file is a usage error",
       "rc=%d" % rc3)
    ck(not hw3.isp_calls, "and nothing was written")
    e.close()


def test_restore_needs_no_pyserial():
    """--restore must not depend on pyserial being installed.

    Proved by running it in a subprocess with a `serial` module on the path
    that raises on import.  install.py catches ImportError when it genuinely
    needs pyserial, so the stand-in raises RuntimeError instead: if the restore
    path imports it at all, the run dies rather than degrading quietly.
    """
    section("--restore: independent of pyserial")
    d = tempfile.mkdtemp(prefix="gbflash-noserial-")
    try:
        with open(os.path.join(d, "serial.py"), "w") as f:
            f.write('raise RuntimeError("pyserial must not be imported by '
                    'the restore path")\n')
        img = os.path.join(d, "backup.bin")
        with open(img, "wb") as f:
            f.write(stock())
        env = dict(os.environ)
        env["PYTHONPATH"] = d + os.pathsep + env.get("PYTHONPATH", "")
        p = subprocess.run(
            [sys.executable, INSTALL_PY, "--dry-run", "--sim", "absent",
             "--restore", img],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=d)
        text = p.stdout.decode("utf-8", "replace")
        ck(p.returncode == inst.EXIT_OK,
           "--restore completes with pyserial unimportable",
           "rc=%d" % p.returncode)
        ck("must not be imported" not in text,
           "pyserial was never imported")
    finally:
        shutil.rmtree(d, ignore_errors=True)


# --------------------------------------------------------------------------
# 5. The boot region: flash 0x0000..0x3DFF
#
# These 15,872 bytes are 34% of a CodeFlash image and they are the half the
# part actually starts executing -- SP from 0x0000, PC from 0x0004, before any
# code runs.  Nothing else covers them: the boot-info record's CRC spans the
# application only.  They went unchecked once, and the consequence was that
# `--restore` would write a backup whose boot region was uniform garbage,
# report "16 checks, 0 failures", and leave a dark board.  Every mutation below
# is one that used to pass.
# --------------------------------------------------------------------------

def _mut(image, off, data):
    b = bytearray(image)
    b[off:off + len(data)] = data
    return bytes(b)


def test_boot_region_gates():
    section("the boot region: flash 0x0000..0x3DFF")
    bl = fake_bootloader()
    good = stock()
    withbl = stock(bootloader=bl)

    ck(all(c.ok for c in inst.backup_gates(good)),
       "the factory zero-filled layout verifies")
    ck(all(c.ok for c in inst.backup_gates(withbl)),
       "an image with a bootloader installed verifies")

    for label, img in (
            ("a zeroed reset vector at 0x0004 is refused",
             _mut(good, 4, b"\x00\x00\x00\x00")),
            ("a garbage reset vector at 0x0004 is refused",
             _mut(good, 4, struct.pack("<I", 0xDEADBEEF))),
            ("a reset vector without the Thumb bit is refused",
             _mut(good, 4, struct.pack("<I", 0x00004090))),
            ("a zeroed initial SP at 0x0000 is refused",
             _mut(good, 0, b"\x00\x00\x00\x00")),
            ("a non-SRAM initial SP is refused",
             _mut(good, 0, struct.pack("<I", 0x08000000))),
            ("one garbled word in the vector table is refused",
             _mut(good, 0x40, struct.pack("<I", 0x12345678))),
            ("a zeroed 0x0000..0x00B7 is refused",
             _mut(good, 0, b"\x00" * 0xB8)),
            ("512 bytes of dirt at 0x1000 is refused",
             _mut(good, 0x1000, b"\xA5" * 0x200)),
            ("a boot region of uniform garbage is refused",
             _mut(good, 0, b"\xA5" * inst.BOOTINFO_BASE)),
            ("a wholly erased boot region is refused -- nothing boots from it",
             _mut(good, 0, b"\xFF" * inst.BOOTINFO_BASE)),
            ("a bootloader with a zeroed vector is refused",
             _mut(withbl, 8, b"\x00\x00\x00\x00")),
            ("a bootloader whose erased fill has dirt in it is refused",
             _mut(withbl, inst.BOOTINFO_BASE - 8, b"\x5A" * 4)),
    ):
        bad = [c.label for c in inst.backup_gates(img) if not c.ok]
        ck(bool(bad), label)

    # The application half of every mutation above is untouched and perfect.
    # If the app gates were the only ones running, all of them would pass --
    # which is exactly the hole these tests exist to keep shut.
    ck(all(c.ok for c in inst.app_gates(
        inst.trim_erased_tail(_mut(good, 0, b"\xA5" * inst.BOOTINFO_BASE)
                              [inst.BOOTINFO_BASE:])[0])),
       "...and the application half of those images is perfect, so only the "
       "boot-region gates can be catching them")

    ck(inst.classify_low(good)["shape"] == "stock",
       "the factory layout is classified as 'stock'")
    ck(inst.classify_low(withbl)["shape"] == "bootloader",
       "an installed bootloader is classified as 'bootloader'")
    ck(inst.classify_low(b"\xFF" * inst.BOOTINFO_BASE)["shape"] == "erased",
       "an erased region is classified as 'erased'")
    ck(inst.classify_low(b"\xA5" * inst.BOOTINFO_BASE)["shape"]
       == "unrecognised", "uniform garbage is classified as 'unrecognised'")

    for v, want in ((0x00000000, "faults at reset"),
                    (0xFFFFFFFF, "faults at reset"),
                    (0x00004090, "faults at reset"),
                    (0x000000BD, "bootloader is installed"),
                    (0x00004091, "no bootloader")):
        ck(want in inst.describe_reset(v),
           "describe_reset(0x%08X) says %r" % (v, want),
           inst.describe_reset(v))


def test_sim_boot_model_is_strict():
    """The simulator must not boot images a real part would not.

    This is load-bearing for every other test in the file: a simulator that
    comes up on a HardFaulting image reports success for exactly the writes the
    gates exist to prevent, and the suite cannot then tell a working guard from
    a removed one.
    """
    section("the simulated device's boot model")
    hw = inst.SimHardware(mode="app", flash=bytearray(stock()))
    ck(hw._boots(), "the factory layout boots")
    hw.flash = bytearray(stock(bootloader=fake_bootloader()))
    ck(hw._boots(), "an installed bootloader with a good application boots")

    for label, img in (
            ("uniform garbage does not boot",
             _mut(stock(), 0, b"\xA5" * inst.BOOTINFO_BASE)),
            ("a zeroed initial SP does not boot",
             _mut(stock(), 0, b"\x00\x00\x00\x00")),
            ("a reset vector without the Thumb bit does not boot",
             _mut(stock(), 4, struct.pack("<I", 0x00004090))),
            ("a reset vector past the end of the application does not boot",
             _mut(stock(), 4, struct.pack("<I", 0x0003E7FF))),
            ("an erased boot region does not boot",
             _mut(stock(), 0, b"\xFF" * inst.BOOTINFO_BASE)),
    ):
        hw.flash = bytearray(img)
        ck(not hw._boots(), label)


def test_install_refuses_a_corrupt_boot_region():
    section("install: the boot region reads back garbled")
    e = Env()
    f = bytearray(stock())
    f[0x40:0x44] = struct.pack("<I", 0x12345678)   # one word lost on the wire
    hw = inst.SimHardware(mode="app", flash=bytes(f))
    before = bytes(hw.flash)
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_REFUSED, "the install refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(bytes(hw.flash) == before, "the device was not modified")
    ck(out.contains("[ xx ] backup does not verify"),
       "it refuses at the backup, naming the reason")
    ck(not out.contains("Step 5 of 7"),
       "it stops at step 4 -- not later, and not by luck")
    e.close()


def test_backup_file_is_read_back():
    """The gates must run on the FILE, not on a copy in memory.

    A short write leaves a file that differs from what the device said, and
    that file is what --restore would later hand to the ISP.  Checking the
    in-memory dump proves nothing about it.
    """
    section("the backup file is read back before it is trusted")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=bytearray(stock()))
    before = bytes(hw.flash)

    real_open = open

    class Lossy(object):
        """Silently drops one 4-byte word on its way to disk."""

        def __init__(self, f):
            self.f = f
            self.n = 0

        def write(self, b):
            self.n += 1
            return len(b) if self.n == 5 else self.f.write(b)

        def flush(self):
            self.f.flush()

        def __enter__(self):
            return self

        def __exit__(self, *a):
            self.f.close()

    def patched(path, mode="r", *a, **kw):
        f = real_open(path, mode, *a, **kw)
        return Lossy(f) if (mode == "wb" and path == e.backup) else f

    import builtins
    builtins.open = patched
    try:
        rc, out = run(e.argv(), hw)
    finally:
        builtins.open = real_open

    ck(rc == inst.EXIT_REFUSED, "the install refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(bytes(hw.flash) == before, "the device was not modified")
    ck(os.path.getsize(e.backup) < len(before),
       "the file on disk really is short",
       "%d vs %d" % (os.path.getsize(e.backup), len(before)))
    ck(out.contains("backup does not verify"),
       "the file/device comparison is what caught it")
    ck(out.contains("--show-checks"),
       "and the run says where to see which check failed")
    ck(not out.contains("Step 5 of 7"),
       "it stops at step 4, before the composite cross-check could cover for "
       "the missing read-back")
    e.close()


def test_two_devices_are_refused():
    """One board is read and a different board can be the one written.

    The ISP write goes to whichever device answers as 4348:55E0 and cannot be
    aimed, so two candidates is a refusal in every mode, not a coin toss.
    """
    section("two devices that could each be a GBFlash")
    ports = ["/dev/sim-a", "/dev/sim-b"]
    for label, extra, want in (
            ("the install refuses", (), inst.EXIT_REFUSED),
            ("--check refuses", ("--check",), inst.EXIT_REFUSED),
    ):
        e = Env()
        hw = inst.SimHardware(mode="app", flash=bytearray(stock()),
                              ports=ports)
        before = bytes(hw.flash)
        rc, out = run(e.argv(*extra), hw)
        ck(rc == want, label, "rc=%d" % rc)
        ck(not hw.isp_calls, "%s -- and nothing was written" % label)
        ck(bytes(hw.flash) == before, "%s -- the device is unmodified" % label)
        ck(out.contains("--port /dev/sim-a"),
           "%s -- and it says how to name one" % label)
        e.close()

    e = Env()
    path = os.path.join(e.dir, "good.bin")
    with open(path, "wb") as f:
        f.write(stock())
    hw = inst.SimHardware(mode="app", flash=bytearray(stock()), ports=ports)
    rc, out = run(e.argv("--restore", path), hw)
    ck(rc == inst.EXIT_REFUSED, "--restore refuses too", "rc=%d" % rc)
    ck(not hw.isp_calls, "--restore wrote nothing")

    # ...and --port resolves it, so the refusal is not a dead end.
    e2 = Env()
    hw2 = inst.SimHardware(mode="app", flash=bytearray(stock()), ports=ports)
    rc2, out2 = run(e2.argv("--port", "/dev/sim-a"), hw2)
    ck(rc2 == inst.EXIT_OK, "--port /dev/sim-a resolves it and the install "
       "completes", "rc=%d" % rc2)
    ck(len(hw2.isp_calls) == 1, "exactly one ISP command was issued")
    e.close()
    e2.close()


def test_restore_refuses_over_a_working_device():
    """--restore-unverified justifies itself on "the device is already dead".

    When the device is answering on a serial port that argument is false, and
    writing a known-broken image over it destroys the only good copy of the
    firmware that exists -- which the person reaching for the flag, by
    definition, does not have another of.
    """
    section("--restore: a failing image over a device that is still running")
    e = Env()
    bad = os.path.join(e.dir, "broken.bin")
    with open(bad, "wb") as f:
        f.write(b"\x00" * 20000)

    hw = inst.SimHardware(mode="app", flash=bytearray(stock()))
    before = bytes(hw.flash)
    rc, out = run(e.argv("--restore", bad, "--restore-unverified"), hw)
    ck(rc == inst.EXIT_REFUSED, "it refuses", "rc=%d" % rc)
    ck(not hw.isp_calls, "no ISP command was issued")
    ck(bytes(hw.flash) == before, "the working device was not touched")
    ck(hw._boots(), "and it still boots")
    ck(out.contains("--backup my-device.bin"),
       "it tells the user to back up the live device first")
    ck(out.contains("--restore-over-a-working-device"),
       "it names the flag that would override it")

    # The override exists and works -- a refusal with no way past it would just
    # move the problem to a text editor.
    hw2 = inst.SimHardware(mode="app", flash=bytearray(stock()))
    rc2, out2 = run(e.argv("--restore", bad, "--restore-unverified",
                           "--restore-over-a-working-device"), hw2)
    ck(len(hw2.isp_calls) == 1,
       "--restore-over-a-working-device writes it after all")
    ck(out2.contains("stop existing anywhere"),
       "and says plainly what is being destroyed")

    # A GOOD image over a running device is not the dangerous case, and is
    # allowed -- it is how someone rolls a device back to an earlier backup.
    good = os.path.join(e.dir, "good.bin")
    with open(good, "wb") as f:
        f.write(stock())
    hw3 = inst.SimHardware(mode="app", flash=bytearray(stock()))
    rc3, out3 = run(e.argv("--restore", good), hw3)
    ck(rc3 == inst.EXIT_OK, "a verified image over a running device is fine",
       "rc=%d" % rc3)
    ck(len(hw3.isp_calls) == 1, "and it is written")
    e.close()


def test_restore_reports_success_without_pyserial():
    """A rescue that worked must not be reported as a failure.

    The write needs no pyserial; only the confirmation afterwards does.  When
    the confirmation cannot run, the correct answer is "restored, could not
    double-check from here" -- NOT "the restore failed, try another backup",
    which sends someone who has just saved their board back to erase it again.
    """
    section("--restore: a host with no pyserial")
    e = Env()
    img = stock()
    path = os.path.join(e.dir, "good.bin")
    with open(path, "wb") as f:
        f.write(img)
    hw = inst.SimHardware(mode="absent", flash=bytearray(), no_pyserial=True)
    rc, out = run(e.argv("--restore", path), hw)
    ck(rc == inst.EXIT_OK, "the restore reports success", "rc=%d" % rc)
    ck(bytes(hw.flash) == img, "the device holds the image, byte for byte")
    ck(out.contains("Restored. Device is running again"),
       "it says so plainly")
    ck(out.contains("pip install pyserial"),
       "it names the missing package rather than blaming the device")
    ck(not out.contains("Try another backup"),
       "it does NOT advise erasing the board again")
    ck(not out.contains("the board is completely dark"),
       "and does not suggest the image was bad")

    # When pyserial IS available the read-back runs and compares.
    hw2 = inst.SimHardware(mode="absent", flash=bytearray())
    rc2, out2 = run(e.argv("--restore", path), hw2)
    ck(rc2 == inst.EXIT_OK, "with pyserial it also succeeds", "rc=%d" % rc2)
    ck(out2.contains("device matches the file, byte for byte"),
       "and it reads the whole image back and compares it")

    # ...and that read-back is not decorative: a device holding something else
    # must be reported.
    class Wrong(inst.SimHardware):
        def run_isp(self, isp, image_path):
            rc, text = inst.SimHardware.run_isp(self, isp, image_path)
            if rc == 0 and len(self.flash) > 0x5000:
                self.flash[0x5000] ^= 0xFF     # one byte lands wrong
            return rc, text

    hw3 = Wrong(mode="absent", flash=bytearray())
    rc3, out3 = run(e.argv("--restore", path), hw3)
    ck(rc3 == inst.EXIT_FAILED,
       "a restore that lands wrong is caught by the read-back", "rc=%d" % rc3)
    ck(out3.contains("came back holding something other than the file that "
                     "was restored"),
       "and is reported as what it is")
    e.close()


# --------------------------------------------------------------------------
# 5b. The PCB version, which one real run reported two different values for
# --------------------------------------------------------------------------

def _pcb_readings(out):
    return re.findall(r"PCB (\d+)", out.text())


class StubDev(object):
    """A serial port that answers QUERY_FW_INFO with a scripted pcb_ver.

    Enough of pyserial's surface for SerialLink to drive, and no more -- this
    exercises the real reply parsing and the real confirm loop.
    """

    def __init__(self, pcbs):
        self.pcbs = list(pcbs)
        self.pending = b""
        self.queries = 0

    def reset_input_buffer(self):
        self.pending = b""

    reset_output_buffer = reset_input_buffer

    def flush(self):
        pass

    def write(self, data):
        if data[:1] != bytes([inst.QUERY_FW_INFO]):
            raise AssertionError("only QUERY_FW_INFO is expected here")
        pcb = self.pcbs[min(self.queries, len(self.pcbs) - 1)]
        self.queries += 1
        name = b"GBFlash"
        self.pending = (struct.pack(">B", 8)
                        + struct.pack(">cHBI", b"L", 15, pcb, 1780000000)
                        + bytes([len(name)]) + name + b"\x01\x01")

    def read(self, n):
        take, self.pending = self.pending[:n], self.pending[n:]
        return take


def _stub_link(pcbs):
    link = object.__new__(inst.SerialLink)
    link.port = "/dev/stub"
    link.dev = StubDev(pcbs)
    return link


def test_query_fw_decodes_the_documented_offsets():
    """The QUERY_FW_INFO decode, pinned field by field at the wire.

    Layout as install.py's query_fw() documents it, and the same decode as the known-good
    gbflash-tools/gbflash_info.py: u8 size, u8 cfw_id, u16be fw_ver, u8
    pcb_ver, u32be fw_ts, then u8 name_len, the name, caps1, caps2.

    An off-by-one anywhere would slide pcb_ver onto an adjacent byte -- one of
    the things that could have printed two PCB numbers in one run.  This is
    what rules that out, and what would fail if the offsets ever drifted.
    """
    section("QUERY_FW_INFO is decoded at the documented offsets")
    link = _stub_link([13])
    fw = link.query_fw()
    ck(fw["cfw_id"] == "L", "cfw_id is byte 0 of the info block", fw["cfw_id"])
    ck(fw["fw_ver"] == 15, "fw_ver is bytes 1..2, big-endian",
       str(fw["fw_ver"]))
    ck(fw["pcb_ver"] == 13, "pcb_ver is byte 3", str(fw["pcb_ver"]))
    ck(fw["fw_ts"] == 1780000000, "fw_ts is bytes 4..7, big-endian",
       str(fw["fw_ts"]))
    ck(fw["pcb_name"] == "GBFlash", "the name follows, length-prefixed",
       str(fw["pcb_name"]))
    ck(link.dev.queries == 1, "one reply is one exchange",
       str(link.dev.queries))
    ck(link.dev.pending == b"", "and the port is left empty behind it",
       link.dev.pending.hex())

    # 12 and 13 are adjacent, so a v1.2 reply has to decode as itself.
    ck(_stub_link([12]).query_fw()["pcb_ver"] == 12, "a v1.2 reply reads 12")


class TwoFacedBoard(inst.SimHardware):
    """A device that answers PCB 13 before the write and PCB 12 after it.

    This is the OBSERVATION reproduced, not a cause: nothing here claims to
    know why a real board did it.  What it drives is install.py's response.
    """

    def _apply(self, tag):
        inst.SimHardware._apply(self, tag)
        if tag == "power-cycle":
            self.fw = dict(self.fw, pcb_ver=12)


def test_pcb_readings_are_reported_not_reconciled():
    """A board that reports two PCB versions in one run must be said so about.

    The cause is unknown and device-side (see docs/RELEASE-NOTES.md).  So the
    contract is: print what the device said, both times, and flag the
    disagreement -- never quietly pick one, and never fail the install over a
    field that has no bearing on whether the write landed.
    """
    section("a PCB reading that changes mid-run is reported")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock())
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_OK, "a consistent board installs", "rc=%d" % rc)
    ck(len(_pcb_readings(out)) >= 2 and set(_pcb_readings(out)) == {"13"},
       "and reports the same PCB version every time", str(_pcb_readings(out)))
    ck(not out.contains("one of the two is stale"),
       "with nothing said about a mismatch that did not happen")
    e.close()

    e2 = Env()
    hw2 = TwoFacedBoard(mode="app", flash=stock())
    rc2, out2 = run(e2.argv(), hw2)
    ck(rc2 == inst.EXIT_OK,
       "a board that changes its answer still installs -- pcb_ver says "
       "nothing about whether the write landed", "rc=%d" % rc2)
    ck(_pcb_readings(out2) == ["13", "12"],
       "both readings are printed as the device gave them",
       str(_pcb_readings(out2)))
    ck(out2.contains("PCB reads 12 now and 13 before"),
       "and the disagreement is named, not smoothed over")
    e2.close()

    # 12 is a real revision, so it must never be treated as a stale reading.
    e3 = Env()
    hw3 = inst.SimHardware(mode="app", flash=stock(),
                           fw={"cfw_id": "L", "fw_ver": 15, "pcb_ver": 12,
                               "fw_ts": 1780000000, "pcb_name": "GBFlash"})
    rc3, out3 = run(e3.argv(), hw3)
    ck(rc3 == inst.EXIT_OK, "a v1.2 board installs too", "rc=%d" % rc3)
    ck(_pcb_readings(out3) and set(_pcb_readings(out3)) == {"12"},
       "and reports PCB 12 throughout", str(_pcb_readings(out3)))
    ck(not out3.contains("one of the two is stale"),
       "12 is not treated as a stale value")
    e3.close()


# --------------------------------------------------------------------------
# 5c. wchisp's own chatter
# --------------------------------------------------------------------------

def test_wchisp_output_is_captured_unless_it_matters():
    """wchisp narrates ~35 lines, one of them the chip UID.

    None of it is the user's business while it is working.  When it FAILS it is
    the only diagnostic there is, so then it is printed in full.
    """
    section("wchisp's log is captured on success, printed on failure")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock())
    rc, out = run(e.argv(), hw)
    ck(rc == inst.EXIT_OK, "the install succeeds", "rc=%d" % rc)
    ck(out.contains("written and verified"),
       "the write is reported in one line")
    for noise in ("INFO", "Chip: CH579", "Erased code flash", "Verify OK",
                  "config registers"):
        ck(not out.contains(noise),
           "wchisp's %r line is not echoed on a successful run" % noise)
    ck("UID" not in out.text(), "and no chip UID is printed")
    e.close()

    # --verbose asks for it, and gets it -- minus the UID.
    e2 = Env()
    hw2 = inst.SimHardware(mode="app", flash=stock())
    rc2, out2 = run(e2.argv("--verbose"), hw2)
    ck(rc2 == inst.EXIT_OK, "--verbose still succeeds", "rc=%d" % rc2)
    ck(out2.contains("Erased code flash"),
       "--verbose shows wchisp's output on a successful run")
    ck(inst.SIM_CHIP_UID not in out2.text(),
       "and --verbose still does not print the chip UID")
    ck(out2.contains("Chip UID: (withheld)"),
       "the UID line is kept, with its value taken out")
    e2.close()

    # A failure prints it, because then it is the whole diagnosis.
    e3 = Env()
    hw3 = inst.SimHardware(mode="app", flash=stock(), isp_rc=1)
    rc3, out3 = run(e3.argv(), hw3)
    ck(rc3 == inst.EXIT_FAILED, "a failed write stops", "rc=%d" % rc3)
    ck(out3.contains("Chip is hosed. Reset or power cycle it."),
       "and wchisp's own words are printed, because they are the diagnostic")
    ck(inst.SIM_CHIP_UID not in out3.text(),
       "with the chip UID taken out of them")
    e3.close()


def test_uid_redaction():
    """The redaction is on the two functions that PRINT captured tool output.

    Nothing upstream is trusted to hand them redacted text: the fixture used to
    do exactly that, which is why a real leak was invisible to every scenario.
    """
    section("the chip UID is taken out of anything printed")
    for raw in ("INFO  Chip UID: A3-7F-21-0C-5E-11-9B-44",
                "INFO  chip uid: a3-7f-21-0c-5e-11-9b-44",
                "Chip UID = A3-7F-21-0C-5E-11-9B-44",
                "UID: A3-7F-21-0C-5E-11-9B-44",
                "device A3-7F-21-0C-5E-11-9B-44 opened"):
        got = inst.redact_uid(raw)
        ck("A3-7F-21" not in got.upper(), "redacted: %r" % raw, got)
        ck("(withheld)" in got, "and says so: %r" % raw, got)
    kept = inst.redact_uid("INFO  Erased code flash\nINFO  Verify OK")
    ck(kept == "INFO  Erased code flash\nINFO  Verify OK",
       "lines with no UID in them are untouched", kept)
    once = inst.redact_uid("INFO  Chip UID: A3-7F-21-0C-5E-11-9B-44")
    ck(inst.redact_uid(once) == once, "and redacting twice changes nothing")

    # Both printers, not just the one the install happens to reach.
    log = "INFO  Chip UID: %s\nINFO  Verify OK" % inst.SIM_CHIP_UID
    said = " ".join(inst.isp_said(log))
    ck(inst.SIM_CHIP_UID not in said, "isp_said() redacts", said)
    out = inst.Out(stream=Sink())
    inst.isp_log(out, log)
    ck(inst.SIM_CHIP_UID not in out.text(), "isp_log() redacts", out.text())


def test_output_is_status_lines():
    """The shape of a successful run, as a whole.

    The install used to print ~95 lines of headed prose sections. It is a
    column of status lines now, and that is a property worth pinning: the
    failure mode being guarded against is prose creeping back one helpful
    sentence at a time.
    """
    section("the shape of the output")
    e = Env()
    hw = inst.SimHardware(mode="app", flash=stock())
    rc, out = run(e.argv(), hw)
    lines = [l for l in out.text().split("\n") if l.strip()]
    ck(rc == inst.EXIT_OK, "the install succeeds", "rc=%d" % rc)
    # 32, not the 29 a release run prints: this Env builds its own
    # bootloader.bin, so the run also carries the two-line "not the published
    # build" note and names the --isp path. The CLI check below pins the real
    # number.
    ck(len(lines) <= 32, "a whole successful install is at most 32 lines",
       "%d lines" % len(lines))
    marked = [l for l in lines if l.startswith("[ ")]
    ck(len(marked) >= 8, "most of it is status lines", "%d" % len(marked))
    ck(all(len(l) <= 4 + inst.WIDTH for l in lines),
       "no line runs past the wrap width")

    # No step headers, no underlines, no first person.
    for banned in ("Step 1", "Step 4 of", "-----", "I will", "I could not",
                   "I cannot", "I have", "let me", "we will"):
        ck(banned.lower() not in out.text().lower(),
           "the output contains no %r" % banned)
    e.close()


def test_no_reassurance_anywhere():
    """Reassurance is the form the prose keeps coming back in.

    Three passes shortened the paragraphs and kept the paragraph's habits: a
    line explaining that a failure is not so bad, a clause explaining what the
    next step is about to do.  Neither is a status line, so both are asserted
    away here -- across the FAILURE paths, which is where they survived
    longest, not just the happy one.
    """
    section("no reassurance and no preambles, on any path")
    banned = ("costs nothing", "changes nothing", "writes nothing",
              "nothing is lost", "nothing here is permanent", "NOT dead",
              "not evidence of failure", "re-checks everything",
              "as many times as you like", "harmlessly", "worth knowing about",
              "Do not skip past this", "never needed again",
              "not a failure", "no more broken than before")
    everything = []
    for name in sorted(inst.SCENARIOS):
        p = subprocess.run([sys.executable, INSTALL_PY, "--dry-run", "--sim",
                            name, "--no-color",
                            "--bootloader", spawned_bootloader()],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           cwd=ROOT)
        everything.append(p.stdout.decode("utf-8", "replace"))
    text = "\n".join(everything)
    for phrase in banned:
        ck(phrase.lower() not in text.lower(),
           "no scenario says %r" % phrase)
    # The mandated keeps are keeps: cutting prose must not cut these.
    for keep in ("H1, not the U22 button",
                 "No LEDs light in ISP mode",
                 "The H1 jumper is needed for the write",
                 "--restore",
                 "Unplug any other CH579 board"):
        ck(keep in text, "and every run still carries %r" % keep)


# --------------------------------------------------------------------------
# 6. The user configuration word
# --------------------------------------------------------------------------

def test_user_config_is_never_written():
    section("the CH579 user configuration word")
    # The long/short forms below are the config-word switches carried by other
    # CH5xx ISP tools. install.py only ever runs wchisp, but --isp takes a
    # path, so the guard has to refuse on the ARGUMENTS rather than trusting
    # which binary is on the far end.
    for bad in (["isp-tool", "--code-flash", "x.bin", "--user-config", "4d"],
                ["isp-tool", "-u", "4d", "--code-flash", "x.bin"],
                ["isp-tool", "--user-config=4d"],
                ["isp-tool", "-w", "ffffffff"],
                ["isp-tool", "--write-protection-config", "ffffffff"]):
        raised = False
        try:
            inst.isp_argv_guard(bad)
        except RuntimeError:
            raised = True
        ck(raised, "isp_argv_guard refuses %r" % (bad[1:],))
    ck(inst.isp_argv_guard(["wchisp", "flash", "x.bin"]) is not None,
       "isp_argv_guard passes a plain `wchisp flash FILE` through")

    forbidden = ("--user-config", "-u", "--write-protection-config", "-w")
    offenders = [c for c in _ALL_ISP_CALLS
                 if any(a.split("=")[0] in forbidden for a in c)]
    ck(not offenders,
       "no ISP command issued anywhere in this suite touched the user "
       "configuration word", "%d call(s) inspected" % len(_ALL_ISP_CALLS))
    ck(all(len(c) == 3 and c[1] == "flash" for c in _ALL_ISP_CALLS),
       "every ISP command in this suite was exactly `wchisp flash FILE` -- "
       "one verb, one file, nothing else",
       "%d call(s)" % len(_ALL_ISP_CALLS))


# --------------------------------------------------------------------------
# 6. The dry-run entry point, as a user would invoke it
# --------------------------------------------------------------------------

def test_dry_run_cli():
    section("--dry-run from the command line")
    expected = {
        "empty": inst.EXIT_OK,
        "installed": inst.EXIT_REFUSED,
        "erased": inst.EXIT_REFUSED,
        "isp": inst.EXIT_REFUSED,
        "absent": inst.EXIT_NODEV,
        "bad-backup": inst.EXIT_REFUSED,
        "no-isp-entry": inst.EXIT_FAILED,
        "isp-fails": inst.EXIT_FAILED,
        "no-isp-tool": inst.EXIT_REFUSED,
        "bad-bootloader": inst.EXIT_REFUSED,
        "two-devices": inst.EXIT_REFUSED,
        "dead": inst.EXIT_NODEV,
        "corrupt-boot-region": inst.EXIT_REFUSED,
    }
    ck(sorted(expected) == sorted(inst.SCENARIOS),
       "every simulated scenario is covered here",
       "missing: %s" % sorted(set(inst.SCENARIOS) - set(expected)))
    for name, want in sorted(expected.items()):
        p = subprocess.run([sys.executable, INSTALL_PY, "--dry-run",
                            "--sim", name,
                            "--bootloader", spawned_bootloader()],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           cwd=ROOT)
        text = p.stdout.decode("utf-8", "replace")
        ck(p.returncode == want, "--dry-run --sim %s exits %d" % (name, want),
           "got %d" % p.returncode)
        ck("Traceback" not in text, "--dry-run --sim %s raises nothing" % name)
        # A stop is marked -- with [ xx ], or with the [ !! ] line that
        # already said the same thing, which the stop then writes underneath
        # rather than restating.
        ck(want == inst.EXIT_OK or "[ xx ]" in text or "[ !! ]" in text,
           "--dry-run --sim %s marks why it stopped" % name)
        # ONE HEADLINE PER FAILURE.  The same words under two markers is the
        # wasted line the reader can least afford, so it is asserted away.
        heads = re.findall(r"^\[ (?:xx|!!) \] (.*)$", text, re.M)
        ck(len(heads) == len(set(heads)),
           "--dry-run --sim %s says each headline once" % name, str(heads))
        ck("/Users/" not in text and "/home/" not in text,
           "--dry-run --sim %s prints no home-directory paths" % name)
        ck(inst.SIM_CHIP_UID not in text,
           "--dry-run --sim %s prints no chip UID" % name)
        if want == inst.EXIT_OK:
            # The shape the owner asked for, measured on the real command:
            # a successful install, minus the lines only --dry-run prints.
            real = [l for l in text.split("\n")
                    if l.strip() and "dry run" not in l.lower()
                    and l.strip() != "itself"]
            ck(len(real) <= 30,
               "a successful --dry-run --sim %s is at most 30 real lines"
               % name, "%d lines" % len(real))


def test_usage():
    section("argument handling")
    for argv in (["--check", "--restore", "x"],
                 ["--backup", "a", "--restore", "b"]):
        p = subprocess.run([sys.executable, INSTALL_PY] + argv,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           cwd=ROOT)
        ck(p.returncode == inst.EXIT_USAGE,
           "%r is rejected as a usage error" % (argv,),
           "rc=%d" % p.returncode)
    p = subprocess.run([sys.executable, INSTALL_PY, "--help"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       cwd=ROOT)
    text = p.stdout.decode("utf-8", "replace")
    ck(p.returncode == 0, "--help works")
    for flag in ("--overwrite-existing-bootloader", "--allow-unverified-backup",
                 "--restore-unverified"):
        ck("CONSEQUENCE" in text.split(flag)[-1][:600] if flag in text
           else False,
           "%s states its consequence in --help" % flag)


# --------------------------------------------------------------------------

def main():
    tests = [
        test_gates,
        test_composite_construction,
        test_synth_matches_make_synthetic_fw,
        test_dist_stamps_the_bootloader_digest,
        test_real_bootloader,
        test_install_happy,
        test_install_refuses_existing_bootloader,
        test_install_refuses_erased_region,
        test_install_no_device,
        test_install_device_in_isp,
        test_install_short_left_on_after_the_write,
        test_restore_short_left_on_after_the_write,
        test_missing_pyserial_is_not_a_broken_device,
        test_no_pyserial_on_windows_is_not_a_missing_device,
        test_restore_with_the_port_held_is_still_a_success,
        test_install_refuses_bad_backup,
        test_unverified_backup_never_claims_a_stop,
        test_unverified_backup_is_never_called_good,
        test_release_phrases,
        test_a_bare_enter_is_not_a_yes,
        test_install_needs_isp_tool_before_the_jumper,
        test_isp_tool_choice,
        test_install_uses_wchisp_when_it_is_there,
        test_download_wchisp_when_accepted,
        test_download_unpacks_both_archive_shapes,
        test_colour,
        test_install_refuses_bad_bootloader,
        test_install_jumper_never_takes,
        test_install_isp_write_fails,
        test_install_readback_catches_a_bad_write,
        test_check_mode,
        test_backup_mode,
        test_restore_on_a_dead_device,
        test_restore_healthy_device,
        test_restore_refuses_unverified,
        test_restore_needs_no_pyserial,
        test_boot_region_gates,
        test_sim_boot_model_is_strict,
        test_install_refuses_a_corrupt_boot_region,
        test_query_fw_decodes_the_documented_offsets,
        test_pcb_readings_are_reported_not_reconciled,
        test_wchisp_output_is_captured_unless_it_matters,
        test_uid_redaction,
        test_output_is_status_lines,
        test_no_reassurance_anywhere,
        test_backup_file_is_read_back,
        test_two_devices_are_refused,
        test_restore_refuses_over_a_working_device,
        test_restore_reports_success_without_pyserial,
        test_dry_run_cli,
        test_usage,
        test_user_config_is_never_written,   # last: it audits every call above
    ]
    for t in tests:
        t()
    tally = "test_install: %d checks, %d failures" % (CHECKS, len(FAILS))
    sys.stdout.write(tally + "\n")
    for f in FAILS:
        sys.stdout.write("  failed: %s\n" % f)
    rc = 1 if FAILS else 0
    return rc or check_docs_quote_the_real_tally()


# Documents quote this suite's output as something a reader can reproduce, and
# the release body greps the real figure out of the run that built it -- so a
# stale number in a doc becomes a published release contradicting its own repo.
# It has drifted twice. This is not a `ck()`: counting it would change the very
# number it is checking.
QUOTED_TALLY = ["docs/RELEASE-NOTES.md", "TECHNICAL_DETAILS.md"]


def check_docs_quote_the_real_tally():
    want = "test_install: %d checks, 0 failures" % CHECKS
    stale = []
    for rel in QUOTED_TALLY:
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            continue
        with open(path) as f:
            for n, line in enumerate(f, 1):
                m = re.search(r"test_install: (\d+) checks, 0 failures", line)
                if m and m.group(1) != str(CHECKS):
                    stale.append("%s:%d quotes %s" % (rel, n, m.group(1)))
    if stale:
        sys.stdout.write("  FAIL the quoted tally is stale -- should be %d\n"
                         % CHECKS)
        for s in stale:
            sys.stdout.write("       %s\n" % s)
        sys.stdout.write("       fix with:  %s\n" % want)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
