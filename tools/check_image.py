#!/usr/bin/env python3
"""check_image.py — offline validation of the GBFlash CH579 bootloader image.

OWNED BY: comp:integrate.   Invoked by `make check`.

Everything here is static analysis of the produced .bin/.elf.  Nothing touches
hardware and nothing needs the device.  This is the stage-0 gate from
the build brief ("run the checklist against our own output").

A FAIL means do not flash the image.  Each check prints the evidence it used,
because on this project the reviewable artifact is the disassembly, not a
green tick.
"""

import argparse
import os
import re
import struct
import subprocess
import sys

HERE     = os.path.dirname(os.path.abspath(__file__))
INCLUDE  = os.path.join(os.path.dirname(HERE), "include")

# --------------------------------------------------------------------------
# Constants — all from the hard constraints in the build brief
# --------------------------------------------------------------------------
FLASH_BUDGET      = 15872          # 0x3E00 — bootloader region, sectors 0..30
BOOTINFO_BASE     = 0x00003E00
APP_BASE          = 0x00004000
STACK_TOP         = 0x20008000
VECTOR_COUNT      = 36
VECTOR_TABLE_SIZE = VECTOR_COUNT * 4    # 0x90

# "no literal in application space" window from the brief:
# 0x4000 (app base) .. 0x4000 + 0x7520 - 1 (end of the shipping L15 payload),
# extended down to the boot-info record.
APPSPACE_LO = 0x00003E00
APPSPACE_HI = 0x0000B51F

# CH579 InfoFlash and the user configuration word holding CFG_BOOT_EN.
INFOFLASH_LO   = 0x00040000
INFOFLASH_HI   = 0x000403FF
USER_CFG_WORD  = 0x00040010
R8_FLASH_PROTECT = 0x40001809
FLASH_UNLOCK_INFOFLASH = 0x8C      # NEVER — can clear CFG_BOOT_EN
FLASH_UNLOCK_DATAFLASH = 0x84      # not used by this design

# The dispatcher from docs/DESIGN.md §1, byte-exact.
#
# It grew from 28 to 36 bytes when the Thumb-bit gate (`lsls r1,r0,#31` / `beq`)
# was added above `bx r0`: an in-range but EVEN application vector now reaches
# the defined spin instead of a HardFault escalating to LOCKUP.  The second
# literal (R8_FLASH_PROTECT) belongs to bl_nmi_handler, which sits immediately
# above and falls through into this symbol, but the literal pool is shared and
# `nm -S` reports it inside bl_vector_forward, so it is listed here.
DISPATCHER_BYTES = bytes.fromhex(
    "eff30580"   # f3ef 8005   mrs   r0, IPSR
    "8000"       # 0080        lsls  r0, r0, #2
    "0549"       # 4905        ldr   r1, [pc, #20]
    "0858"       # 5808        ldr   r0, [r1, r0]
    "810b"       # 0b81        lsrs  r1, r0, #14
    "04d0"       # d004        beq.n .Lhang
    "810c"       # 0c81        lsrs  r1, r0, #18
    "02d1"       # d102        bne.n .Lhang
    "c107"       # 07c1        lsls  r1, r0, #31   <- Thumb-bit gate
    "00d0"       # d000        beq.n .Lhang
    "0047"       # 4700        bx    r0
    "fee7"       # e7fe        b.n   .   (.Lhang)
    "c046"       # 46c0        nop
    "00400000"   # .word 0x00004000   (application vector table base)
    "09180040"   # .word 0x40001809   (R8_FLASH_PROTECT, used by bl_nmi_handler)
)
DISPATCHER_MNEMONICS = [
    "mrs", "lsls", "ldr", "ldr", "lsrs", "beq", "lsrs", "bne",
    "lsls", "beq", "bx", "b", "nop",
]
DISPATCHER_LITERALS = [0x00004000, R8_FLASH_PROTECT]

# bl_nmi_handler: 8 bytes that lock CodeFlash and then FALL THROUGH into the
# dispatcher above (ARMv6-M `b` only reaches +/-2 KB, so fall-through rather
# than a branch).  The explicit `nop` keeps the prologue exactly 8 bytes so
# bl_vector_forward stays 4-byte aligned for its PC-relative pool.
NMI_BYTES = bytes.fromhex(
    "0948"       # 4809        ldr   r0, =R8_FLASH_PROTECT
    "8021"       # 2180        movs  r1, #0x80
    "0170"       # 7001        strb  r1, [r0]
    "c046"       # 46c0        nop
)

# --------------------------------------------------------------------------
# Stage 3 — USB.  Constants transcribed from the stock L15 application image
# and verified against it byte for byte; docs/DESIGN.md records where each
# array comes from and why it must be reproduced verbatim.  They are spelled
# out HERE, independently of src/usb_desc.c, on purpose: a check that read its
# expectation out of the file it is checking would prove nothing.
# --------------------------------------------------------------------------
USB_DESC_DEVICE = bytes.fromhex(
    "12" "01" "1001" "ff" "00" "02" "08"     # len, type, bcdUSB, class/sub/proto, ep0
    "861a" "2375" "0403"                     # idVendor, idProduct, bcdDevice
    "00" "00" "00" "01"                      # iMfr, iProd, iSerial, bNumConfigurations
)
USB_DESC_CONFIG = bytes.fromhex(
    "09" "02" "2700" "01" "01" "00" "80" "f0"    # configuration, wTotalLength 39
    "09" "04" "00" "00" "03" "ff" "01" "02" "00" # interface 0, vendor FF/01/02
    "07" "05" "82" "02" "2000" "00"              # EP 0x82 bulk IN  32 B
    "07" "05" "02" "02" "2000" "00"              # EP 0x02 bulk OUT 32 B
    "07" "05" "81" "03" "0800" "01"              # EP 0x81 int IN    8 B (never driven)
)
# Thirteen independent 2-byte replies handed out in call order to successive
# bmRequestType==0xC0 requests.  Not one blob, and not decodable — see
# the descriptor analysis.  Byte order is the order delivered into ep0buf[0..1].
USB_CH340_VENDOR_TBL = bytes.fromhex(
    "3000" "c300" "ffec" "9fec" "ffec" "dfec" "dfec"
    "dfec" "9fec" "9fec" "9fec" "9fec" "ffec"
)
USB_VID = 0x1A86
USB_PID = 0x7523

# Cortex-M0 NVIC.  The stage-3 design constraint in one line: ICER/ICPR yes,
# ISER NEVER.  An enabled IRQ6 (USB) would take vector 22, which the
# bootloader's table trampolines into the APPLICATION's table at 0x4000 — and
# during an update that table is erased, so the fetch returns 0xFFFFFFFF, the
# range guard in bl_vector_forward rejects it and the core spins forever with
# CodeFlash unlocked.  That is the failure this check exists to prevent.
NVIC_ISER = 0xE000E100
NVIC_ICER = 0xE000E180
NVIC_ICPR = 0xE000E280

# --------------------------------------------------------------------------
# Stage 4/5/6 constants.
# --------------------------------------------------------------------------
# The writable window flash.c enforces, from include/flash.h.  Section 6 proves
# the linked image really implements exactly this half-open interval, which is
# what "the write floor is still a guard constant" has to mean to be worth
# checking at all.
FLASH_WRITE_FLOOR = 0x00003E00
FLASH_END         = 0x0003E800

# THE OTHER TWO WINDOWS, which are the same interval enforced by two more
# guards that do not share a line of code with flash.c's.
#
#   proto.c   range_ok()             [BL_IMAGE_BASE,    BL_IMAGE_END)
#   boot.c    bl_update_flash_read() [BL_BOOTINFO_BASE, BL_CODEFLASH_END)
#
# All three intervals are [0x3E00, 0x3E800) today and they are DELIBERATELY
# spelled separately here, from the three different headers, so that a change
# to one of them fails rather than being absorbed by the others.
#
# Why these need their own binary gates: both guards were written as a single
# `len > END - addr` test with no bound on addr, which for any addr past END
# lets the unsigned subtraction wrap to nearly 4 GB so that EVERY length
# passes.  On the read path there is no third layer behind it — proto.c range
# checks, boot.c range checks, and flash.c is not involved at all — and the
# failure it admits is the one unrecoverable fault in this design: a read of
# unmapped space is a BusFault -> HardFault -> a trampoline into the
# application vector table the update in progress has already erased -> a spin
# in .Lhang with CodeFlash unlocked, recoverable only with the H1 jumper.
#
# Both were fixed by adding an explicit `addr >= END` test ABOVE the
# subtraction.  Section 9(f) is what makes reverting either of them fail the
# build instead of leaving `make check` green: the fixed form folds the two
# address tests into one unsigned window test whose literal pool holds the
# NEGATED floor and the span, and the single-subtraction form emits neither.
PROTO_IMAGE_BASE  = 0x00003E00
PROTO_IMAGE_END   = 0x0003E800
READ_FLOOR        = 0x00003E00     # BL_BOOTINFO_BASE
READ_END          = 0x0003E800     # BL_CODEFLASH_END

# GPIO port B, from bl_config.h.  PB12 is the activity LED.
# The stage-2 observable is "LED lit ~500 ms after power-on == the APPLICATION
# booted", so nothing on the handoff path may touch these two registers — only
# led.c, and led.c may only be entered from update mode.
R32_PB_OUT = 0x400010C8
R32_PB_CLR = 0x400010CC

# ARMv6-M application interrupt and reset control, and the write key that makes
# a SYSRESETREQ take.  Present exactly once, in the handover.
SCB_AIRCR         = 0xE000ED0C
AIRCR_SYSRESETREQ = 0x05FA0004

# ARMv6-M SysTick (B3.3).  Exception 15, NOT an NVIC line — which is exactly
# why the timebase module is allowed to use it at all.  The bootloader runs
# SysTick as a FREE-RUNNING COUNTER with its interrupt disabled: TICKINT
# (SYST_CSR bit 1) must never be set, because an enabled SysTick exception
# takes vector 15, and vector 15 — like every entry 3..35 — trampolines into
# the APPLICATION's table at 0x4000, which is ERASED during an update.  That is
# the same failure mode as NVIC_ISER, reached through a different door.
SYST_CSR = 0xE000E010
SYST_RVR = 0xE000E014
SYST_CVR = 0xE000E018

CSR_ENABLE    = 1 << 0
CSR_TICKINT   = 1 << 1          # NEVER SET
CSR_CLKSOURCE = 1 << 2

# The only two values the bootloader ever writes to SYST_CSR: 5 = run
# (ENABLE|CLKSOURCE, TICKINT clear) and 0 = stop.  Section 10 asserts that no
# function which touches SYST_CSR ever materialises a CSR-shaped immediate with
# TICKINT in it — i.e. none of 2, 3, 6 or 7.  7 is what the shipping
# APPLICATION's systick_init writes; it must never appear here.
CSR_ALLOWED   = {0, CSR_ENABLE, CSR_ENABLE | CSR_CLKSOURCE}
CSR_FORBIDDEN = {v for v in range(8) if v & CSR_TICKINT}

# Where .bss must stay clear of, and where it must not grow into.
UPDATE_MAGIC_ADDR = 0x20000090
NOINIT_END        = 0x200000A0

# Literals ALLOWED to fall inside the application-space window.  Each is a
# deliberate, documented pointer or constant; anything not listed is a hard
# failure, which is the point of the check.  Every value PRESENT must be listed
# — a dead entry stops the list being a record of what is in the image.
APPSPACE_WHITELIST = {
    0x00003E00: "boot-info record base (BL_BOOTINFO_BASE)",
    0x00003E02: "boot-info +0x02, the \"LFBG\" name field",
    0x00003E03: "boot-info +0x03, inside the name field (byte compare)",
    0x00003E04: "boot-info +0x04, inside the name field (byte compare)",
    0x00003E05: "boot-info +0x05, inside the name field (byte compare)",
    0x00003E06: "boot-info +0x06, application CRC16",
    0x00003E08: "boot-info +0x08, application length",
    0x00003E0C: "boot-info +0x0C, record CRC16",
    0x00003E10: "boot-info +0x10, one past the 14-byte record (loop bound)",
    0x00004000: "application vector table base — the dispatcher's whole purpose",
    0x00004004: "application reset vector, read by bl_jump_to_app()",
    0x00005555: "BL_BI_MARKER_STAMPED — a VALUE compared against, never a "
                "pointer and never written (hard constraint 5)",
}


class Checker:
    def __init__(self):
        self.failures = []
        self.warnings = []
        self.n = 0

    def check(self, ok, name, detail=""):
        self.n += 1
        tag = "PASS" if ok else "FAIL"
        print(f"  [{tag}] {name}")
        if detail:
            for line in str(detail).splitlines():
                print(f"         {line}")
        if not ok:
            self.failures.append(name)
        return ok

    def warn(self, name, detail=""):
        print(f"  [WARN] {name}")
        if detail:
            for line in str(detail).splitlines():
                print(f"         {line}")
        self.warnings.append(name)


def section(title):
    print()
    print(title)
    print("-" * len(title))


def hexo(v):
    """Format an optional address for a detail line.

    Several checks compare against a symbol that may be missing.  The
    comparison itself already fails on None, but formatting None with `:08X`
    raises TypeError and turns a clean FAIL into a traceback, which reads like
    a broken checker rather than a broken image.
    """
    return "ABSENT" if v is None else f"0x{v:08X}"


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def parse_symbols(nm, elf):
    """symbol name -> address"""
    out = run([nm, "-n", elf])
    syms = {}
    for line in out.splitlines():
        m = re.match(r"^([0-9a-fA-F]+)\s+(\S)\s+(\S+)$", line.strip())
        if m:
            syms[m.group(3)] = int(m.group(1), 16)
    return syms


def parse_disasm(objdump, elf):
    """Returns (entries, literals).

    entries  — list of (addr, symbol, mnemonic, operands, raw_hex)
    literals — list of (addr, symbol, value) for every literal-pool .word
    """
    out = run([objdump, "-d", "-z", elf])
    entries = []
    literals = []
    cur = "?"
    sym_re = re.compile(r"^([0-9a-f]+)\s+<(.+)>:$")
    ins_re = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f ]+?)\s{2,}(\S+)\s*(.*)$")
    for line in out.splitlines():
        m = sym_re.match(line.strip())
        if m:
            cur = m.group(2)
            continue
        m = ins_re.match(line.rstrip())
        if not m:
            continue
        addr = int(m.group(1), 16)
        raw = m.group(2).replace(" ", "")
        mnem = m.group(3)
        ops = m.group(4).split("@")[0].strip()
        entries.append((addr, cur, mnem, ops, raw))
        if mnem == ".word":
            literals.append((addr, cur, int(ops, 16)))
    return entries, literals


def window_evidence(literals, sym, floor, end):
    """Is the half-open window [floor, end) pinned by `sym`'s literal pool?

    Returns (ok, detail) — detail is printed either way, because on a failure
    the pool itself is the only thing that tells the reader what the compiler
    actually did.

    THIS IS ONE FUNCTION AND NOT THREE COPIES ON PURPOSE.  Three separate
    guards enforce this same interval, in three modules that share no code —
    flash.c's range_writable(), proto.c's range_ok() and boot.c's
    bl_update_flash_read() — and each is checked here.  Three hand-written
    copies of the recognition logic would be three places for the check to rot
    independently of the code it checks.

    Each guard is written as three separate tests:

        addr <  floor          -> reject
        addr >= end            -> reject
        len  >  end - addr     -> reject

    and GCC folds the first two into one unsigned window test,

        (addr - floor) > (end - floor - 1)   -> reject

    so the literal pool holds the NEGATED floor and the SPAN rather than the
    floor and the end.  Both shapes are accepted and BOTH pin the same
    interval: change the floor or the end and one of these words changes.

    What makes this worth the trouble is what it REJECTS.  Drop the middle test
    and the guard becomes `addr < floor` plus a bare subtraction; on this
    target GCC then materialises both constants with movs/lsls immediates and
    the literal pool holds NEITHER pair — so the single-subtraction form, which
    is the exact defect these three guards were fixed for, fails here.

    If a future compiler emits a third legitimate shape this FAILS and prints
    the pool.  Read it, confirm it still means [floor, end), then teach this
    function the shape.  Do not delete the check to make it pass.
    """
    span = end - floor - 1
    neg_floor = (-floor) & 0xFFFFFFFF
    lits = [v for (_a, s, v) in literals if s == sym]
    explicit = floor in lits and end in lits
    folded = neg_floor in lits and span in lits
    if folded:
        return True, (f"{sym}: folded window test — "
                      f"-0x{floor:04X} = 0x{neg_floor:08X} and "
                      f"span 0x{span:05X} both present")
    if explicit:
        return True, (f"{sym}: explicit — 0x{floor:04X} and 0x{end:05X} "
                      f"both present")
    return False, (f"{sym} literal pool holds "
                   + (", ".join(f"0x{v:08X}" for v in lits) or "NOTHING")
                   + f"\nwanted either 0x{floor:04X} + 0x{end:05X} (explicit) "
                     f"or 0x{neg_floor:08X} + 0x{span:05X} (folded)")


def header_defines(path):
    """Simple `#define NAME <integer literal>` pairs from a C header.

    Deliberately naive: it reads only two body shapes, both unambiguous —
    a plain hex/decimal literal, and the `(1u << N)` bit-position idiom the
    register headers use.  Anything else expression-shaped is SKIPPED rather
    than guessed at, so a caller either gets a number the header really states
    or gets nothing at all; the caller must treat "missing" as a failure, not
    as agreement.
    """
    out = {}
    if not os.path.exists(path):
        return out
    num = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+"
                     r"\(?\s*(0[xX][0-9a-fA-F]+|\d+)\s*[uUlL]*\s*\)?\s*(?:/\*|//|$)")
    bit = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+"
                     r"\(\s*1[uUlL]*\s*<<\s*(\d+)[uUlL]*\s*\)\s*(?:/\*|//|$)")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = num.match(line)
        if m:
            out[m.group(1)] = int(m.group(2), 0)
            continue
        m = bit.match(line)
        if m:
            out[m.group(1)] = 1 << int(m.group(2))
    return out


def code_byte_map(entries):
    """offset -> the byte the ELF's disassembly says lives there, for every
    offset objdump decoded as an INSTRUCTION.

    Only sections objdump -d disassembles are covered, and .word literal pools
    are deliberately excluded.  In this image that means: .text instruction
    bytes and the 2-byte alignment pads between functions (objdump decodes a
    0x0000 pad as `movs r0, r0`) are in the map; literal pools, .vectors,
    .rodata and .data are NOT — they are DATA sections, objdump -d never sees
    them, so nothing there can be exempted by this map.

    Section 4(b) uses it to tell a real stray pointer from an INSTRUCTION
    ALIAS: two adjacent Thumb halfwords whose little-endian concatenation
    happens to land in application space.

    It maps to VALUES, not just to a set of offsets, and that is load-bearing.
    The `--bin`/`--elf` pair is not required to agree — one of this file's own
    controls patches the .bin and feeds it the genuine .elf precisely so that
    only the raw scans can catch the tamper.  An exemption keyed on position
    alone would hand that control a blind spot: patch a pointer over four code
    bytes and the scan would skip it.  Requiring the bytes to MATCH what the
    ELF decoded means a tampered word is never exempt, because a tampered word
    is by definition not the instruction the ELF says is there.

    objdump prints Thumb encodings as space-separated 16-bit halfwords in
    numeric order (`f3ef 8005`), while the file stores each halfword
    little-endian, so the byte order is swapped within each halfword pair.
    """
    code = {}
    for (addr, _sym, mnem, _ops, raw) in entries:
        if mnem == ".word" or len(raw) % 4 != 0:
            continue
        for h in range(len(raw) // 4):            # one halfword at a time
            hw = int(raw[h * 4:h * 4 + 4], 16)
            code[addr + h * 2]     = hw & 0xFF    # low byte first on disk
            code[addr + h * 2 + 1] = (hw >> 8) & 0xFF
    return code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--objdump", default="arm-none-eabi-objdump")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--keep-unused", type=int, default=1,
                    help="mirrors the Makefile's KEEP_UNUSED knob: with 1 the "
                         "not-yet-called public API is required to be present, "
                         "with 0 its absence is a warning")
    ap.add_argument("--dry-run-build", type=int, default=0,
                    help="mirrors `make EXTRA_CFLAGS=-DBL_DRY_RUN`.  A dormant "
                         "safety valve: it permits section 9(c2)'s window "
                         "checks to degrade to warnings IF that variant ever "
                         "inlines proto.c's flash wrappers away, and only "
                         "after the image has been confirmed to carry the "
                         "dry-run fingerprint, so the flag cannot wave through "
                         "a shipping build whose guard has been reverted.  "
                         "arm-none-eabi-gcc 15.3.Rel1 does NOT inline them, so "
                         "on this toolchain the flag changes nothing — see the "
                         "note at section 9(c2)")
    args = ap.parse_args()

    img = open(args.bin, "rb").read()
    syms = parse_symbols(args.nm, args.elf)
    entries, literals = parse_disasm(args.objdump, args.elf)

    c = Checker()
    print("=" * 74)
    print("GBFlash CH579 bootloader — offline image check")
    print(f"  image : {args.bin}  ({len(img)} bytes)")
    print(f"  elf   : {args.elf}")
    print("=" * 74)

    # ----------------------------------------------------------------- 1
    section("1. Size")
    c.check(len(img) <= FLASH_BUDGET,
            f"image fits the bootloader region (<= {FLASH_BUDGET} bytes)",
            f"{len(img)} bytes used, {FLASH_BUDGET - len(img)} bytes free, "
            f"{100 * len(img) / FLASH_BUDGET:.1f}% of budget")
    c.check(len(img) >= VECTOR_TABLE_SIZE,
            "image is at least one vector table long",
            f"{len(img)} >= {VECTOR_TABLE_SIZE}")

    # ----------------------------------------------------------------- 2
    section("2. Vector table")
    vec = list(struct.unpack_from("<%dI" % VECTOR_COUNT, img, 0))

    vs, ve = syms.get("__vectors_start"), syms.get("__vectors_end")
    c.check(vs == 0 and ve == VECTOR_TABLE_SIZE,
            f"vector table is {VECTOR_COUNT} entries / 0x{VECTOR_TABLE_SIZE:02X} bytes",
            f"__vectors_start={hexo(vs)} __vectors_end={hexo(ve)}")

    c.check(vec[0] == STACK_TOP,
            "word 0 (initial MSP) == 0x20008000",
            f"got 0x{vec[0]:08X}")

    reset = vec[1]
    c.check(reset & 1 == 1,
            "word 1 (reset vector) has the Thumb bit set",
            f"got 0x{reset:08X}")
    c.check((reset & ~1) < FLASH_BUDGET,
            "word 1 points inside the bootloader region 0x0000..0x3DFF",
            f"target 0x{reset & ~1:08X} < 0x{FLASH_BUDGET:04X}")
    c.check(syms.get("Reset_Handler") == (reset & ~1),
            "word 1 resolves to the Reset_Handler symbol",
            f"Reset_Handler = 0x{syms.get('Reset_Handler', 0):08X}")

    # Entry 0 is the initial stack pointer, not a vector: it is exempt from the
    # Thumb-bit rule (and is even by definition).  Entries 1..35 are code.
    bad_thumb = [(i, v) for i, v in enumerate(vec) if i >= 1 and v != 0 and (v & 1) == 0]
    c.check(not bad_thumb,
            "every non-zero vector entry 1..35 has the Thumb bit set",
            "all clear (entry 0 is the MSP and is exempt)" if not bad_thumb
            else "\n".join(f"entry {i}: 0x{v:08X}" for i, v in bad_thumb))
    zeros = [i for i, v in enumerate(vec) if i >= 1 and v == 0]
    c.check(not zeros, "no vector entry 1..35 is zero",
            "all populated" if not zeros else f"zero at {zeros}")

    disp = syms.get("bl_vector_forward")
    nmi = syms.get("bl_nmi_handler")
    c.check(disp is not None, "bl_vector_forward symbol exists",
            f"0x{disp:08X}" if disp is not None else "missing")
    c.check(nmi is not None, "bl_nmi_handler symbol exists",
            f"0x{nmi:08X}" if nmi is not None else "missing")
    if disp is not None and nmi is not None:
        # Entry 2 (NMI) is deliberately NOT the shared trampoline: it first
        # locks CodeFlash, then falls through into it.  Entries 3..35 enter the
        # dispatcher directly.
        c.check(vec[2] == (nmi | 1),
                "entry 2 (NMI) points at bl_nmi_handler (Thumb bit set)",
                f"got 0x{vec[2]:08X}, want 0x{nmi | 1:08X}")
        want = (disp | 1)
        wrong = [(i, v) for i, v in enumerate(vec[3:], start=3) if v != want]
        c.check(not wrong,
                "entries 3..35 all point at the dispatcher (Thumb bit set)",
                f"all 33 entries == 0x{want:08X}" if not wrong
                else "\n".join(f"entry {i}: 0x{v:08X} != 0x{want:08X}" for i, v in wrong))
        c.check(disp % 4 == 0,
                "dispatcher is 4-byte aligned (PC-relative literal pool)",
                f"0x{disp:08X}")
        c.check(nmi + len(NMI_BYTES) == disp,
                "bl_nmi_handler falls through into the dispatcher",
                f"0x{nmi:08X} + {len(NMI_BYTES)} == 0x{disp:08X}")
        got_nmi = img[nmi:nmi + len(NMI_BYTES)]
        c.check(got_nmi == NMI_BYTES,
                f"bl_nmi_handler machine code is the expected {len(NMI_BYTES)} bytes",
                f"want {NMI_BYTES.hex()}\ngot  {got_nmi.hex()}")

    # ----------------------------------------------------------------- 3
    section("3. Dispatcher encoding")
    if disp is not None and disp + len(DISPATCHER_BYTES) <= len(img):
        got = img[disp:disp + len(DISPATCHER_BYTES)]
        c.check(got == DISPATCHER_BYTES,
                f"dispatcher machine code is the expected {len(DISPATCHER_BYTES)} bytes",
                f"want {DISPATCHER_BYTES.hex()}\ngot  {got.hex()}")
        # And that the bytes actually mean what we think they mean.
        seq = [(a, m, o) for (a, s, m, o, _r) in entries
               if disp <= a < disp + len(DISPATCHER_BYTES)]
        # strip the Thumb width suffix (`beq.n` -> `beq`) but keep directives
        # like `.word` intact.
        mnems = [m if m.startswith(".") else m.split(".")[0]
                 for (_a, m, _o) in seq]
        c.check(mnems == DISPATCHER_MNEMONICS + [".word", ".word"],
                "dispatcher disassembles to the expected instruction sequence",
                "\n".join(f"0x{a:04X}  {m:6s} {o}" for a, m, o in seq))
        lit = [v for (a, _s, v) in literals if disp <= a < disp + len(DISPATCHER_BYTES)]
        c.check(lit == DISPATCHER_LITERALS,
                "dispatcher literals are the application vector base 0x00004000 "
                "and R8_FLASH_PROTECT",
                f"literals in range: {[hex(x) for x in lit]}")
        # No register other than r0/r1 may be written: r4-r11 are not stacked.
        clob = [(a, m, o) for (a, m, o) in seq
                if m not in (".word", "nop", "b", "beq", "bne", "bx")
                and re.match(r"^r(?!0\b|1\b)\d+", o)]
        c.check(not clob,
                "dispatcher clobbers only r0/r1 (r4-r11 are not exception-stacked)",
                "verified" if not clob else str(clob))
    else:
        c.check(False, "dispatcher present in image", "symbol missing or out of range")

    # ----------------------------------------------------------------- 4
    section("4. No stray pointers into application space "
            f"(0x{APPSPACE_LO:04X}..0x{APPSPACE_HI:04X})")

    # (a) literal-pool words — precise, every one has a symbol and an address
    lit_hits = [(a, s, v) for (a, s, v) in literals if APPSPACE_LO <= v <= APPSPACE_HI]
    bad_lit = [(a, s, v) for (a, s, v) in lit_hits if v not in APPSPACE_WHITELIST]
    if lit_hits:
        print("  literal-pool words in range (all must be whitelisted):")
        for a, s, v in sorted(lit_hits, key=lambda t: t[2]):
            why = APPSPACE_WHITELIST.get(v, "*** NOT WHITELISTED ***")
            print(f"    0x{v:08X}  at 0x{a:04X} in {s:<24s} {why}")
    c.check(not bad_lit,
            "every literal-pool word in application space is a documented constant",
            f"{len(lit_hits)} in range, all whitelisted" if not bad_lit
            else "\n".join(f"0x{v:08X} at 0x{a:04X} in {s}" for a, s, v in bad_lit))

    # (b) raw word-aligned scan of the whole image, vector table included.
    #
    #     This scan is the backstop for anything the literal-pool scan cannot
    #     see: .vectors, .rodata, .data — every DATA section, none of which
    #     objdump -d disassembles.  Its one weakness is INSTRUCTION ALIASING:
    #     two adjacent Thumb halfwords can concatenate to a word that looks
    #     like an application-space address without any pointer existing.  That
    #     is not hypothetical — `bx lr` (0x4770) followed by a 2-byte alignment
    #     pad (0x0000) reads back as 0x00004770, and proto.c's le32() ends
    #     exactly that way.
    #
    #     Whitelisting the VALUE would excuse a genuine pointer to 0x4770
    #     anywhere in the image, including in .rodata where this scan is the
    #     only check there is.  Instead a word is skipped only when all four of
    #     its bytes ARE, BYTE FOR BYTE, the instruction bytes the ELF's
    #     disassembly says belong there (see code_byte_map).  Data sections are
    #     not in that map, so the backstop keeps its full reach over
    #     .vectors/.rodata/.data; and a .bin patched behind the ELF's back no
    #     longer matches, so a planted pointer is still caught in the middle of
    #     the instruction stream.
    code = code_byte_map(entries)
    raw = {}
    aliased = []
    for off in range(0, len(img) - 3, 4):
        v = struct.unpack_from("<I", img, off)[0]
        if not (APPSPACE_LO <= v <= APPSPACE_HI):
            continue
        if all(code.get(off + i) == img[off + i] for i in range(4)):
            aliased.append((off, v))
            continue
        raw.setdefault(v, []).append(off)
    bad_raw = {v: o for v, o in raw.items() if v not in APPSPACE_WHITELIST}
    if aliased:
        print("  in-range words skipped as instruction aliases "
              "(all 4 bytes decoded as code, no literal involved):")
        for off, v in aliased:
            sym = max((a, s) for (a, s, _m, _o, _r) in entries if a <= off)[1]
            print(f"    0x{v:08X}  at 0x{off:04X} inside {sym}")
    c.check(not bad_raw,
            "raw word-aligned scan of the .bin finds no un-whitelisted value in range",
            f"{len(raw)} distinct values in data, all whitelisted; "
            f"{len(aliased)} instruction aliases skipped"
            if not bad_raw else
            "\n".join(f"0x{v:08X} at offsets {[hex(x) for x in o]}"
                      for v, o in sorted(bad_raw.items())))

    # ----------------------------------------------------------------- 5
    section("5. CH579 user configuration word / InfoFlash")

    # (a) no literal anywhere near InfoFlash
    info_lit = [(a, s, v) for (a, s, v) in literals if INFOFLASH_LO <= v <= INFOFLASH_HI]
    c.check(not info_lit,
            f"no literal in InfoFlash 0x{INFOFLASH_LO:08X}..0x{INFOFLASH_HI:08X}",
            "none" if not info_lit
            else "\n".join(f"0x{v:08X} at 0x{a:04X} in {s}" for a, s, v in info_lit))

    raw_info = [off for off in range(0, len(img) - 3, 4)
                if INFOFLASH_LO <= struct.unpack_from("<I", img, off)[0] <= INFOFLASH_HI]
    c.check(not raw_info,
            "raw scan finds no InfoFlash address in the image",
            "none" if not raw_info else f"offsets {[hex(x) for x in raw_info]}")

    cfg_hits = [off for off in range(0, len(img) - 3, 4)
                if struct.unpack_from("<I", img, off)[0] == USER_CFG_WORD]
    c.check(not cfg_hits,
            f"the user configuration word 0x{USER_CFG_WORD:08X} appears nowhere in the image",
            "absent" if not cfg_hits else f"offsets {[hex(x) for x in cfg_hits]}")

    # (b) the only thing that can actually clear CFG_BOOT_EN is writing 0x8C to
    #     R8_FLASH_PROTECT.  Enumerate every 8-bit immediate materialised
    #     anywhere in the image, then require 0x8C never to appear in a function
    #     that could reach that register.
    #
    #     SCOPING, and why: R8_FLASH_PROTECT is 0x40001809, which no ARMv6-M
    #     instruction can materialise as an immediate — reaching it requires the
    #     address in a register, and every site in this image forms it from its
    #     own literal pool (verified by the R8_FLASH_PROTECT listing printed
    #     below, and by the fact that no function takes it as a parameter: grep
    #     src/ for 0x40001809).  So a `movs rN,#0x8C` inside a function whose
    #     pool does NOT contain that address cannot become a write to it.
    #
    #     This scoping is not cosmetic.  The unscoped form produced a real false
    #     failure: proto.c's feed_one materialises 0x8C as the high half of a
    #     struct offset (`movs r3,#140` / `lsls r3,r3,#3` = 0x460), which has
    #     nothing to do with flash protection.  Failing on that trains readers
    #     to ignore this gate, which is worse than not having it.  Immediates
    #     outside the flash-touching functions are still listed, as INFO.
    imm8 = {}
    imm_re = re.compile(r"^r\d+,\s*#(\d+)$")
    for (a, s, m, o, _r) in entries:
        if m.startswith("movs") or m.startswith("mov"):
            mm = imm_re.match(o)
            if mm:
                imm8.setdefault(int(mm.group(1)), []).append((a, s))

    # Functions whose literal pool contains R8_FLASH_PROTECT — the only ones
    # that can store to it.
    protect_fns = {s for (_a, s, v) in literals if v == R8_FLASH_PROTECT}
    hits_8c = imm8.get(FLASH_UNLOCK_INFOFLASH, [])
    dangerous = [(a, s) for (a, s) in hits_8c if s in protect_fns]
    c.check(not dangerous,
            "the InfoFlash unlock value 0x8C is never materialised in a function "
            "that can reach R8_FLASH_PROTECT",
            ("absent — CFG_BOOT_EN cannot be cleared by this image"
             + ("" if not hits_8c else
                "\nunrelated 0x8C immediates (no R8_FLASH_PROTECT in their pool): "
                + ", ".join(f"0x{a:04X} in {s}" for a, s in hits_8c)))
            if not dangerous
            else "\n".join(f"0x{a:04X} in {s}" for a, s in dangerous))
    c.check(all(v != FLASH_UNLOCK_INFOFLASH for (_a, _s, v) in literals),
            "0x8C is not present as a literal-pool word either")
    if FLASH_UNLOCK_DATAFLASH in imm8:
        c.warn("the DataFlash unlock value 0x84 is materialised somewhere",
               str(imm8[FLASH_UNLOCK_DATAFLASH]))

    protect_lit = [(a, s) for (a, s, v) in literals if v == R8_FLASH_PROTECT]
    print(f"  R8_FLASH_PROTECT (0x{R8_FLASH_PROTECT:08X}) referenced from "
          f"{len(protect_lit)} literal pool(s):")
    for a, s in protect_lit:
        print(f"    0x{a:04X} in {s}")
    print("  8-bit immediates materialised anywhere in the image: "
          + ", ".join(f"0x{v:02X}" for v in sorted(imm8)))

    # ----------------------------------------------------------------- 6
    section("6. Write-floor and self-protection sanity")
    # Nothing in the image may name a flash address below 0x3E00 as a write
    # target.  We cannot prove intent statically, but we can prove the driver's
    # floor constant is present and that no literal names a low flash address
    # in a flash-driver function.
    # THE WRITE FLOOR, AS THE LINKED IMAGE ACTUALLY IMPLEMENTS IT.  A bare
    # "is 0x3E00 a literal anywhere" test says "no" forever — every stage-4
    # build folds the floor into an arithmetic form — so the fold itself is
    # what is recognised, and it pins the window far more tightly.
    #
    # flash.c's range_writable() is written as three separate tests and GCC
    # folds two of them into one unsigned window test; window_evidence()
    # recognises both shapes and explains itself.  proto.c's range_ok() and
    # boot.c's bl_update_flash_read() get the identical treatment in section
    # 9(f) — three independent guards over the same interval, three separate
    # gates, one recognition function.
    c.check("range_writable" in syms,
            "flash.c's shared range guard range_writable() is in the image",
            f"0x{syms.get('range_writable', 0):08X}")
    ok, detail = window_evidence(literals, "range_writable",
                                 FLASH_WRITE_FLOOR, FLASH_END)
    c.check(ok,
            "the writable window is pinned to exactly "
            f"[0x{FLASH_WRITE_FLOOR:04X}, 0x{FLASH_END:05X}) by constants in "
            "range_writable()",
            detail)

    # And that every entry point really goes through it.  A guard nothing calls
    # is the failure mode this catches: bl_flash_program_word() is a public
    # entry point in its own right, so "bl_flash_program checks the range" is
    # not enough.
    def callees_of(sym):
        return {t for (_a, s, m, ops, _r) in entries
                if s == sym and m.startswith("bl") and "<" in ops
                for t in [ops[ops.index("<") + 1:ops.rindex(">")].split("+")[0]]}

    #
    # ALL FOUR must be PRESENT as well as guarded.  `f in syms and ...` alone
    # made the check vacuous for any entry point that had left the image: an
    # absent function is trivially not unguarded.  Every one of them is reached
    # in this build (the first two through bl_update_flash_ops, program_word
    # from bl_flash_program, check_erased from bl_flash_erase_sector), so an
    # absence is a real change and should be read as one.
    guarded = ["bl_flash_erase_sector", "bl_flash_program",
               "bl_flash_program_word", "bl_flash_check_erased"]
    absent = [f for f in guarded if f not in syms]
    unguarded = [f for f in guarded
                 if f in syms and "range_writable" not in callees_of(f)]
    c.check(not absent and not unguarded,
            "every flash entry point is in the image and calls range_writable() "
            "before touching the controller",
            ", ".join(f"{f} -> range_writable" for f in guarded)
            if not absent and not unguarded else
            (f"ABSENT from the image: {absent}\n" if absent else "")
            + (f"NOT guarded: {unguarded}" if unguarded else ""))

    lowflash = [(a, s, v) for (a, s, v) in literals
                if 0 < v < BOOTINFO_BASE and s.startswith("bl_flash")]
    c.check(not lowflash,
            "no flash-driver literal names an address below 0x3E00",
            "none" if not lowflash
            else "\n".join(f"0x{v:08X} at 0x{a:04X} in {s}" for a, s, v in lowflash))

    # ----------------------------------------------------------------- 7
    section("7. Link hygiene")
    undef = run([args.nm, "-u", args.elf]).strip()
    c.check(undef == "", "no unresolved symbols", undef or "none")

    # ld/bootloader.ld defines every symbol below UNCONDITIONALLY.  These were
    # once written as `if sym is not None:` and that is the wrong shape for a
    # gate: deleting the PROVIDE from the linker script would not fail the
    # check, it would DELETE the check, and `make check` would go on reporting a
    # green count one smaller than the run before.  A missing symbol is now the
    # failure it actually is.
    exidx_start = syms.get("__exidx_start")
    exidx_end = syms.get("__exidx_end")
    c.check(exidx_start is not None and exidx_end is not None
            and exidx_end == exidx_start,
            ".ARM.exidx is empty (no unwind tables linked in)",
            f"{hexo(exidx_start)}..{hexo(exidx_end)}")

    bss_start = syms.get("__bss_start")
    data_start = syms.get("__data_start")
    c.check(bss_start is not None and bss_start >= NOINIT_END,
            "the .bss zeroing loop cannot reach the 0x20000090 update magic",
            f"__bss_start = {hexo(bss_start)} >= 0x{NOINIT_END:08X}")
    c.check(data_start is not None and data_start >= NOINIT_END,
            ".data starts above the reserved no-init hole",
            f"__data_start = {hexo(data_start)}")

    end = syms.get("__flash_image_end")
    c.check(end is not None and end == len(img),
            "__flash_image_end matches the .bin length",
            f"{hexo(end)} vs {len(img)} bytes")

    # ----------------------------------------------------------------- 8
    section("8. Stage 3 — polled USB (CH340 emulation)")

    # (a) The layer is actually linked in and actually reached.  --gc-sections
    #     would happily discard the whole thing if bl_update_mode were still the
    #     weak spinning stub, and the image would still pass every other check
    #     in this file while doing nothing at all.
    core_syms = ["bl_usb_init", "bl_usb_poll", "bl_usb_rx",
                 "bl_usb_desc_device", "bl_usb_desc_config",
                 "bl_usb_ch340_vendor_tbl"]
    missing = [s for s in core_syms if s not in syms]
    c.check(not missing,
            "the USB layer is linked into the image",
            ", ".join(f"{s}=0x{syms[s]:08X}" for s in core_syms) if not missing
            else f"missing: {missing}")

    # bl_usb_tx / bl_usb_configured have no caller until stage 4 replaces the
    # echo pump, so they survive only because the Makefile's KEEP_UNUSED list
    # names them.  Under KEEP_UNUSED=0 --gc-sections is entitled to drop them
    # and that is not a defect, so it warns rather than fails.
    api_syms = ["bl_usb_tx", "bl_usb_configured"]
    gone = [s for s in api_syms if s not in syms]
    if args.keep_unused:
        c.check(not gone,
                "the stage-4 USB API survives the link (KEEP_UNUSED=1)",
                ", ".join(f"{s}=0x{syms[s]:08X}" for s in api_syms) if not gone
                else f"dropped by --gc-sections: {gone}")
    elif gone:
        c.warn("stage-4 USB API dropped by --gc-sections (KEEP_UNUSED=0)",
               ", ".join(gone) + " — expected with this knob; the reported "
               "image size is smaller than stage 4's will be")

    # Update mode must actually bring USB up.  Asked from the call site rather
    # than the symbol table, because "bl_usb_init exists" and "something calls
    # it" are different claims and only the second one matters — the weak
    # spinning stub from stage 0 would satisfy the first.
    #
    # bl_update_mode() is a single call site and GCC is entitled to inline it
    # into bl_main(), which would delete the symbol without changing behaviour;
    # accepting either caller keeps this a behavioural check rather than a
    # codegen check.
    def callers_of(target):
        return {s for (_a, s, m, ops, _r) in entries
                if m.startswith("bl") and f"<{target}>" in ops}

    ok_callers = {"bl_update_mode", "bl_main"}
    init_from = callers_of("bl_usb_init")
    poll_from = callers_of("bl_usb_poll")
    c.check(bool(init_from) and init_from <= ok_callers
            and bool(poll_from & ok_callers),
            "update mode brings USB up and services it (not the stage-0 stub)",
            f"bl_usb_init called from: {sorted(init_from) or 'NOTHING'}\n"
            f"bl_usb_poll called from: {sorted(poll_from) or 'NOTHING'}\n"
            "bl_update_mode = "
            + (f"0x{syms['bl_update_mode']:08X}" if "bl_update_mode" in syms
               else "inlined into its caller"))

    # (b) THE stage-3 design constraint.  See the NVIC_ISER comment above.
    iser_lit = [(a, s) for (a, s, v) in literals if v == NVIC_ISER]
    c.check(not iser_lit,
            f"NVIC_ISER (0x{NVIC_ISER:08X}) is referenced by NO literal pool — "
            "no interrupt can ever be enabled",
            "absent" if not iser_lit
            else "\n".join(f"0x{a:04X} in {s}" for a, s in iser_lit))
    iser_raw = [off for off in range(0, len(img) - 3, 4)
                if struct.unpack_from("<I", img, off)[0] == NVIC_ISER]
    c.check(not iser_raw,
            "raw word-aligned scan finds no NVIC_ISER address in the image either",
            "absent" if not iser_raw else f"offsets {[hex(x) for x in iser_raw]}")
    have_icer = any(v == NVIC_ICER for (_a, _s, v) in literals)
    have_icpr = any(v == NVIC_ICPR for (_a, _s, v) in literals)
    c.check(have_icer and have_icpr,
            "NVIC_ICER and NVIC_ICPR are present — IRQ6 is positively masked",
            f"ICER 0x{NVIC_ICER:08X}: {'yes' if have_icer else 'NO'}, "
            f"ICPR 0x{NVIC_ICPR:08X}: {'yes' if have_icpr else 'NO'}")

    # (c) Descriptors.  Located by symbol (file offset == address: the image is
    #     a flat load from 0x0000 and __flash_image_end == len(img), checked in
    #     section 7), then compared against the transcription above, then
    #     decoded — because "the bytes are there" and "the bytes mean
    #     1A86:7523" are different claims.
    def at(sym, n):
        a = syms.get(sym)
        if a is None or a + n > len(img):
            return None
        return img[a:a + n]

    dev = at("bl_usb_desc_device", len(USB_DESC_DEVICE))
    c.check(dev == USB_DESC_DEVICE,
            "device descriptor is present, verbatim from the application image",
            f"want {USB_DESC_DEVICE.hex()}\ngot  {dev.hex() if dev else 'ABSENT'}")
    if dev is not None and len(dev) == 18:
        vid, pid = struct.unpack_from("<HH", dev, 8)
        ok = (dev[0] == 18 and dev[1] == 1 and vid == USB_VID and pid == USB_PID
              and dev[7] == 8 and dev[17] == 1)
        c.check(ok,
                "device descriptor decodes to a CH340: "
                f"{USB_VID:04X}:{USB_PID:04X}",
                f"bLength={dev[0]} bDescriptorType={dev[1]} "
                f"idVendor=0x{vid:04X} idProduct=0x{pid:04X} "
                f"bMaxPacketSize0={dev[7]} bcdDevice=0x"
                f"{struct.unpack_from('<H', dev, 12)[0]:04X} "
                f"bNumConfigurations={dev[17]}")

    cfg = at("bl_usb_desc_config", len(USB_DESC_CONFIG))
    c.check(cfg == USB_DESC_CONFIG,
            "configuration descriptor set is present, verbatim",
            f"want {USB_DESC_CONFIG.hex()}\ngot  {cfg.hex() if cfg else 'ABSENT'}")
    if cfg is not None and len(cfg) == 39:
        wtotal = struct.unpack_from("<H", cfg, 2)[0]
        # 9 config + 9 interface, then three 7-byte endpoint descriptors;
        # bEndpointAddress is at +2 of each, i.e. offsets 20, 27, 34.
        eps = (cfg[20], cfg[27], cfg[34])       # 0x82, 0x02, 0x81
        c.check(wtotal == 39 and cfg[4] == 1 and cfg[13] == 3
                and eps == (0x82, 0x02, 0x81),
                "configuration descriptor decodes: 1 interface, 3 endpoints, "
                "bulk 0x82 IN / 0x02 OUT",
                f"wTotalLength={wtotal} bNumInterfaces={cfg[4]} "
                f"bNumEndpoints={cfg[13]} endpoints="
                + " ".join(f"0x{e:02X}" for e in eps))

    tbl = at("bl_usb_ch340_vendor_tbl", len(USB_CH340_VENDOR_TBL))
    c.check(tbl == USB_CH340_VENDOR_TBL,
            "the 26-byte canned CH340 vendor-IN table is present, verbatim "
            "(13 replies in call order — it cannot be regenerated)",
            f"want {USB_CH340_VENDOR_TBL.hex()}\n"
            f"got  {tbl.hex() if tbl else 'ABSENT'}")

    # (d) RAM.  The USB layer added ~1 KB of .bss; re-assert both ends of it.
    #     The low end is the update magic at 0x20000090, which start.S's zeroing
    #     loop must never reach (that word is how the application asks for
    #     update mode, and it is read by bl_main BEFORE anything else runs).
    bss_end = syms.get("__bss_end")
    c.check(bss_start is not None and bss_start >= NOINIT_END,
            f"stage-3 .bss still starts above the no-init hole holding "
            f"0x{UPDATE_MAGIC_ADDR:08X}",
            f"__bss_start = {hexo(bss_start)} >= 0x{NOINIT_END:08X} "
            f"(magic at 0x{UPDATE_MAGIC_ADDR:08X} is "
            f"{NOINIT_END - UPDATE_MAGIC_ADDR} bytes below it)")
    c.check(bss_start is not None and bss_end is not None
            and bss_end <= STACK_TOP - 1024,
            "static RAM leaves at least 1 KB of stack below 0x20008000",
            f"__bss_end = {hexo(bss_end)}, "
            + (f"{STACK_TOP - bss_end} bytes to the stack top, "
               f"{bss_end - bss_start} bytes of .bss"
               if bss_start is not None and bss_end is not None else ""))

    # ----------------------------------------------------------------- 9
    section("9. Stages 4, 5 and 6 — protocol, flash driver and LED in update mode")

    # Everything here is a CALL-SITE claim, not a symbol-table claim.  The
    # Makefile's KEEP_UNUSED knob forces the whole stage-4/5/6 API to survive
    # the link whether or not anything reaches it, so "the symbol exists" was
    # true throughout stages 0-3 and proves nothing now.  What changed at stage
    # 4 is that these functions acquired callers.

    # (a) proto.c is driven by the update loop.
    proto_entry = ["bl_proto_reset", "bl_proto_bind", "bl_proto_feed"]
    proto_bad = [f for f in proto_entry if not (callers_of(f) & ok_callers)]
    c.check(not proto_bad,
            "update mode drives the protocol framer",
            "\n".join(f"{f} called from {sorted(callers_of(f))}"
                      for f in proto_entry)
            if not proto_bad else
            "\n".join(f"{f} called from {sorted(callers_of(f)) or 'NOTHING'}"
                      for f in proto_bad))

    # (b) STAGE-3 DEFECT 4, closed here and nowhere else.
    #
    #     proto.h calls bl_proto_idle() MANDATORY for the receive loop: without
    #     it a host killed part-way through a data frame leaves a legal header
    #     with up to 522 bytes outstanding, and the framer eats whatever
    #     arrives next to satisfy the count.  Both hosts send exactly two init
    #     frames at connect and then report "No device found.", so a reconnect
    #     can be swallowed whole and the device looks dead until it is
    #     unplugged.  test_proto measures the difference: without the idle
    #     notification the framer alone recovers 31 of 119 truncation points,
    #     with it all 119 recover inside one init frame.
    #
    #     Through stages 0-3 this function was linked (KEEP_UNUSED names it)
    #     and called by nothing, and the status log recorded it as satisfied.  That
    #     is exactly the kind of claim a symbol-table check would still rubber-
    #     stamp today, which is why this one asks for a caller.
    idle_from = callers_of("bl_proto_idle")
    c.check(bool(idle_from & ok_callers),
            "bl_proto_idle() HAS A CALLER — a truncated frame cannot swallow "
            "the reconnect (stage-3 defect 4)",
            f"called from {sorted(idle_from) or 'NOTHING — DEFECT 4 IS OPEN'}")

    # (c) STAGE 5/6 — the flash driver proto.c actually gets handed.
    #     The table is three function pointers in .rodata; read them out of the
    #     image and resolve them, because a table pointing at the wrong three
    #     functions would satisfy every other check in this file.
    ops_addr = syms.get("bl_update_flash_ops")
    if ops_addr is None or ops_addr + 12 > len(img):
        c.check(False, "bl_update_flash_ops is present in the image",
                "symbol missing — proto.c would be bound to nothing and every "
                "data packet would NAK")
    else:
        ptrs = struct.unpack_from("<3I", img, ops_addr)
        by_addr = {}
        for name, a in syms.items():
            by_addr.setdefault(a, []).append(name)

        def resolve(p):
            if p & 1 == 0:
                return None
            names = by_addr.get(p & ~1, [])
            # prefer the function name over any alias at the same address
            return names[0] if names else None

        got = [resolve(p) for p in ptrs]
        want = ["bl_flash_erase_sector", "bl_flash_program",
                "bl_update_flash_read"]
        c.check(got == want,
                "the flash ops bound into proto.c are the three real entry "
                "points, in order, with the Thumb bit set",
                "\n".join(f"  .{fld:<13s} 0x{p:08X} -> {n}"
                          for fld, p, n in
                          zip(["erase_sector", "program", "read"], ptrs, got)))

    # (c2) THE OTHER TWO RANGE GUARDS, PINNED THE SAME WAY SECTION 6 PINS
    #      flash.c's.
    #
    #      Section 6 has proved since stage 5 that flash.c's range_writable()
    #      really implements [0x3E00, 0x3E800) and that all four flash entry
    #      points call it.  Nothing did the same for the two guards ABOVE it,
    #      and those are the ones that had the defect: proto.c's range_ok() and
    #      boot.c's bl_update_flash_read() were both written as
    #
    #          if (addr < FLOOR)          return 0;
    #          if (len > END - addr)      return 0;      /* "incl. overflow" */
    #
    #      which is true only on the low side.  For any addr past END the
    #      unsigned subtraction wraps to nearly 4 GB and EVERY length passes.
    #      Both now carry an explicit `addr >= END` test above the subtraction.
    #
    #      This is the gate that makes removing it FAIL rather than leaving 63
    #      green checks: the fixed three-test form folds into an unsigned window
    #      test whose literal pool holds the negated floor and the span, and the
    #      two-test form emits neither (both constants become movs/lsls
    #      immediates).  See window_evidence().
    #
    #      The READ path is the one that matters most, and it is the one with
    #      the least behind it.  A write that slips a guard is caught by
    #      flash.c, which range-checks again and is the only code that can
    #      unlock the controller.  A READ has no third layer at all — proto.c
    #      checks, boot.c checks, and flash.c is not on the path — and an
    #      out-of-map read is a BusFault -> HardFault -> a trampoline into the
    #      application vector table the update in progress has already erased ->
    #      a spin in .Lhang with CodeFlash unlocked.  That is the one fault in
    #      this design that needs the H1 jumper to recover from.
    #      THE ONE EXEMPTION, AND WHY IT CANNOT BE ABUSED.
    #
    #      `make EXTRA_CFLAGS=-DBL_DRY_RUN` builds a diagnostic image in which
    #      fl_erase/fl_program/fl_read return BEFORE reaching the flash ops.
    #      That makes all three tiny enough for GCC to inline them into
    #      feed_one() and to inline range_ok() along with them, taking its
    #      literal pool — and this check's evidence — with it.  The guard is
    #      still applied; erase_ok()/program_ok() still run.  There is simply
    #      nothing left in the image to read them out of.
    #
    #      MEASURED, arm-none-eabi-gcc 15.3.Rel1.  In the SHIPPING build all
    #      four symbols are present (fl_program 0x718, fl_read 0x74E,
    #      fl_erase 0x790, range_ok 0x6F0).  In the dry-run build
    #      `arm-none-eabi-nm build/bootloader.elf` shows none of them, and
    #      `make EXTRA_CFLAGS=-DBL_DRY_RUN check` reports
    #      "RESULT: PASS — 65 checks, 0 failures, 2 warnings", the two warnings
    #      being the two this exemption downgrades.  So the exemption IS
    #      reached, on every dry-run build, and the check count is 65 rather
    #      than the shipping 68 because three image-level call-site claims
    #      cannot be made against an image that no longer carries them.
    #      Re-verify with `arm-none-eabi-nm` rather than believing this
    #      paragraph after a toolchain change.
    #
    #      From the image alone, "the dry-run build inlined range_ok" and "the
    #      guard was reverted and GCC inlined range_ok" look identical, so the
    #      exemption has to be told about (--dry-run-build) rather than
    #      inferred.  A flag that is simply believed would be a hole, so it is
    #      NOT simply believed: the exemption applies only once the image is
    #      confirmed to carry the dry-run fingerprint — ALL THREE of
    #      fl_erase/fl_program/fl_read gone.  A shipping build with the guard
    #      reverted keeps all three (they still call through the ops table), so
    #      passing the flag to one of those does not silence anything.
    proto_wrappers = ["fl_erase", "fl_program", "fl_read"]
    wrappers_present = [f for f in proto_wrappers if f in syms]
    dry_run_shaped = bool(args.dry_run_build) and not wrappers_present

    for sym, floor, end, whose in (
            ("range_ok", PROTO_IMAGE_BASE, PROTO_IMAGE_END,
             "proto.c's shared range guard"),
            ("bl_update_flash_read", READ_FLOOR, READ_END,
             "boot.c's flash-read op (the one bound into proto.c)")):
        if sym not in syms:
            if sym == "range_ok" and dry_run_shaped:
                c.warn("range_ok() was inlined away by -DBL_DRY_RUN, so its "
                       "window cannot be read from the image",
                       "fl_erase, fl_program and fl_read are all absent too, "
                       "which is the dry-run fingerprint: the wrappers return "
                       "before the flash ops, so GCC inlined the lot.  The "
                       "guard still runs, its constants are just movs/lsls "
                       "immediates now.  THE SHIPPING BUILD IS THE ONE THAT "
                       "MATTERS HERE and it is checked in full — this variant "
                       "never writes flash at all.")
                continue
            c.check(False,
                    f"{sym}() — {whose} — is in the image as its own symbol",
                    "SYMBOL ABSENT.  The likely cause is the defect this gate "
                    "exists for: DELETING the `addr >= END` test shrinks the "
                    "guard to two tests whose constants both fit in movs/lsls "
                    "immediates, GCC then inlines it into its callers, and its "
                    "literal pool disappears along with the window this check "
                    "reads.  Verified: that is exactly what the reverted form "
                    "does here.\n"
                    "Read `make disasm` before doing anything else.  If the "
                    "guard is genuinely intact and merely inlined, say in this "
                    "file which caller now owns the window — do not delete the "
                    "check to make it pass.")
            continue
        c.check(True, f"{sym}() — {whose} — is in the image as its own symbol",
                f"0x{syms[sym]:08X}")
        ok, detail = window_evidence(literals, sym, floor, end)
        c.check(ok,
                f"{sym}()'s window is pinned to exactly "
                f"[0x{floor:04X}, 0x{end:05X}) — the `addr >= END` test is "
                f"still there, not folded back into one subtraction",
                detail)

    #      And that proto.c really funnels every flash access through range_ok.
    #      A guard nothing calls is the failure mode this catches: proto.c has
    #      three separate wrappers and they are the only code allowed to reach
    #      the bl_flash_ops table.  callees_of() is section 6's, reused rather
    #      than re-declared — a second byte-identical copy stood here and was
    #      one more place for the recognition logic to rot independently.
    ungated = [f for f in wrappers_present if "range_ok" not in callees_of(f)]
    if dry_run_shaped:
        c.warn("proto.c's flash wrappers were inlined away by -DBL_DRY_RUN, so "
               "their call sites cannot be read from the image",
               "same fingerprint as above; nothing in this variant reaches the "
               "flash ops at all, which is the whole point of it")
    else:
        c.check(bool(wrappers_present) and not ungated,
                "every proto.c flash wrapper calls range_ok() before reaching "
                "the bl_flash_ops table",
                ", ".join(f"{f} -> range_ok" for f in wrappers_present)
                if wrappers_present and not ungated
                else (f"NOT gated: {ungated}" if wrappers_present
                      else "none of fl_erase/fl_program/fl_read survived as "
                           "symbols — they were inlined.  If this is a "
                           "-DBL_DRY_RUN build, pass --dry-run-build 1; "
                           "otherwise read `make disasm`, because the shipping "
                           "build has no reason to inline them."))

    # (d) The response path.  Stage 3 had no caller for bl_usb_tx at all (the
    #     echo pump used the internal queue); stage 4 is the first build in
    #     which a protocol reply can physically leave the device.
    tx_from = callers_of("bl_usb_tx")
    c.check(bool(tx_from & ok_callers),
            "update mode can transmit a protocol response (bl_usb_tx has a "
            "caller)",
            f"called from {sorted(tx_from) or 'NOTHING'}")

    # (e) THE LED, AND THE STAGE-2 OBSERVABLE IT MUST NOT BREAK.
    #
    #     "LED lit ~500 ms after power-on == the APPLICATION booted" is a
    #     diagnostic the device owner relies on and it only holds while nothing
    #     on the handoff path touches PB12.  Two independent claims:
    #       1. the three LED entry points are reached ONLY from update mode;
    #       2. the two GPIO registers that can light or darken PB12 are named
    #          ONLY by led.c's own functions — so even an inlined or mis-routed
    #          call could not drive the pin from elsewhere.
    #     Claim 1 is stated as ONLY AND EXACTLY {bl_update_mode}, not as
    #     "nobody outside update mode".  The weaker form was what stood here,
    #     and it passed for a function with NO caller at all: an empty caller
    #     set has nothing outside update mode in it.  So a build in which the
    #     LED had quietly stopped being driven — the stage-2 observable gone,
    #     which is the exact thing this check is for — read as PASS with
    #     "called from []" printed underneath, and the printed evidence was the
    #     only place the truth appeared.
    led_fns = ["bl_led_init", "bl_led_set_pattern", "bl_led_poll"]
    if "bl_update_mode" not in syms:
        c.warn("bl_update_mode was inlined; the LED call-site check degrades",
               "cannot distinguish update mode from the handoff path inside "
               "bl_main — re-read `make disasm` by hand")
    else:
        led_missing = [f for f in led_fns if f not in syms]
        led_bad = {f: sorted(callers_of(f)) or "NOTHING" for f in led_fns
                   if f in syms and callers_of(f) != {"bl_update_mode"}}
        c.check(not led_missing and not led_bad,
                "the LED is driven from update mode and from nowhere else — "
                "the handoff path never touches PB12 (stage-2 observable "
                "intact)",
                "\n".join(f"{f} called from {sorted(callers_of(f))}"
                          for f in led_fns)
                if not led_missing and not led_bad else
                (f"absent from the image: {led_missing}\n" if led_missing else "")
                + (str(led_bad) if led_bad else ""))

    pb_syms = {s for (_a, s, v) in literals if v in (R32_PB_OUT, R32_PB_CLR)}
    led_owned = {"bl_led_init", "led_apply"}
    c.check(pb_syms and pb_syms <= led_owned,
            f"PB_OUT (0x{R32_PB_OUT:08X}) and PB_CLR (0x{R32_PB_CLR:08X}) are "
            "named only by led.c",
            f"referenced from {sorted(pb_syms)}"
            + ("" if pb_syms <= led_owned else
               f"\nunexpected: {sorted(pb_syms - led_owned)}"))

    # (f) STAGE 6 handover.  A SYSRESETREQ needs BOTH the AIRCR address and the
    #     0x05FA write key; either one appearing outside the handover would mean
    #     something else in the image can reset the part.
    #
    #     boot.c offers two handover mechanisms and BL_BOOT_VIA_RESET selects
    #     between them: SYSRESETREQ (the default) or a direct bl_jump_to_app().
    #     Demanding AIRCR be PRESENT would false-fail the documented
    #     `-DBL_BOOT_VIA_RESET=0` build, where its absence is the whole point.
    #     The claim asserted is "exactly one handover mechanism exists, and the
    #     reset registers, IF present, are confined to the handover"; the
    #     detected mechanism is printed either way, because silently accepting
    #     either would let a build lose its handover entirely.
    aircr_syms = {s for (_a, s, v) in literals if v == SCB_AIRCR}
    key_syms = {s for (_a, s, v) in literals if v == AIRCR_SYSRESETREQ}
    via_reset = bool(aircr_syms) or bool(key_syms)
    # The direct-branch build inlines bl_update_handover into bl_update_mode,
    # so accept the jump being reached from either.
    jump_from = callers_of("bl_jump_to_app")
    via_branch = bool(jump_from & {"bl_update_handover", "bl_update_mode"})
    if via_reset:
        ok = (aircr_syms == {"bl_update_handover"}
              and key_syms == {"bl_update_handover"})
        c.check(ok,
                "SCB_AIRCR and the 0x05FA0004 reset key appear only in "
                "bl_update_handover",
                f"handover mechanism: SYSRESETREQ (BL_BOOT_VIA_RESET=1)\n"
                f"AIRCR referenced from {sorted(aircr_syms) or 'NOTHING'}, "
                f"key from {sorted(key_syms) or 'NOTHING'}")
    else:
        c.check(via_branch,
                "the handover is a direct branch and no SYSRESETREQ mechanism "
                "exists anywhere in the image",
                f"handover mechanism: direct bl_jump_to_app() "
                f"(BL_BOOT_VIA_RESET=0)\n"
                f"neither SCB_AIRCR (0x{SCB_AIRCR:08X}) nor the 0x05FA0004 key "
                f"is named by any literal pool\n"
                f"bl_jump_to_app called from {sorted(jump_from) or 'NOTHING'}"
                if via_branch else
                "NEITHER mechanism found: no AIRCR/key literal, and "
                f"bl_jump_to_app is called from {sorted(jump_from) or 'NOTHING'}"
                " — update mode cannot hand over at all")
    #     Reached ONLY from update mode, and — when the symbol survives the
    #     link — reached at all.  An empty caller set must NOT pass: it cannot
    #     be told from an inlined function by call sites alone.  The symbol
    #     table can tell them apart — an inlined bl_update_handover is not in
    #     `syms` either — so that is what the two branches below split on.
    ho_from = callers_of("bl_update_handover")
    if "bl_update_handover" not in syms:
        c.check(True, "bl_update_handover was inlined into its caller",
                "no symbol to check call sites against; the handover mechanism "
                "itself is covered by the check above")
    else:
        c.check(ho_from == {"bl_update_mode"},
                "bl_update_handover is reached from update mode and nowhere else",
                f"called from {sorted(ho_from) or 'NOTHING — update mode cannot hand over'}")

    # (g) Honesty about KEEP_UNUSED.  Now that stages 4-6 are wired, most of
    #     KEEP_SYMS has a real caller and the knob is only still paying for the
    #     remainder.  This is INFORMATIONAL: dead public API is not a defect,
    #     but the size headline should say what it is buying.
    keep_syms = ["bl_proto_reset", "bl_proto_bind", "bl_proto_feed",
                 "bl_proto_feed_buf", "bl_frame_build", "bl_proto_finalized",
                 "bl_crc16", "bl_crc16_update",
                 "bl_flash_erase_sector", "bl_flash_program",
                 "bl_flash_program_word", "bl_flash_lock", "bl_flash_is_erased",
                 "bl_usb_tx", "bl_usb_configured"]
    # A function reached only through the ops table has no textual call site.
    via_table = {"bl_flash_erase_sector", "bl_flash_program"}
    orphans = [s for s in keep_syms
               if s in syms and not callers_of(s) and s not in via_table]
    print("  [INFO] KEEP_UNUSED=%d; public API still with no in-image caller: %s"
          % (args.keep_unused, ", ".join(orphans) if orphans else "none"))
    print("         (rebuild with KEEP_UNUSED=0 to drop them; the two flash ops "
          "are reached\n          through bl_update_flash_ops and so have no "
          "textual call site)")

    # ----------------------------------------------------------------- 10
    section("10. Time base — SysTick is used, its INTERRUPT is not")

    # The timebase module gave the update loop a real millisecond clock by
    # running SysTick as a free-running counter.  That is only safe while the
    # SysTick EXCEPTION stays disabled: exception 15 goes through the same
    # bootloader vector table as everything else, and entries 3..35 trampoline
    # into the application's table at 0x4000 — erased mid-update.  An enabled
    # SysTick interrupt is therefore the NVIC_ISER failure reached by a
    # different door, and it needs its own assertion.

    # (0) FIRST: this section's own constants must be the header's constants.
    #     Everything below is an address comparison, so a stale SYST_CSR here
    #     would not fail — it would silently find nothing and PASS, which is the
    #     worst possible outcome for a safety check.  Read the addresses and the
    #     TICKINT bit back out of include/timebase.h and refuse to proceed on a
    #     mismatch, so the two copies cannot drift apart the way
    #     BL_LOOP_POLLS_PER_MS and BL_LED_POLLS_PER_MS did.
    tb = header_defines(os.path.join(INCLUDE, "timebase.h"))
    want = {"BL_TIME_SYST_CSR": SYST_CSR,
            "BL_TIME_SYST_RVR": SYST_RVR,
            "BL_TIME_SYST_CVR": SYST_CVR,
            "BL_TIME_CSR_ENABLE": CSR_ENABLE,
            "BL_TIME_CSR_TICKINT": CSR_TICKINT,
            "BL_TIME_CSR_CLKSOURCE": CSR_CLKSOURCE}
    missing = [k for k in want if k not in tb]
    drift = [(k, tb[k], v) for k, v in want.items() if k in tb and tb[k] != v]
    c.check(not missing and not drift,
            "this section's SysTick constants match include/timebase.h",
            "\n".join(f"{k} = 0x{tb[k]:08X}" for k in sorted(want))
            if not missing and not drift else
            (f"missing from the header: {sorted(missing)}\n" if missing else "")
            + "\n".join(f"{k}: header 0x{h:08X} != checker 0x{w:08X}"
                        for k, h, w in drift))
    if drift:
        # A wrong address makes every check below vacuous; say so rather than
        # printing a row of PASSes derived from it.
        print("         ^ the checks below are NOT meaningful until this agrees")

    csr_syms = {s for (_a, s, v) in literals if v == SYST_CSR}
    rvr_syms = {s for (_a, s, v) in literals if v == SYST_RVR}
    cvr_syms = {s for (_a, s, v) in literals if v == SYST_CVR}

    # (a) SysTick really is the time base — a positive assertion, so that
    #     silently reverting to a poll-count estimate shows up as a failure
    #     rather than as an unchanged PASS.
    c.check(bool(csr_syms) and bool(rvr_syms) and bool(cvr_syms),
            "SysTick is in use as the time base (CSR, RVR and CVR all referenced)",
            f"CSR 0x{SYST_CSR:08X}: {sorted(csr_syms) or 'NOBODY'}\n"
            f"RVR 0x{SYST_RVR:08X}: {sorted(rvr_syms) or 'NOBODY'}\n"
            f"CVR 0x{SYST_CVR:08X}: {sorted(cvr_syms) or 'NOBODY'}")

    # (b) THE assertion.  Every function that can reach SYST_CSR is examined
    #     for CSR-shaped immediates — any value 0..7, the whole space of a
    #     3-bit control register — and none may have TICKINT in it.  7
    #     (ENABLE|TICKINT|CLKSOURCE) is what the shipping application's
    #     systick_init writes; 5 (ENABLE|CLKSOURCE) is what this image writes.
    #
    #     Scope and limits, stated rather than implied: this looks at MOV/ADD/
    #     SUB/CMP immediates and at literal-pool words inside those functions
    #     only.  It cannot follow a value synthesised by shifting or by
    #     arithmetic on a register, so it is a tripwire on the natural way to
    #     write the bug, not a proof.  The machine-checked proof is in
    #     src/timebase.c, whose _Static_assert refuses to compile if
    #     BL_TIME_CSR_RUN & BL_TIME_CSR_TICKINT is ever non-zero; this check is
    #     the independent second opinion taken from the linked image.
    imm_re = re.compile(r"#(\d+)")
    csr_imms = []
    for (a, s, m, ops, _r) in entries:
        if s not in csr_syms:
            continue
        if m == ".word":
            v = int(ops, 16)
            if v < 8:
                csr_imms.append((a, s, m, v))
            continue
        if m.split(".")[0] in ("movs", "mov", "adds", "add", "subs", "sub", "cmp"):
            mi = imm_re.search(ops)
            if mi and int(mi.group(1)) < 8:
                csr_imms.append((a, s, m, int(mi.group(1))))
    tickint = [(a, s, m, v) for (a, s, m, v) in csr_imms if v in CSR_FORBIDDEN]
    if csr_imms:
        print("  CSR-shaped immediates (0..7) in functions that touch SYST_CSR:")
        for a, s, m, v in csr_imms:
            note = ("*** TICKINT SET ***" if v in CSR_FORBIDDEN
                    else "ok" if v in CSR_ALLOWED else "no TICKINT")
            print(f"    {v:#05x}  at 0x{a:04X} in {s:<20s} {m:<6s} {note}")
    c.check(not tickint,
            f"TICKINT (SYST_CSR bit {CSR_TICKINT.bit_length() - 1}) is never set — "
            "SysTick raises no exception",
            f"{len(csr_imms)} CSR-shaped immediates examined, none of "
            f"{sorted(CSR_FORBIDDEN)}" if not tickint
            else "\n".join(f"{v:#x} at 0x{a:04X} in {s} ({m})"
                           for a, s, m, v in tickint))

    # (c) SysTick is exception 15, so it needs no NVIC line at all.  Restated
    #     here as a distinct claim from section 8's: that one is about IRQ6/USB,
    #     this one is about the module that just started using a timer.
    c.check(not [1 for (_a, _s, v) in literals if v == NVIC_ISER],
            f"NVIC_ISER (0x{NVIC_ISER:08X}) is still absent — the time base "
            "enabled no interrupt line",
            "absent (SysTick is exception 15, not an NVIC line)")

    # (d) HANDOFF.  §5.1.2 requires the application to receive SysTick in its
    #     reset state.  bl_jump_to_app() is what guarantees that: inside its
    #     interrupts-masked quiesce block it writes SYST_CSR = 0 (disable),
    #     SYST_RVR = 0 and SYST_CVR = 0 (which also clears COUNTFLAG).
    #     bl_time_deinit() is NOT on this path — it is gc-sectioned out — so
    #     these three stores are the ONLY guarantee and deleting one would
    #     silently hand a live piece of SysTick state to the application.
    #
    #     ALL THREE REGISTERS, and RVR is not decorative padding on the list.
    #     This check named only CSR and CVR for two stages, during which the
    #     handoff really did leave RVR at 0x00FFFFFF while the surrounding
    #     comments claimed SysTick was handed over "in its reset state" — one
    #     of three registers was not.  It was harmless for the shipped image
    #     (fw.bin's systick_init at 0xAB3C writes RVR before CVR and CSR, and
    #     the only other 0xE000E000 reference in its 30,496 bytes is the
    #     handler at 0x46B8, which touches CSR alone) but the claim was false,
    #     and a future application that read RVR would have found a value this
    #     bootloader chose.  Zero is written rather than a nominal "reset
    #     value" because ARMv6-M B3.3.3 leaves SYST_RVR's reset value UNKNOWN.
    #
    #     Scope, stated rather than implied: this proves each register is NAMED
    #     by bl_jump_to_app's literal pool, and the next check proves the
    #     function materialises a zero.  It does not prove the pairing of each
    #     address with that zero — that is what reading `make disasm` is for,
    #     and the emitted order (CSR, RVR, CVR, then ICER/ICPR/ICSR) is
    #     recorded in boot.c's NOTE 2.
    handoff_regs = [("SYST_CSR", SYST_CSR, csr_syms),
                    ("SYST_RVR", SYST_RVR, rvr_syms),
                    ("SYST_CVR", SYST_CVR, cvr_syms)]
    missing_handoff = [n for (n, _v, s) in handoff_regs
                       if "bl_jump_to_app" not in s]
    c.check(not missing_handoff,
            "bl_jump_to_app still clears SYST_CSR, SYST_RVR and SYST_CVR — the "
            "application gets the WHOLE of SysTick in its reset state",
            ", ".join(f"{n} 0x{v:08X}: "
                      f"{'named' if 'bl_jump_to_app' in s else 'NOT NAMED'}"
                      for (n, v, s) in handoff_regs)
            + ("" if not missing_handoff
               else f"\nnot cleared at handoff: {missing_handoff}"))
    jump_zero = [(a, ops) for (a, s, m, ops, _r) in entries
                 if s == "bl_jump_to_app" and m.split(".")[0] == "movs"
                 and re.search(r"#0\b", ops)]
    c.check(bool(jump_zero),
            "bl_jump_to_app materialises the 0 it stores to them",
            f"{len(jump_zero)} `movs rN, #0` at "
            f"{[hex(a) for a, _ in jump_zero]}")

    # ----------------------------------------------------------------- done
    print()
    print("=" * 74)
    if c.failures:
        print(f"RESULT: FAIL — {len(c.failures)} of {c.n} checks failed")
        for f in c.failures:
            print(f"  - {f}")
        print("=" * 74)
        return 1
    print(f"RESULT: PASS — {c.n} checks, 0 failures, {len(c.warnings)} warnings")
    print("=" * 74)
    return 0


if __name__ == "__main__":
    sys.exit(main())
