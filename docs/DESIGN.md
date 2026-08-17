# Design

Why this bootloader is built the way it is. Most of what follows is about the CH579
specifically and about ARMv6-M generally, and is written to be useful to someone who
arrived here looking for one of those rather than for GBFlash.

**This is the deep end of the documentation set, and it is the authority for everything
in it.** `TECHNICAL_DETAILS.md` in the repository root is the shorter bridge — flash
layout, the verification record, the shape of the design — and defers here for the
reasoning. Where the two overlap, this page is correct. Where a number is quoted in both,
this page is where it was derived.

---

## The problem

A bootloader that lives at flash `0x0000` on a Cortex-M0 has to solve one problem that
does not exist on Cortex-M3 and above, and one that is specific to this part:

1. **ARMv6-M has no VTOR.** The vector table address cannot be moved. The bootloader
   owns the table at `0x0000` forever, including for the entire lifetime of the
   application it hands off to.
2. **The bootloader must erase and program the flash it is running near**, over USB,
   from a device with 32 KB of SRAM and no second bank.

Everything below follows from those two.

---

## 1. The vector table, and the IPSR trampoline

### There is no way to move the table

On ARMv6-M, `VTOR` (`0xE000ED08`) is not implemented. On this part it is not implemented
either — SRAM is not aliased at address 0, and the CH579's `RB_ROM_CODE_OFS` remapping
bit is disqualified twice over: wrong granularity, and it is not cleared by
`SYSRESETREQ`, so a bootloader that used it could never regain control on the next
reset. The hardware will always fetch exceptions from a table at `0x00000000`.

So the bootloader's table is the only table. The application's own table, at `0x4000`,
is never installed and is never fetched from by hardware.

### The escape

A vector table is a table of pointers, and the application's entries are absolute,
self-contained, Thumb-bit-set code addresses. ARMv6-M provides `MRS Rd, IPSR`, and the
IPSR exception number is *numerically identical to the vector table index*:

| IPSR | vector index | exception |
|---:|---:|---|
| 2 | 2 | NMI |
| 3 | 3 | HardFault |
| 11 | 11 | SVCall |
| 14 | 14 | PendSV |
| 15 | 15 | SysTick |
| 16+n | 16+n | IRQn |

So a single shared handler can read its own exception number, index the application's
table at `0x4000` with it, and branch there. Confirmed against the vendor datasheet's
vector table rather than ARM lore alone: `SVCall 0x2C`, `PendSV 0x38`, `SysTick 0x3C`,
`USB 0x58`, `UART1 0x6C`, `WDOG_BAT 0x8C` are all exactly `4 * exception_number`. This
part has 20 IRQs, so the table is 36 entries, `0x90` bytes.

```asm
bl_vector_forward:
    mrs     r0, ipsr            @ exception number == vector table index
    lsls    r0, r0, #2          @ -> byte offset into the application's table
    ldr     r1, .Lapp_vectors   @ r1 = 0x00004000
    ldr     r0, [r1, r0]        @ r0 = the application's handler address
    lsrs    r1, r0, #14         @ == 0 iff r0 <  0x4000   (catches 0x00000000)
    beq     .Lhang
    lsrs    r1, r0, #18         @ != 0 iff r0 >= 0x40000  (catches 0xFFFFFFFF)
    bne     .Lhang
    lsls    r1, r0, #31         @ == 0 iff Thumb bit clear (catches even targets)
    beq     .Lhang
    bx      r0                  @ enter the real handler, LR = EXC_RETURN
.Lhang:
    b       .Lhang
```

44 bytes total, once, for all 34 forwarded exceptions: 36 for the dispatcher above
(26 bytes of code, 2 of pad and its two 4-byte literals) plus the 8-byte NMI prologue
described below. About 18 cycles, roughly 0.56 µs at 32 MHz, on top of the ~16 cycles
ARMv6-M already spends entering an exception.

**Why it is transparent.** On ARMv6-M exception entry the hardware auto-stacks
`r0`-`r3`, `r12`, `LR`, `PC` and `xPSR` before the handler's first instruction runs. So
`r0`-`r3` and `r12` are already saved and are scratch by AAPCS, and exception handlers
take no arguments. This code clobbers only `r0` and `r1`. `r4`-`r11` are *not* stacked
and must not be touched. `LR` still holds `EXC_RETURN` at the `bx`, so the real handler's
ordinary epilogue (`pop {pc}` or `bx lr`) performs a correct exception return. The
application cannot tell the difference.

### Both guards are mandatory

**The range check.** An earlier design guarded only against zero. That is wrong. Erased
CH579 CodeFlash reads `0xFF`, so in update mode with the application region erased the
fetched entry is `0xFFFFFFFF`, and a zero test does not fire. The failure is worse than
executing erased flash: `bx r0` runs in Handler mode, where ARMv6-M interprets a PC value
of `0xFFFFFFFx` as `EXC_RETURN` rather than as a branch. `0xFFFFFFFF` is a reserved
`EXC_RETURN` — only `F1`, `F9` and `FD` are valid — so it faults, and a fault taken from
a fault handler on M0 escalates to **LOCKUP**, not to the intended defined spin.

**The Thumb-bit check.** Range alone is not enough. `bx` to an address with bit 0 clear
is an attempt to enter ARM state, which ARMv6-M does not have: it raises a HardFault at
the branch. Taken from inside a fault handler that is exactly the LOCKUP the range check
exists to avoid. An in-range even word is not hypothetical — it is what a table
half-programmed by an interrupted update looks like, and CH579 CodeFlash programming
only clears bits, so a partially written entry can be any bit-subset of the intended
value.

On a rejected entry the trampoline spins. That is deliberate: a spin is the most
debuggable defined state available. The device simply stops, a debugger finds the PC
inside the bootloader, and a power cycle plus the H1 ISP path recovers it — see
[RECOVERY.md](RECOVERY.md) for what that path assumes about your access to the board.
Resetting could loop forever, and returning is not possible because there is no valid
handler to return from.

**Alignment is load-bearing.** ARMv6-M computes a PC-relative literal address as
`Align(PC, 4) + imm`. If this function began at a 2-mod-4 address the pool read would
miss. Hence `.align 2` immediately before the label, and the literal pool is placed by
the assembler — never hand-code the offset.

### NMI is the one special case

Vector slot 2 does not go straight to the trampoline. It enters a four-instruction
prologue that locks CodeFlash and then **falls through** into the identical trampoline
code.

The only NMI source on this part is the brown-out detector. The shipping application
arms it. And the flash driver's `cpsid i` does **not** mask NMI — so an NMI can land
inside the driver's unlock window and either reset the chip mid-update, or reach the
trampoline's range guard with the application region erased and spin there with
CodeFlash still unlocked. Locking first makes the second case safe.

Slot 2 must still *forward* in the general case, because the bootloader's table stays
installed for the application's entire life and the application re-arms its own brown-out
NMI. Hence fall-through rather than a local-only handler. (An ARMv6-M unconditional `b`
reaches only ±2 KB and these two symbols are not guaranteed to stay that close in a
15.8 KB image; keeping them adjacent in one section deletes the range question.)

This is defence in depth. The reset code disarms the brown-out NMI outright before
anything can reach the flash driver, so the prologue should never execute at all.

---

## 2. USB is polled, deliberately

`NVIC_ISER` is never written. Not once, anywhere in the image, and `make check` asserts
that its address appears in no literal pool and in no word-aligned scan of the binary.
`NVIC_ICER` and `NVIC_ICPR` bit 6 *are* written, so IRQ6 is positively masked rather
than merely left un-enabled.

The reason is section 1. Vector 22 is IRQ6, the USB interrupt, and like every other
forwarded vector it trampolines into the application's table at `0x4000` — **which is
erased during an update.** An enabled IRQ6 firing mid-update would fetch `0xFFFFFFFF`,
be rejected by the range guard, and spin forever *with CodeFlash unlocked*, because the
interrupt would have landed inside the flash driver's unlock window.

There is no version of "install our own USB handler in the table" that fixes this, since
there is only one table and it must keep forwarding for the application's sake. Polling
is not a simplification here; it is the only design that is correct.

The consequences are contained:

- **No path may run longer than about 10 ms without calling the USB poll**, because a
  USB bus reset must be answered inside the host's reset-recovery window. The flag
  latches, so late service is degraded rather than fatal — the host simply retries, and
  the SIE auto-NAKs meanwhile.
- A single 512-byte sector erase-plus-program is well inside that budget.
- The time base is SysTick run as a **free-running counter with its interrupt
  disabled**, sampled by polling. No vector is taken, `TICKINT` is never set, and
  `make check` asserts both. The application still receives SysTick in its reset state
  because the handoff clears `SYST_CSR`, `SYST_RVR` and `SYST_CVR` on the way out.

---

## 3. The CH579 flash controller

This section is the one most likely to be useful outside this project.

CodeFlash is 250 KB at `0x00000000..0x0003E7FF`; DataFlash begins at `0x3E800`. It is
memory-mapped and readable by ordinary loads. The controller is four registers:

| address | width | register |
|---|---|---|
| `0x40001800` | 32 | `R32_FLASH_DATA` |
| `0x40001804` | 32 | `R32_FLASH_ADDR` |
| `0x40001808` | 8 | `R8_FLASH_COMMAND` |
| `0x40001809` | 8 | `R8_FLASH_PROTECT` |
| `0x4000180A` | 16 | `R16_FLASH_STATUS` |

Commands: `0x9A` programs one 32-bit word, `0xA6` erases one 512-byte sector.

### The three things that catch people out

**1. `R8_FLASH_PROTECT` does not read back what you wrote.** You unlock CodeFlash by
writing `0x88` — `RB_ROM_WE_MUST_10` (`0x80`) plus `RB_ROM_CODE_WE` (`0x08`). Read it
back and you get `0x08`, because `RB_ROM_WE_MUST_10` is **write-only**. Treating that as
a failure is the classic bug on this part. Do not verify this register; just write it.

**2. There is no busy bit, and you must not poll for one.** The MCU is *paused by
hardware* for the duration of an erase or a program. Execution does not continue while
the operation runs. So the status read on the very next instruction is already the final
result — there is nothing to wait for, and any wait loop you write is dead code:

```c
R8_FLASH_PROTECT = 0x88;        /* unlock                            */
R32_FLASH_ADDR   = addr;
R32_FLASH_DATA   = word;        /* program only                      */
R8_FLASH_COMMAND = 0x9A;        /* core is paused here               */
status = R16_FLASH_STATUS & 0xFF;   /* already the final result      */
R8_FLASH_PROTECT = 0x80;        /* re-lock immediately               */
```

**3. Mask the status to 8 bits.** Bit 9 of `R16_FLASH_STATUS` reads 1 permanently. The
success signature is exactly `0x40` (`RB_ROM_ADDR_OK`) in the low byte; `0x01` is
timeout, `0x02` is error. An unmasked compare against `0x40` can never succeed.

### And two more that matter for correctness

**Programming only clears bits.** A program is an AND into the existing contents:
`1 -> 0` is possible, `0 -> 1` is not. Read-modify-write is therefore not available at
word granularity, and an unaligned or odd-length tail cannot be patched in — it has to
be rejected or padded with `0xFF` before the write, never merged. Programming `0xFF`
into erased flash is a no-op, which is what makes `0xFF` the correct pad.

**Erased flash reads `0xFF`, not `0x00`.** Proven on this silicon. This matters
everywhere a "blank" test is written, and it is why the trampoline's range check has to
reject `0xFFFFFFFF` explicitly.

### How this driver uses it

- **Two values, ever, to `R8_FLASH_PROTECT`:** `0x80` (locked) and `0x88` (CodeFlash
  write-enabled). `0x8C` would additionally set `RB_ROM_DATA_WE`, which is the
  precondition for touching InfoFlash — where the user configuration word and
  `CFG_BOOT_EN` live. As long as at most one write-enable bit is ever set, no bug in
  this driver can disarm the H1 ISP recovery path. (WCH's own SDK writes `0x8C` on every
  call. It is not the reference used here.) `make check` asserts `0x8C` is never
  materialised in any function that can reach the register.
- **Interrupts are masked across the unlock window.** The bootloader is fully polled so
  nothing should fire, but masking costs two instructions and makes "flash is unlocked" a
  window no other code can observe or extend. No wait loop is needed inside it — see
  point 2 above — so masking cannot lengthen the blackout.
- **The write floor is `0x3E00`, and it is a sector boundary.** `0x3E00 / 512 = 31`
  exactly and `0x4000 / 512 = 32` exactly. Because the floor is itself a boundary, no
  erase this driver will accept can reach a single byte of bootloader code, and the
  boot-info record can be rewritten without disturbing one instruction. This is
  structural, not a matter of checking carefully.
- **Every entry point range-checks first**, with the upper bound written as an explicit
  `addr >= END` test *before* any subtraction. The tempting single-test form
  (`len > END - addr`) wraps for an address past the end and lets every length through.
- **The source buffer for a program must be in SRAM.** Checked, not assumed.

---

## 4. The boot-info record

One 512-byte sector, sector 31, at `0x3E00`, alone. 14 bytes used, the rest erased.
Little-endian, unlike the wire protocol, which is big-endian everywhere — the endianness
flip at that boundary is a real trap.

| offset | size | field |
|---|---|---|
| `0x00` | u16 | marker. `0xFFFF` in every distributed image. **Not validated.** |
| `0x02` | 4 | tag, ASCII `LFBG` |
| `0x06` | u16 | CRC16 of `length` bytes at `0x4000` |
| `0x08` | u32 | application length |
| `0x0C` | u16 | CRC16 of record bytes `0x00..0x0B` |

`fw.bin` byte 0 lands at `0x3E00`, so this record is simply the first 14 bytes of the
vendor firmware file, and `fw.bin` byte `0x200` lands at `0x4000` as the application's
vector table.

### The validation gates

Two independent pieces of code look at these same 14 bytes:

- **the validator** — decides whether the device boots what is already in flash;
- **the writer** — decides whether an update's finalize command may answer `SUCCESS`.

**They must apply the same gates.** Every divergence found during this project's
development was a real, reachable defect, and all of them had the same shape:

- the writer was 8× *stricter* on maximum length, so a legitimate image was NAKed
  forever — *after* the first data packet had already erased the boot-info sector,
  leaving the device in update mode with no valid application, unable to accept the very
  firmware being installed;
- the writer was *looser* on minimum length, so the device answered `SUCCESS` for a
  4-byte "application" with self-consistent CRCs that the validator then refused
  forever — a device sitting in update mode reporting a successful update;
- the writer whitelisted the marker and the validator did not (below).

Both directions are bugs. The two gate sets are therefore declared as bitmasks in the
two headers, spelled identically, and a static assertion in whichever header is included
second fails the build if they differ. A native check additionally compares them at run
time, because two macros that both expand at the point of use would compare equal even
if their *values* had diverged.

The gates, in the order the validator applies them:

1. tag is `LFBG`;
2. record CRC16 over bytes `0x00..0x0B` matches the stored value at `0x0C`;
3. length is within `[0x90, 0x3A800]` and `0x4000 + length` stays inside CodeFlash —
   checked **before** the payload CRC, so a corrupt length can never make the CRC loop
   walk off the end of the address map;
4. the application's initial SP (`*(u32 *)0x4000`) looks like SRAM:
   `(sp & 0x2FFE0000) == 0x20000000`;
5. the application's reset vector (`*(u32 *)0x4004`) has the Thumb bit set and points
   inside the application image;
6. payload CRC16 over `length` bytes at `0x4000` matches the stored value at `0x06`.
   About 24.8 ms at 32 MHz for the stock 30,496-byte image.

Gate 5 is not optional, and its absence was a device-bricking defect found in review.
The handoff loads that word and `BX`es to it directly; it is the one vector dispatched by
hand rather than through the range-checked trampoline. Without the gate, an application
accidentally linked for base `0x0000` instead of `0x4000` — **exactly the layout a
bootloader-less GBFlash ships with** — passes the SP mask and both CRCs while carrying a
reset vector like `0x00A5`, and the bootloader then branches into its own code at an
arbitrary offset. That can land past the argument guards of the flash driver, which
would unlock CodeFlash and issue an erase or program with whatever happens to be in the
registers, writing below `0x3E00` and destroying the bootloader.

### Why the marker must not be stamped

It is tempting to have the bootloader write a "valid" marker into `0x00` after a
successful install — the field is right there, and images carry `0xFFFF` in it.

Two reasons not to, and the second is fatal.

**The record CRC at `0x0C` covers bytes `0x00..0x0B`, including the marker.** Rewriting
`0xFFFF` to anything else invalidates the very record the bootloader must later
validate. The device would refuse to boot the image it just successfully installed.

**And it is not physically possible anyway.** Programming only clears bits. `0xFFFF` can
become `0x5555`, but going the other way, or to most other values, requires setting bits,
which requires an erase — of the sector that holds the record you are trying to stamp.

So the marker is written **exactly as the host sent it**, and nothing is ever stamped
over it. It is authenticated as a side effect of the record CRC, which is also why the
validator does not whitelist it: a whitelist of two literal values can only reject an
otherwise-perfect future image, and rejecting one is a device stuck in update mode while
accepting one costs nothing, since the CRC still authenticates the bytes.

### The commit is one word

The record is programmed **last**, after the whole image has been received, CRC-checked
and read back from flash, and in a fixed word order:

```
words 0x10..0x1FF   the erased tail            record still invalid
word  0x00          marker + "LF"              record still invalid
word  0x04          "BG" + payload CRC         record still invalid
word  0x08          length                     record still invalid
word  0x0C          header CRC                 <-- VALID from this instant
```

Everything except the commit word is read back and compared *while the record is still
invalid*, so a read-back problem in the 508 bytes that carry the actual content leaves
the device safely in update mode rather than reporting failure over a record that is
already live. The commit itself is a single word write — ~27 µs typical, ~36 µs worst
case (see the note on flash timing below; both figures appear in this document and they
are the two ends of the same number, not a contradiction).

### Ordering makes an interrupted update safe

The first data packet erases the boot-info sector **first**, before anything else is
written. From that instant there is no valid boot-info record, so any interruption —
power loss, cable pull, host crash — lands back in update mode instead of booting a
half-written application. The boot-info page itself is buffered in RAM for the whole
session and only programmed at finalize.

Application sectors are erased and programmed one at a time as their packet arrives.
Never bulk-erased: 59 sectors is 83 ms typical and 142 ms worst case, which blows the
host's roughly 100 ms response window.

---

## 5. Handoff, and the post-update reset

### Booting the application

```
1. read the update magic at SRAM 0x20000090, and clear it unconditionally
2. if it was 0xAA55BB01           -> update mode (reason: host requested)
3. else if U22 is held            -> update mode (reason: button)
4. else if the application validates -> jump to it
5. else                           -> update mode (reason: no valid application)
```

Step 1's clear is unconditional and comes first. SRAM survives `SYSRESETREQ` on this
part — measured — and the application's own startup does not zero this word, so a
bootloader that failed to clear it would leave the device in update mode on every reset
forever. That is not hypothetical: it is the state a half-built bootloader was found in.
The clear is unconditional rather than guarded by the comparison so a partially written
or garbage value cannot persist either. The linker script places a `NOLOAD` section over
the whole reserved hole `0x20000000..0x2000009F` as the first thing in RAM, with a
link-time assertion that `.bss` starts above it, so the `.bss` zeroing loop cannot reach
the magic either.

The handoff quiesces the core to as close to cold-reset state as software can manage —
SysTick's three registers cleared, all IRQs disabled and un-pended, PendSV and SysTick
pending bits cleared — then sets MSP from the application's vector 0, unmasks interrupts,
and `BX`es to its vector 1.

`PRIMASK` is deliberately left **clear** at the branch. The application's startup never
executes `CPSIE I`, so a bootloader that handed off with interrupts masked would leave
the application permanently unable to service one — a device that looks dead after a
perfectly successful boot. Likewise the reset code never executes `cpsid i`: the
"fully polled" rule is satisfied by never *enabling* an NVIC source, not by masking.

The reset vector is `BX`ed to as-is, with no `ORR #1`. Forcing the Thumb bit would mask a
genuinely malformed table instead of faulting visibly.

### Why a reset, not a branch, after an update

When finalize succeeds, the bootloader does **not** branch straight into the new
application. It issues `SYSRESETREQ` and lets itself run again from a real reset, then
re-validates and hands off through exactly the path that is proven on every ordinary
boot.

This is a USB argument, not a CPU one.

A direct branch leaves the USB SIE exactly as update mode left it: enumerated, D+ pull-up
attached, and holding the address the host assigned to the *bootloader's* CH340 device.
The application's own USB initialisation then resets the SIE and clears the device
address back to 0 — but it never drops the pull-up. So the host sees no disconnect, keeps
addressing the device at the old address, and the device answers nothing.

**The result is a board that looks dead after a completely successful update, until it is
physically unplugged.** That is the single worst outcome available here, because the
user's natural next move is to run the updater again.

`SYSRESETREQ` resets the USB block, the pull-up is removed for the ~25 ms the bootloader
spends re-validating the image, and the host sees a clean disconnect followed by the
application enumerating normally. That this works on this silicon is not an inference: it
is the exact mechanism the application already uses to *enter* update mode.

It costs one extra application CRC and nothing else.

### The reboot is delayed, and the delay is not optional

The "finalized" flag goes true *during* the call that parses the finalize command — that
is, **before** the acknowledgement has left the USB endpoint. A loop shaped

```c
feed(...);
if (finalized()) reset();
```

resets while the ack is still queued, and every host then reports failure on a successful
update. The reboot is therefore gated on 250 ms of receive silence measured from the last
byte received, restarted by any further inbound traffic — which also absorbs a finalize
retransmission instead of rebooting into the middle of one. Both known hosts wait far
longer than that before looking for the device again.

---

## 6. Clock, and the absence of a crystal

The reset code writes `R16_CLK_SYS_CFG = 0x0088`: PLL divider 8, system clock mode 2
("directly from 32 MHz"), and critically **`RB_CLK_OSC32M_XT` clear — the internal RC
oscillator**. The crystal selection bit is not touched.

That is a deliberate constraint, not an oversight. Some GBFlash boards ship with no X1
crystal fitted, and a crystal-dependent bootloader would fail on exactly the devices that
need one. The internal RC is proven on this silicon to drive USB at full speed and the
CH340 emulation at 2 Mbaud — the application itself runs USB on the internal RC.

`R16_CLK_SYS_CFG` is a safe-access register: it requires a `0x57`/`0xA8` unlock
immediately before the write, and if the window is missed the write is simply **ignored**
and the register keeps its value. The failure mode is "clock stays at the default", not
corruption. And a wrong clock cannot take the ISP entry path away either — the ROM ISP
runs before any of this code and configures its own clock, so H1 still enumerates. That
is the entry point staying available, not a guarantee of recovery; see
[RECOVERY.md](RECOVERY.md).

Only the 16-bit half at `0x40001008` is written, with `strh` and never `str`, because
`R8_HFCK_PWR_CTRL` shares the same word at `0x4000100A` and must not be disturbed. The
same discipline applies to the brown-out disarm at `0x40001024`, where the control and
configuration bytes share a word with a read-only status byte.

---

## 7. Update mode, and what the LED says

Update mode brings up polled USB, the activity LED and the protocol framer, and runs
them in one loop. Each iteration polls USB, advances the LED pattern, drains up to two
bulk packets into the framer, and transmits any response the framer produced.

The ACT LED on PB12 is **active low** and is driven only from update mode. The normal
boot path never touches PB12 at all — `make check` asserts that — so the existing
observable holds: an LED that lights steadily shortly after power-on means the
*application* booted, which is how the device's owner tells a good boot from a bad one.
The pin's output latch is preset dark before the direction register makes it an output,
otherwise entry to update mode produces a visible flash.

A pattern is 16 slots of 100 ms, so a 1.6-second repeat:

| pattern | reason |
|---|---|
| **blink, blink, pause** | the host asked for update mode. This is the firmware family's established idiom, and what FlashGBX's CLI tells the user to look for. |
| **blink, blink, blink, pause** | U22 was held at power-on. |
| **fast, 5 Hz, never pausing** | there is no valid application. An alarm rather than a heartbeat, because it is one. |
| **slow 0.8 s on / 0.8 s off** | update mode with no reason recorded. Should not happen. |

The pattern visibly slows during a firmware transfer, because each sector erase pauses
the core for up to 2.4 ms and a 512-byte program for about 4.6 ms. That reads as "busy"
and is useful. There is deliberately no catch-up: at most one slot advances per call, so
a long stall can never turn into a burst of fast transitions.

> **On the two flash timing figures.** A word program is quoted in this project as both
> ~27 µs and ~36 µs, and both are correct: **~27 µs typical, ~36 µs worst case**. Sector
> erase is likewise **~1.4 ms typical, up to 2.4 ms worst case**. Everything the design
> has to be safe against is derived from the worst-case pair — 128 words at ~36 µs is the
> ~4.6 ms quoted above for a 512-byte page, and 59 sectors of erase-plus-program is where
> the whole-image figures come from. Where a number is used to argue that a deadline is
> met, it is the worst case. Where one describes what a single operation usually costs,
> it is the typical.

---

## 8. Testing without hardware

The protocol layer is written to be **portable** — no MMIO, no target headers, no libc —
so it compiles unchanged for Cortex-M0 and natively on a host. Flash access goes through
an injected function table, so the host substitutes a RAM-backed model.

That is what makes the offline end-to-end rehearsal possible: three consecutive complete
firmware updates, through the real protocol code and the real flash driver against a
register-level model of the flash controller, each one checked byte-for-byte and then run
through the real `bl_app_valid()`. Plus 30,000 fuzz iterations into the framer with guard
pages behind every buffer.

That rehearsal is the one suite that needs firmware images supplied from outside the tree
— this repository ships none — so it is skipped in a plain checkout. Everything else,
including the replays of three real host implementations, runs against a synthetic image
the repository generates. See [BUILDING.md](BUILDING.md).

None of that replaces hardware testing. It does mean that when hardware testing happens,
it is testing the parts only hardware can test.

See [BUILDING.md](BUILDING.md) for how to run it, and [PROTOCOL.md](PROTOCOL.md) for the
wire format.
