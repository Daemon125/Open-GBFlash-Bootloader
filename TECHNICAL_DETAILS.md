# Technical details

Everything a reader who wants to understand or extend this bootloader needs, and a
flasher does not. If you only want working firmware updates, [README.md](README.md) is
the whole story and this file is optional.

This file is a bridge, not a duplicate. The deep material lives in `docs/` and is linked
rather than repeated.

## What lives where

| document | what it answers |
|---|---|
| [README.md](README.md) | do I need this, and how do I install it — `python3 install.py` |
| **this file** | what it is, how it is laid out, what has actually been verified |
| [docs/DESIGN.md](docs/DESIGN.md) | **why it is built this way** — the reasoning, in depth |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | the wire protocol, byte-exact |
| [docs/BUILDING.md](docs/BUILDING.md) | toolchain, targets, what each gate asserts, the host suites |
| [docs/INSTALLING.md](docs/INSTALLING.md) | the same install done **by hand**, with every check and failure mode — what `install.py` is doing, and why |
| [docs/RECOVERY.md](docs/RECOVERY.md) | `install.py --restore`, backups, the manual ROM ISP restore, recovering from specific states |
| [docs/RELEASE-NOTES.md](docs/RELEASE-NOTES.md) | the complete verified/not-verified ledger and known limitations |

When this file and `docs/` appear to disagree, `docs/` is the authority — it is written
against the code, and this is a summary of it.

---

## What it does, and what it does not do

**It does:**

- own the vector table at `0x0000`, validate the application, and hand off to it;
- forward every exception to the application's own vector table at `0x4000`, so the
  application behaves exactly as it does without a bootloader;
- enter update mode on request from the running application, when no valid application is
  present, or on the U22 button held at power-on — request and button proven on hardware;
- speak the same wire protocol the vendor bootloader speaks, so FlashGBX and other
  existing hosts drive it unmodified;
- write only at or above `0x3E00`, structurally — the write floor is a sector boundary,
  so no erase it will accept can reach one byte of its own code;
- verify what it wrote by reading flash back before it declares success;
- reboot into the new firmware itself, with no power cycle.

**It does not:**

- modify, patch, re-sign or inspect the application beyond the validity gates;
- write the CH579 user configuration word, ever — that word is what keeps the H1 ISP
  recovery path working, and nothing in this project touches it;
- write DataFlash or InfoFlash;
- enable a single interrupt. USB is polled, deliberately.

Two properties worth stating for anyone porting this: it needs **no X1 crystal** — USB
runs off the internal 32 MHz RC oscillator, so it works on boards with a crystal fitted
and boards without — and it **installs stock vendor firmware unmodified**.

Size: **7,056 bytes** of the 15,872-byte region, 8,816 free; 3,420 bytes of SRAM.

---

## Flash layout

```
0x0000 .. 0x3DFF   bootloader        15,872 B, sectors 0..30   (this project)
0x3E00 .. 0x3FFF   boot-info record  sector 31, 14 bytes used  (fw.bin byte 0)
0x4000 ..          application       fw.bin byte 0x200 onward
0x3E800            end of CodeFlash (250 KB); DataFlash begins here
```

`0x3E00` is `31 x 512` exactly, so the bootloader region and the boot-info record never
share an erase sector, and the boot-info record can be rewritten without disturbing one
instruction of the bootloader.

The boot-info record and the validation gates that read it are covered in
[docs/DESIGN.md](docs/DESIGN.md) §4.

---

## What goes wrong on a device with no bootloader

The application's `BOOTLOADER_RESET` command writes the magic word `0xAA55BB01` to SRAM
`0x20000090` and issues `SYSRESETREQ`. The chip resets, fetches its vectors from flash
`0x0000` — which on such a device is a verbatim copy of the application's own vector
table — and re-enters the application, which ignores the magic. Nothing ever answers the
host's update handshake, so the update cannot even begin.

That is the whole failure: not a broken update, an absent participant.

## How the presence check works

The running application exposes `GET_VARIABLE` (opcode `0xAD`), which resolves a
caller-supplied index against a fixed base and returns what is there. The index is not
bounds-checked, so it doubles as an arbitrary read of the memory map — including
memory-mapped CodeFlash at `0x00000000`.

[`docs/check-bootloader-region.py`](docs/check-bootloader-region.py) uses exactly that,
and nothing else: it sends `0xA1` (QUERY_FW_INFO) and `0xAD` (GET_VARIABLE) and reads
`0x0000..0x3DFF` back. Reads are harmless — nothing is written, no jumper is involved,
the device is not reset. [`docs/backup-codeflash.py`](docs/backup-codeflash.py) is the
same primitive applied to the whole image, one USB round trip per 32-bit word, which is
why a full backup takes a few minutes. `install.py --check` and `install.py --backup` are
the same two reads through the guided installer, and on the one device this was run
against the installer's backup came out byte-identical to `backup-codeflash.py`'s.

Two fields decide the verdict:

| | no bootloader | bootloader present |
|---|---|---|
| `0x00B8..0x3DFF` | all zeros | carries code |
| reset vector at `0x0004` | `>= 0x4000` — the application's own | `< 0x3E00` — into the bootloader |

---

## Why the design looks like this

Summaries only. [docs/DESIGN.md](docs/DESIGN.md) is the real argument for each.

### There is no VTOR on ARMv6-M, so exceptions are trampolined

Cortex-M0 has no Vector Table Offset Register. The table is at address `0` and cannot be
moved, so the bootloader necessarily owns every exception vector for the lifetime of the
device — including while the application is running. Each vector therefore enters a
shared trampoline which reads `IPSR` to learn which exception it is, indexes the
application's own table at `0x4000`, and branches there. The application sees exactly the
handlers it would have seen if it owned the table.

This is the single largest constraint on the design, and it is why the bootloader cannot
simply get out of the way after handoff. NMI is a special case, and both guards on the
trampoline are mandatory rather than defensive — see [docs/DESIGN.md](docs/DESIGN.md) §1.

### USB is polled, and no interrupt is ever enabled

The bootloader enables no interrupt line at all: not USB, not SysTick's exception, not
one NVIC entry. Since the vector table it owns is the same table the application will
inherit, anything it left enabled would fire into a half-configured world during handoff.
Polling removes the class of bug rather than managing it, and `make check` asserts the
absence structurally — that `TICKINT` is never set and `NVIC_ISER` never appears in the
image. See [docs/DESIGN.md](docs/DESIGN.md) §2 and §8.

### Installing needs the H1 jumper; installing firmware afterwards does not

The bootloader occupies `0x0000..0x3DFF`, and `0x0000` is the vector table the device is
executing from right now. Erase granularity is 512 bytes, so writing sector 0 means
erasing sector 0 — and the instant that erase completes, the vector table is gone, along
with the interrupt-driven USB path that would have delivered the next command. No
host-driven write to sector 0 can ever finish. That is structural, not a matter of care.

So the install goes through the CH579's ROM ISP, which does not depend on CodeFlash at
all. Every subsequent firmware update is a plain USB operation, because the bootloader
lives below `0x3E00` and never rewrites itself. That asymmetry is the entire point of the
project. [docs/INSTALLING.md](docs/INSTALLING.md) has the procedure.

### The install image must be a composite

The ROM ISP writes CodeFlash as a whole — `wchisp flash`, and every equivalent, erases the
array and writes from address `0` — so `bootloader.bin` cannot be flashed alone;
the result would be a bootloader with no firmware under it. `tools/build_composite.py` joins the bootloader to an application
image and checks up to 26 properties of the result, the load-bearing one being that the
composite differs from the device's current image **only below `0x3E00`**.

Read that one carefully. In the normal install the application half is copied verbatim out
of the reader's own backup and the same file is the comparison baseline, so the property
is guaranteed *by construction* — the tool annotates it as such rather than presenting a
tautology as evidence. It becomes a measurement only when the application half is sourced
separately. Taking the application half from the reader's own backup is what makes an
install a bootloader install rather than a firmware change — and it is why the backup step
is mandatory. Check counts are per-invocation: 26 with every input supplied, 25 plus one
`SKIP` for the `--backup` release flow, 22 plus four for a bare `--app`. [docs/BUILDING.md](docs/BUILDING.md) §"The composite install image" covers the
tool, including which of its checks are genuine measurements and which hold by
construction, and why the general-purpose full-image tools must not be used instead.

`install.py` builds the composite itself and gates it on the same properties
`bl_app_valid()` applies on-chip, plus two deliberately stricter ones, before it writes
anything to disk — so the device cannot be handed an image it would refuse to boot. Where
`build_composite.py` can be found it then rebuilds the same image with it as an
independent second opinion, and requires the two to agree byte for byte before the write.

---

## Verification record

### On hardware

**Tested on exactly one device: PCB v1.3, CH579M.** No other board revision has run any
of this — nothing has been on a v1.2 or earlier board.

Four complete firmware updates with **unmodified stock vendor `fw.bin`**, every one
checked by reading flash back and comparing byte-for-byte against the stock file rather
than by trusting the updater's own success message:

| # | transition | driven by | result |
|---|---|---|---|
| 1 | L15 -> L14 | `gbflash_serial_update.py` | flash byte-identical to stock file |
| 2 | L14 -> L15 | `gbflash_serial_update.py` | flash byte-identical to stock file |
| 3 | L15 -> L14 | `gbflash_serial_update.py` | flash byte-identical to stock file |
| 4 | L14 -> L15 | FlashGBX's own GUI Firmware Updater | flash byte-identical to stock file |

Both directions of the size change are exercised, so length and CRC handling are covered
rather than the same bytes being rewritten. No power cycle and no jumper were in the
loop: update mode was entered in software, and after finalize the bootloader validated
the new image and rebooted into it — confirmed by `R8_RESET_STATUS` reading
`RST_FLAG_SW` on each new firmware's first boot.

Also proven on silicon: the boot path and handoff, the IPSR exception trampoline (every
byte of verification traffic to the running application was forwarded through it), and
the CH340 emulation, which enumerates as `1A86:7523`.

**A complete `install.py` install has been run on that device, end to end**, and confirmed
the way the updates were: by reading all of flash back afterwards and comparing it byte for
byte against the image that was written.

**`wchisp` is the ISP tool this project drives, and it is the only one — verified on that
device.** `wchisp info` identified the part, and `wchisp flash` of a full composite erased,
wrote and verified 47,104 bytes, leaving the bootloader intact at `0x0004` and the
application byte-identical to the stock file. H1 worked after every write.

`wchisp flash` does not touch the configuration registers at all — those are a separate
`wchisp config` subcommand, which `install.py`'s ISP argv guard forbids. That guard is what
keeps the user configuration word, and with it the H1 recovery path, out of reach of any
install. [docs/RECOVERY.md](docs/RECOVERY.md#what-recovery-depends-on) has the detail,
including what to watch for if you drive a different ISP tool by hand.

### Not verified on hardware

- **Other board revisions.** Nothing here has run on a v1.2 or earlier board, or on any
  CH579 part other than the CH579M on this one PCB. Everything above is a statement about
  one device.
- **`src/led.c`** is the one shipping module with no host-side test. Its failure mode is
  cosmetic, and `make check` asserts the LED is driven only from update mode.
- `src/led.c` is never compiled natively; its coverage is offline only.

### How strong the offline verification actually is

**The USB and flash models the host suites test against were written from the same
analysis as the code.** If that analysis is wrong about the silicon, the model and the
code are wrong in the same direction and the tests still pass. This is the honest limit
of the offline gates, and it is why the four real updates are the stronger evidence.

The offline rehearsal is a rehearsal: it replays recorded host conversations against a
model, and its fixtures are generated from a **synthetic** image
(`host/make_synthetic_fw.py`) — this repository contains no vendor firmware and must not
acquire any.

An earlier **diagnostic** build (`make BL_USB_ECHO=1`, a different binary that does not
answer the update protocol) round-tripped bulk echo at 1, 8, 31, 32, 33, 64, 256, 512,
1024 and 4096 bytes, 10/10, with 4,096 B in 32.5 ms. `src/usb.c` has been revised since
that measurement, so treat those throughput figures as indicative of the datapath rather
than as a property of the shipping image.

The complete ledger is [docs/RELEASE-NOTES.md](docs/RELEASE-NOTES.md).

---

## Building, and the gates

Toolchain: `arm-none-eabi-gcc` **15.3.Rel1** (Arm GNU Toolchain). Any recent
`arm-none-eabi` GCC with Cortex-M0 support should work, but 15.3.Rel1 is the one every
figure in this repository was produced with.

```sh
make all                   # -> build/bootloader.elf, .bin, .map
make check                 # 68 offline assertions against the produced .bin
make -C host test          # the native suites plus the installer suite, no hardware
make -C host test-install   # the installer suite alone: no compiler, no pyserial, no device
```

Output from a clean rebuild of this tree, quoted:

```
Memory region         Used Size  Region Size  %age Used
           FLASH:        7056 B      15872 B     44.46%
             RAM:        3420 B        32 KB     10.44%
  IMAGE       7056 bytes / 15872 budget (8816 free)

RESULT: PASS — 68 checks, 0 failures, 0 warnings

77 assertions, 158446 executions passed, 0 failed          (timebase)
test_flash: 208 assertions / 241 executions, 0 failures
193 assertions, 305 executions passed, 0 failed, 1 notes   (usb)
216 assertions, 1836 executions passed, 0 failed, 0 notes  (proto)
fuzz: 30000 iterations, 41314040 bytes fed, 524562 response bytes,
      109645 idle notifications, 0 failures
rehearse_update: 80 checks, 0 failures
test_install: 540 checks, 0 failures
```

`test_install` drives `install.py`'s own logic against simulated devices in thirteen states
— including a device that already has a bootloader, a backup that fails verification, a
missing ISP tool, a jumper that never takes, and a write that lands one byte wrong — and
asserts both that the run refuses and *where* it stops. It needs no compiler, no cross
toolchain, no pyserial and no device, so it is the one suite that still runs when the C
build is broken. It also runs the installer's own bootloader gates over an image — the
real `build/bootloader.bin` when the tree has been built, a synthetic one otherwise, so
the check count does not depend on the environment — and asserts that it fits the
`0x3E00` budget. Neither the exact size nor the sha256 is asserted; both are reported.

The build is deterministic for a given toolchain, not across toolchains: with the one
named in [docs/BUILDING.md](docs/BUILDING.md) the image is sha256
`4cf2a387bf62f1d64dfbbb145681fd0e107b0698d812e23686ce31b66d00d4cd`, and a different
`arm-none-eabi-gcc` produces a different, equally valid one. That is why nothing asserts
the value — an earlier version did, and it would have failed the release job on every CI
runner. If an *incremental* build changes the hash with the toolchain unchanged,
`make clean && make all` is the fix: a stale object tree, not a source change.

A release stamps the digest of the binary it publishes into the `install.py` published
beside it (`BL_SHA256`, written by `tools/stage_release.py`), so the pair vouches for
itself whatever compiler CI used. In a checkout the constant is `None` and nothing is
claimed, because a from-source build is not a forgery.

`make check` needs a Python 3 interpreter and nothing else; it defaults to `python3` and
takes a `PYTHON=` override.

Some host checks can use stock vendor firmware images, which are **not redistributed
here**. Without them `make -C host test` still exits 0 and prints exactly what it
skipped. To run the lot, supply your own:

```sh
make -C host test FW_L14=/path/to/L14/fw.bin FW_L15=/path/to/L15/fw.bin
```

What each gate asserts, what degrades without vendor images, and the sanitizer and
guard-page setup are all in [docs/BUILDING.md](docs/BUILDING.md).

---

## Licensing considerations

**MIT.** See [LICENSE](LICENSE), which carries the licence text and the same scope notes.
What the grant does and does not reach, stated plainly:

- **The code is original.** Every line under `src/`, `include/`, `ld/`, `tools/`,
  `host/`, `docs/` and `.github/`, and `install.py`, was written for this project — which
  covers every file a release publishes. It is not derived from WCH's SDK, from the
  GBFlash firmware, or from any existing bootloader.
- **No third-party firmware or host code is redistributed here.** The test fixtures are
  generated from a synthetic image (`host/make_synthetic_fw.py`), and the harness
  degrades gracefully when vendor images are absent — one optional check reports itself
  skipped and the suite still exits 0.
- **The wire protocol was reverse-engineered**, from FlashGBX's open-source host
  implementation and from an independent third-party project's documentation of it, and
  cross-checked against a script published by the original hardware author. A protocol
  is an interface, but the analysis was of someone else's software.
- **The USB descriptors are reproduced from the application.** `src/usb_desc.c` is a
  verbatim transcription of 83 bytes lifted from the GBFlash firmware image — the device
  descriptor, the configuration descriptor set and the CH340 canned vendor-reply table.
  They are reproduced rather than reinvented because the host driver binds on exactly
  these bytes. They are not this project's to license.
- **The device identifiers are someone else's.** The bootloader enumerates as
  `1A86:7523`, which is WCH's CH340 VID/PID, for the same reason: that is what the host
  driver binds to.

None of this is legal advice.

Attribution for the projects this work depends on is in
[README.md](README.md#attribution).
