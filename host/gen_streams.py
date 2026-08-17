#!/usr/bin/env python3
"""gen_streams.py — drive the three REAL updater implementations against the
bootloader's protocol layer, with no hardware, and record the byte streams.

The point of this file is that it does NOT reimplement the protocol. It imports
the actual host code and hands it a fake pyserial port whose other end is
src/proto.c, compiled into a shared library and reached through ctypes:

  FlashGBX  .flashgbx/FlashGBX/hw_GBFlash.py :: FirmwareUpdater
            (imported and driven unmodified; TryConnect + WriteFirmware)
  unlocker  gbflash_serial_update.py (third-party; not redistributed here)
            (imported and driven unmodified; trigger + connect + write)
  getserial the creator's own getserial.py, from getserial.zip
            (exec'd unmodified, including its module-level self-invocation;
             only builtins/`serial`/`time` are patched underneath it)

Each run writes three files into fixtures/:
    <name>.h2d.bin   every byte the host sent, in order
    <name>.d2h.bin   every byte the device answered, in order
    <name>.meta.txt  image, page_size, packet count, host's own verdict

test_proto.c then replays <name>.h2d.bin through a fresh device under UBSan
and guard pages and requires the output to equal <name>.d2h.bin byte for byte,
so the C test suite needs neither Python nor the vendor code at run time.

Exit status is nonzero if any host implementation fails or disagrees.
"""

from __future__ import annotations

import ctypes
import io
import json
import os
import struct
import sys
import types
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
BOOTLOADER = HERE.parent                       # work/bootloader
WORK = BOOTLOADER.parent                       # work
ROOT = WORK.parent                             # the GBFlash project root

# The image the drivers are fed. By DEFAULT a synthetic image built by
# make_synthetic_fw.py, not vendor firmware: the protocol layer never inspects
# payload meaning — only framing, CRCs, sequencing and lengths — so a
# structurally valid image exercises the same paths and the repository needs no
# third-party firmware. Set FW_BIN to rehearse against a real fw.bin locally.
SYNTH_FW = HERE / "build" / "synthetic_fw.bin"
FW_BIN = Path(os.environ["FW_BIN"]) if os.environ.get("FW_BIN") else SYNTH_FW
FLASHGBX_ROOT = ROOT / "gbflash-tools" / ".flashgbx"
UNLOCKER = WORK / "ref" / "gbflash_unlocker" / "gbflash_serial_update.py"


def ensure_fw_image() -> bool:
    """Make sure FW_BIN exists, generating the synthetic default if needed."""
    if FW_BIN.is_file():
        return True
    if FW_BIN != SYNTH_FW:
        return False                      # caller pointed us somewhere missing
    sys.path.insert(0, str(HERE))
    import make_synthetic_fw
    FW_BIN.parent.mkdir(parents=True, exist_ok=True)
    FW_BIN.write_bytes(make_synthetic_fw.build(0x7520))
    print(f"  generated synthetic image: {rel(FW_BIN)}")
    return True

# getserial.py and its se.pkbin payload, extracted read-only from getserial.zip.
# host/vendor/getserial/ is where they go if you have them; set GETSERIAL_DIR to
# point somewhere else. They are NOT redistributed here, so by default neither
# is present and run_getserial() skips itself with a note. The bundled
# getserial.exe is never executed.
def _find_getserial() -> Path:
    env = os.environ.get("GETSERIAL_DIR")
    candidates = ([Path(env)] if env else []) + [HERE / "vendor" / "getserial"]
    for c in candidates:
        if (c / "getserial.py").is_file() and (c / "se.pkbin").is_file():
            return c
    return candidates[-1]


GETSERIAL_DIR = _find_getserial()

FIXTURES = HERE / "fixtures"
BUILD = HERE / "build"
LIB = BUILD / "libgbfdev.dylib"

INTRO = 0x48484A4A
OUTRO = 0x4A4A4848

failures: list[str] = []
notes: list[str] = []


# --------------------------------------------------------------------------
# The device: src/proto.c + the RAM flash stub, through ctypes.
# --------------------------------------------------------------------------

class Device:
    def __init__(self, libpath: Path):
        self.lib = ctypes.CDLL(str(libpath))
        self.lib.dev_feed.restype = ctypes.c_uint32
        self.lib.dev_feed.argtypes = [ctypes.c_char_p, ctypes.c_uint32,
                                      ctypes.c_char_p, ctypes.c_uint32]
        self.lib.dev_flash_read.restype = ctypes.c_uint32
        self.lib.dev_flash_read.argtypes = [ctypes.c_uint32, ctypes.c_char_p,
                                            ctypes.c_uint32]
        for fn in ("dev_finalized", "dev_bootloader_intact"):
            getattr(self.lib, fn).restype = ctypes.c_int
        for fn in ("dev_flash_stat", "dev_proto_stat"):
            getattr(self.lib, fn).restype = ctypes.c_uint32
            getattr(self.lib, fn).argtypes = [ctypes.c_int]

    def reset(self):
        self.lib.dev_reset()

    def feed(self, data: bytes) -> bytes:
        if not data:
            return b""
        out = ctypes.create_string_buffer(len(data) + 4096)
        n = self.lib.dev_feed(data, len(data), out, len(out))
        if n == 0xFFFFFFFF:
            raise RuntimeError("device response buffer overflow")
        return out.raw[:n]

    def flash(self, addr: int, n: int) -> bytes:
        buf = ctypes.create_string_buffer(n)
        got = self.lib.dev_flash_read(addr, buf, n)
        return buf.raw[:got]

    @property
    def finalized(self) -> bool:
        return bool(self.lib.dev_finalized())

    @property
    def bootloader_intact(self) -> bool:
        return bool(self.lib.dev_bootloader_intact())


class Recorder:
    """Records the full conversation so it can be replayed from C."""

    def __init__(self, dev: Device):
        self.dev = dev
        self.h2d = bytearray()
        self.d2h = bytearray()
        self.rx = bytearray()

    def write(self, data: bytes) -> int:
        data = bytes(data)
        self.h2d += data
        resp = self.dev.feed(data)
        self.d2h += resp
        self.rx += resp
        return len(data)

    def read(self, n: int) -> bytes:
        if n <= 0:
            return b""
        take = self.rx[:n]
        del self.rx[:n]
        return bytes(take)


class FakeSerial:
    """Just enough pyserial for all three updaters.

    close()/open() are deliberately no-ops on the device: a real bootloader
    keeps its state across a host-side port close, and FlashGBX closes and
    reopens the port in the middle of TryConnect.
    """

    _rec: Recorder | None = None

    def __init__(self, port=None, baudrate=None, timeout=None, **kw):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.is_open = True

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()
        return False

    @property
    def in_waiting(self) -> int:
        return len(FakeSerial._rec.rx)

    def write(self, data) -> int:
        return FakeSerial._rec.write(data)

    def read(self, n=1) -> bytes:
        return FakeSerial._rec.read(n)

    def readline(self) -> bytes:
        return b""

    def flush(self):
        pass

    def reset_input_buffer(self):
        FakeSerial._rec.rx.clear()

    def close(self):
        self.is_open = False

    def open(self):
        self.is_open = True


class SerialException(Exception):
    pass


def make_fake_serial_module() -> types.ModuleType:
    mod = types.ModuleType("serial")
    mod.Serial = FakeSerial
    mod.SerialException = SerialException
    util = types.ModuleType("serial.serialutil")
    util.SerialException = SerialException
    mod.serialutil = util
    lp = types.ModuleType("serial.tools.list_ports")

    class _Port:
        device = "FAKE0"
        vid = 0x1A86
        pid = 0x7523

    lp.comports = lambda: [_Port()]
    tools = types.ModuleType("serial.tools")
    tools.list_ports = lp
    mod.tools = tools
    return mod


def make_fake_time() -> types.ModuleType:
    """time with sleep() removed. The updaters sleep 3 s after the trigger and
    1 s between retries; none of that is meaningful against an instantaneous
    device, and the harness must not take three minutes to run."""
    import time as real_time

    mod = types.ModuleType("time")
    mod.sleep = lambda *_a, **_kw: None
    mod.monotonic = real_time.monotonic
    mod.time = real_time.time
    return mod


# --------------------------------------------------------------------------
# Fixture bookkeeping
# --------------------------------------------------------------------------

def rel(path) -> str:
    """A path as it appears in a fixture's .meta.txt.

    fixtures/*.meta.txt are checked in, so they must not carry the absolute
    paths of whoever generated them. Everything this harness reads lives under
    the project root, so record it relative to that; anything outside (a
    GETSERIAL_DIR override) degrades to the bare filename.
    """
    p = Path(path).resolve()
    try:
        return p.relative_to(ROOT).as_posix()
    except ValueError:
        return p.name


def save(name: str, rec: Recorder, meta: dict) -> None:
    FIXTURES.mkdir(parents=True, exist_ok=True)
    (FIXTURES / f"{name}.h2d.bin").write_bytes(bytes(rec.h2d))
    (FIXTURES / f"{name}.d2h.bin").write_bytes(bytes(rec.d2h))
    (FIXTURES / f"{name}.meta.txt").write_text(
        "\n".join(f"{k}={v}" for k, v in meta.items()) + "\n"
    )


def check(cond: bool, msg: str) -> bool:
    if not cond:
        failures.append(msg)
    return cond


def verify_device(dev: Device, name: str, image: bytes) -> dict:
    """The invariants that matter, checked on the modelled flash."""
    written = dev.flash(0x3E00, len(image))
    ok_image = written == image
    check(ok_image, f"{name}: flash at 0x3E00 does not match the source image")

    tail = dev.flash(0x3E00 + len(image), 0x200)
    ok_tail = set(tail) <= {0xFF}
    check(ok_tail, f"{name}: bytes past the image are not erased")

    ok_intact = dev.bootloader_intact
    check(ok_intact, f"{name}: the bootloader region below 0x3E00 was modified")

    ok_final = dev.finalized
    check(ok_final, f"{name}: device did not reach the finalized state")

    return {
        "image_match": ok_image,
        "tail_erased": ok_tail,
        "bootloader_intact": ok_intact,
        "finalized": ok_final,
    }


def parse_frames(stream: bytes) -> list[dict]:
    """Parse a device->host stream the way both hosts do, for meta output."""
    out = []
    pos = 0
    while pos + 11 <= len(stream):
        intro, sender, seq, cmd, plen = struct.unpack(">IBHHH",
                                                      stream[pos:pos + 11])
        body = stream[pos + 11:pos + 11 + plen]
        outro = stream[pos + 11 + plen:pos + 15 + plen]
        out.append({"intro": intro, "sender": sender, "seq": seq, "cmd": cmd,
                    "plen": plen, "payload": body, "outro": outro})
        pos += 15 + plen
    return out


# --------------------------------------------------------------------------
# Driver 1 — FlashGBX's own FirmwareUpdater, imported and run unmodified
# --------------------------------------------------------------------------

def run_flashgbx(dev: Device, image_path: Path, gui_double_trigger: bool):
    name = "flashgbx_gui" if gui_double_trigger else "flashgbx"
    image = image_path.read_bytes()

    # FlashGBX pulls Mapper -> dateutil, which is not installed here and is
    # irrelevant to the updater. Stub it so the REAL hw_GBFlash.py can load.
    if "dateutil" not in sys.modules:
        du = types.ModuleType("dateutil")
        rd = types.ModuleType("dateutil.relativedelta")
        rd.relativedelta = object
        du.relativedelta = rd
        sys.modules["dateutil"] = du
        sys.modules["dateutil.relativedelta"] = rd

    sys.path.insert(0, str(FLASHGBX_ROOT))
    try:
        import FlashGBX.hw_GBFlash as hw
    finally:
        sys.path.pop(0)

    fake_serial = make_fake_serial_module()
    hw.serial = fake_serial
    hw.time = make_fake_time()

    BUILD.mkdir(parents=True, exist_ok=True)
    zip_path = BUILD / "fw_GBFlash.zip"
    with zipfile.ZipFile(zip_path, "w") as z:
        z.writestr("fw.bin", image)

    dev.reset()
    rec = Recorder(dev)
    FakeSerial._rec = rec

    if gui_double_trigger:
        # GUI path: UpdateFirmware() calls CONN.BootloaderReset() first, which
        # sends F1 / read1 / 01 to the *application*; WriteFirmware then sends
        # the identical trigger again, this time to a device already sitting in
        # the bootloader. hw_GBFlash.py lines 627 + 312-314.
        rec.write(b"\xF1")
        rec.read(1)
        rec.write(b"\x01")

    statuses = []

    # The keyword names mirror FlashGBX's own SetStatus signature exactly,
    # because FlashGBX calls this callback by keyword and a stub that omitted
    # one would raise TypeError. Every argument but `text` is ignored here;
    # none of them means anything to this project.
    def status(text=None, setProgress=None, enableUI=None, cloneError=None):
        if text is not None:
            statuses.append(str(text))

    upd = hw.FirmwareUpdater(app_path=str(BUILD), port="FAKE0")
    rc = upd.WriteFirmware(str(zip_path), status)

    check(rc == 1, f"{name}: FlashGBX WriteFirmware returned {rc}, expected 1")
    # FlashGBX reports "Update failed!" on a bad finalize and *still* returns 1
    # (see docs/PROTOCOL.md), so the status text has to be inspected.
    check("Update failed!" not in statuses,
          f"{name}: FlashGBX reported 'Update failed!' on finalize")

    frames = parse_frames(bytes(rec.d2h))
    meta = {"host": "FlashGBX FirmwareUpdater (imported)",
            "source": rel(FLASHGBX_ROOT / "FlashGBX" / "hw_GBFlash.py"),
            "image": rel(image_path), "image_len": len(image),
            "gui_double_trigger": gui_double_trigger,
            "rc": rc, "device_frames": len(frames),
            "final_status": statuses[-1] if statuses else ""}
    meta.update(verify_device(dev, name, image))
    save(name, rec, meta)
    print(f"  {name}: {len(rec.h2d)} bytes host->dev, {len(rec.d2h)} back, "
          f"{len(frames)} response frames, rc={rc}")
    return meta


# --------------------------------------------------------------------------
# Driver 2 — the unlocker's gbflash_serial_update.py, imported and run
# --------------------------------------------------------------------------

def run_unlocker(dev: Device, image_path: Path):
    name = "unlocker"
    image = image_path.read_bytes()

    import importlib.util
    spec = importlib.util.spec_from_file_location("gbflash_serial_update",
                                                  UNLOCKER)
    mod = importlib.util.module_from_spec(spec)
    fake_serial = make_fake_serial_module()
    saved = sys.modules.get("serial")
    sys.modules["serial"] = fake_serial
    sys.modules["serial.tools"] = fake_serial.tools
    sys.modules["serial.tools.list_ports"] = fake_serial.tools.list_ports
    try:
        spec.loader.exec_module(mod)
        mod.time = make_fake_time()

        dev.reset()
        rec = Recorder(dev)
        FakeSerial._rec = rec

        logs = []
        err = None
        try:
            # update_port() runs the whole thing: trigger (F1/read/01),
            # double 0x21 init, every 0x24, then 0x23.
            mod.update_port("FAKE0", image, 0.5, False, log=logs.append)
        except Exception as exc:            # UpdateError and friends
            err = exc
    finally:
        if saved is not None:
            sys.modules["serial"] = saved
        else:
            sys.modules.pop("serial", None)
        sys.modules.pop("serial.tools", None)
        sys.modules.pop("serial.tools.list_ports", None)

    check(err is None, f"{name}: unlocker raised {err!r}")

    frames = parse_frames(bytes(rec.d2h))
    meta = {"host": "gbflash_serial_update.py (imported)",
            "source": rel(UNLOCKER),
            "image": rel(image_path), "image_len": len(image),
            "error": repr(err) if err else "",
            "device_frames": len(frames),
            "log": " | ".join(l for l in logs if l)}
    meta.update(verify_device(dev, name, image))
    save(name, rec, meta)
    print(f"  {name}: {len(rec.h2d)} bytes host->dev, {len(rec.d2h)} back, "
          f"{len(frames)} response frames, error={err!r}")
    return meta


# --------------------------------------------------------------------------
# Driver 3 — the creator's getserial.py, exec'd unmodified
# --------------------------------------------------------------------------

def run_getserial(dev: Device):
    name = "getserial"
    src_path = GETSERIAL_DIR / "getserial.py"
    payload_path = GETSERIAL_DIR / "se.pkbin"
    if not src_path.is_file() or not payload_path.is_file():
        notes.append(f"getserial: skipped, {src_path} not found")
        print(f"  {name}: SKIPPED (not found at {GETSERIAL_DIR})")
        return None

    image = payload_path.read_bytes()
    source = src_path.read_text()

    fake_serial = make_fake_serial_module()
    dev.reset()
    rec = Recorder(dev)
    FakeSerial._rec = rec

    # getserial.py calls d() at module level and then blocks on input() twice.
    # Nothing is edited: serial, time, input and sys.argv are patched in the
    # namespace it executes in. Its bundled .exe is never touched.
    g = {
        "__name__": "getserial",
        "__file__": str(src_path),
        "input": lambda *_a: "",
        "print": lambda *a, **k: None,
    }
    saved = sys.modules.get("serial")
    saved_argv = sys.argv
    sys.modules["serial"] = fake_serial
    sys.modules["serial.tools"] = fake_serial.tools
    sys.modules["serial.tools.list_ports"] = fake_serial.tools.list_ports
    sys.argv = ["getserial.py", str(payload_path)]
    out = io.StringIO()
    err = None
    try:
        with redirect_stdout(out):
            code = compile(source, str(src_path), "exec")
            exec(code, g)                      # noqa: S102 — vendor code, on purpose
            g["time"] = make_fake_time()
    except Exception as exc:
        err = exc
    finally:
        sys.argv = saved_argv
        if saved is not None:
            sys.modules["serial"] = saved
        else:
            sys.modules.pop("serial", None)
        sys.modules.pop("serial.tools", None)
        sys.modules.pop("serial.tools.list_ports", None)

    check(err is None, f"{name}: getserial.py raised {err!r}")

    frames = parse_frames(bytes(rec.d2h))
    # getserial sends 0x21 exactly ONCE (FlashGBX and the unlocker send it
    # twice); that asymmetry is the whole reason it is in this harness.
    inits = [f for f in frames if f["cmd"] == 0x21]
    check(len(inits) == 1,
          f"{name}: expected exactly one 0x21 exchange, saw {len(inits)}")

    meta = {"host": "getserial.py (creator's own, exec'd unmodified)",
            "source": rel(src_path),
            "image": rel(payload_path), "image_len": len(image),
            "error": repr(err) if err else "",
            "device_frames": len(frames),
            "init_frames": len(inits)}
    meta.update(verify_device(dev, name, image))
    save(name, rec, meta)
    print(f"  {name}: {len(rec.h2d)} bytes host->dev, {len(rec.d2h)} back, "
          f"{len(frames)} response frames, error={err!r}")
    return meta


# --------------------------------------------------------------------------
# Cross-implementation agreement
# --------------------------------------------------------------------------

def compare_streams():
    """FlashGBX and the unlocker build byte-identical frames (FGBX assembles
    the 0x24 payload back to front, the unlocker forward). The only difference
    in a full run should be none at all — assert it, because a divergence means
    the spec reconciliation in docs/PROTOCOL.md is wrong somewhere."""
    a = (FIXTURES / "flashgbx.h2d.bin")
    b = (FIXTURES / "unlocker.h2d.bin")
    if not (a.is_file() and b.is_file()):
        return
    da, db = a.read_bytes(), b.read_bytes()
    if da == db:
        notes.append("FlashGBX and the unlocker produced byte-identical "
                     f"host->device streams ({len(da)} bytes)")
    else:
        n = min(len(da), len(db))
        i = next((k for k in range(n) if da[k] != db[k]), n)
        failures.append(f"FlashGBX and unlocker streams diverge at byte {i} "
                        f"(lens {len(da)}/{len(db)})")


# --------------------------------------------------------------------------
# Response baseline
#
# The device->host fixtures are RECORDED FROM proto.c, and the Makefile
# regenerates them whenever proto.c changes, before test_proto replays them.
# So test_proto.c's "response bytes differ from what the real host accepted"
# check is, on its own, a self-comparison: it cannot notice a changed response,
# because the thing it compares against was just re-recorded from the change.
#
# What actually gates response correctness at generation time is the hosts'
# own accept/reject, and those are lenient — none of them inspects payload[0],
# payload[5:7] or the sender byte, and page_size is simply obeyed, so any
# self-consistent value passes.
#
# d2h-baseline.json pins the SHA-256 of each recorded response stream. It is
# NOT regenerated by a normal run and is not removed by `make clean`, so a
# change in what the device says has to be acknowledged deliberately:
#
#     GBFLASH_REBASELINE=1 make -C host fixtures
#
# --------------------------------------------------------------------------

BASELINE = FIXTURES / "d2h-baseline.json"


def check_response_baseline() -> None:
    import hashlib

    current = {
        p.name.removesuffix(".d2h.bin"):
            hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(FIXTURES.glob("*.d2h.bin"))
    }
    if os.environ.get("GBFLASH_REBASELINE") == "1" or not BASELINE.is_file():
        was = "rewritten" if BASELINE.is_file() else "created"
        BASELINE.write_text(json.dumps({"sha256": current}, indent=2) + "\n")
        notes.append(f"device->host response baseline {was} "
                     f"({len(current)} streams)")
        return

    recorded = json.loads(BASELINE.read_text()).get("sha256", {})
    for name, digest in sorted(current.items()):
        if name not in recorded:
            notes.append(f"{name}: no pinned response baseline yet; rerun with "
                         f"GBFLASH_REBASELINE=1 to pin it")
            continue
        check(recorded[name] == digest,
              f"{name}: the device's RESPONSE stream changed "
              f"({recorded[name][:12]} -> {digest[:12]}). The replay test "
              f"cannot catch this on its own, because it compares against a "
              f"fixture re-recorded from the same code. If the change is "
              f"intended, rerun with GBFLASH_REBASELINE=1 and say why in the "
              f"commit.")
    for name in sorted(set(recorded) - set(current)):
        notes.append(f"{name}: pinned in the response baseline but no longer "
                     f"generated")


def main() -> int:
    if not LIB.is_file():
        print(f"error: {LIB} not built; run `make -C host` first",
              file=sys.stderr)
        return 2
    if not ensure_fw_image():
        print(f"error: FW_BIN={FW_BIN} not found", file=sys.stderr)
        return 2

    dev = Device(LIB)
    print("Generating authentic updater streams:")
    run_flashgbx(dev, FW_BIN, gui_double_trigger=False)
    run_flashgbx(dev, FW_BIN, gui_double_trigger=True)
    run_unlocker(dev, FW_BIN)
    run_getserial(dev)
    # Write the image the drivers were fed alongside the fixtures, so the C test
    # suite can compare its reconstruction against it without reaching outside
    # the repository for a file it may not have.
    (FIXTURES / "image.bin").write_bytes(FW_BIN.read_bytes())
    print(f"  reference image: {rel(FIXTURES / 'image.bin')} "
          f"({FW_BIN.stat().st_size} bytes)")

    compare_streams()

    check_response_baseline()

    for n in notes:
        print(f"  note: {n}")
    if failures:
        print("\nFAILURES:")
        for f in failures:
            print(f"  - {f}")
        # Deliberately do NOT write manifest.json: it is the make stamp, and
        # stamping a failed generation would let the next `make test` skip
        # straight past the failure.
        return 1

    (FIXTURES / "manifest.json").write_text(json.dumps(
        {"fixtures": sorted(p.name for p in FIXTURES.glob("*.h2d.bin"))},
        indent=2) + "\n")
    print("  all host implementations completed successfully")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
