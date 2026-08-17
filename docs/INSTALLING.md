# Installing the bootloader, by hand

`python3 install.py` does all of this for you and is the recommended way in. This page is
the same procedure driven by hand, with the reasoning, the output to expect and what each
failure looks like.

Where this page and the script disagree about *what to do*, the script is right — it is
executable and it is tested. Where they disagree about *why*, this page is. Either way one
of them is stale and worth reporting.

The steps, and nothing is optional:

| | | |
|---|---|---|
| 1 | check you need this | [below](#step-1--check-that-you-need-this) |
| 2 | prove you can enter ISP mode — **before writing anything** | [below](#step-2--prove-you-can-enter-isp-mode) |
| 3 | take a full backup | [below](#step-3--take-a-full-backup) |
| 4 | build the composite install image | [below](#step-4--build-the-composite-install-image) |
| 5 | short H1, plug in while shorted, write the composite | [below](#step-5--write-the-composite-over-isp) |
| 6 | remove the jumper, power-cycle, verify | [below](#step-6--verify) |

Afterwards, firmware updates go through FlashGBX in the ordinary way and never need the
jumper again.

## How this maps onto `install.py`

| what the script does | this page |
|---|---|
| finds `bootloader.bin`, `build_composite.py` and `wchisp`, and checks the bootloader image | ["What you need"](#what-you-need) — done up front, so a missing tool surfaces before any physical step |
| works out whether the device is in application mode, ISP mode, or absent | no manual equivalent; you know which state you put it in |
| reads `0x0000..0x3DFF` and classifies the region | [step 1](#step-1--check-that-you-need-this) |
| takes a backup and runs 21 restorability checks on it | [step 3](#step-3--take-a-full-backup) |
| builds the install image and validates it | [step 4](#step-4--build-the-composite-install-image) |
| guides the jumper, then writes with one ISP command | [step 2](#step-2--prove-you-can-enter-isp-mode) and [step 5](#step-5--write-the-composite-over-isp) |
| power-cycles, reads all of flash back, compares byte for byte | [step 6](#step-6--verify), plus a full read-back the manual procedure does not do |

Two differences matter if you are choosing between them. The script checks `wchisp` exists
**before** it asks you to jumper anything, so nobody jumpers a board and then discovers the
tool is missing; and it compares the whole of flash against the image it wrote rather than
spot-checking the reset vector. By hand, both are on you.

It does not do step 6(c), the U22 button check, at all — that is a separate route into
update mode, not part of confirming the install.

**Connect one device at a time.** The backup is read over a serial port, but the ISP write
goes to whichever board answers as `4348:55E0` and cannot be aimed at a particular one.
With two GBFlash units attached, one device's firmware could be written over another's.
`install.py` refuses to run at all when more than one candidate is present, and tells you
to unplug the rest or name a port with `--port`. The manual procedure has no such guard —
that is on you.

Before the write, the script states what is about to be erased and makes you type
`INSTALL`; a bare Enter is not a yes. Only then does it ask for the jumper. If `wchisp` is
not installed it offers to download it, and downloads nothing unless you answer yes.

Its backup gate checks both halves. The application from `0x3E00` up is covered by the
boot-info record's own CRCs; the boot region below it is covered by nothing, and it is the
half the part actually starts executing — SP from `0x0000`, reset vector from `0x0004`,
before any code runs. A word garbled on the wire there leaves a file that looks like a
perfect backup and restores to a dark board, so it is gated too:
[RECOVERY.md](RECOVERY.md#what-is-checked-below-0x3e00).

### The modes

| command | what it does |
|---|---|
| `python3 install.py` | the guided install |
| `python3 install.py --check` | read-only: is there a bootloader in `0x0000..0x3DFF`? |
| `python3 install.py --backup FILE` | read-only: take a full backup and prove it is restorable |
| `python3 install.py --restore FILE` | write a backup back over the ROM ISP |
| `python3 install.py --dry-run` | simulate a device and run the real logic against it |
| `python3 install.py --help` | every flag, including the four overrides and what each costs |

`--check` and `--backup` are read-only in the strict sense: two opcodes, both reads, no
jumper, no reset, and DataFlash and the user configuration word untouched.

Every gate always runs. A block where nothing failed prints as a single line with its
tally; anything that fails prints in full, with the value that failed. Add `--show-checks`
to list the passing ones too — useful for a record of an install, not for doing one.

### Rehearsing it with no device attached

```sh
python3 install.py --dry-run
```

Runs the real installer logic against a simulated device, in a temporary directory,
touching nothing physical. It is the fastest way to see what the script asks you, and in
what order, before you have a board in front of you.

`--sim NAME` picks which device to simulate, including the ones that must make it refuse:

```
$ python3 install.py --list-sims
Pretend devices for --dry-run --sim NAME:
  absent               nothing plugged in
  bad-backup           a device whose flash comes back corrupted as it is read
  bad-bootloader       a damaged bootloader.bin
  corrupt-boot-region  a device whose start-up area reads back garbled, while its firmware reads back perfectly
  dead                 a device that will not start up -- the one --restore is for
  empty                a normal device with no bootloader -- the ordinary install
  erased               a device whose bootloader area has been wiped
  installed            a device that already has a bootloader
  isp                  a device already in ISP mode
  isp-fails            a device where the write itself fails
  no-isp-entry         a device that never enters ISP mode after the jumper step
  no-isp-tool          a computer with no tool that can write to the chip
  two-devices          two things plugged in that could each be a GBFlash
```

---

## Why this needs the H1 jumper

The bootloader occupies flash `0x0000..0x3DFF`, and `0x0000` is the vector table the
device is executing from *right now*.

The CH579's erase granularity is 512 bytes, so writing anything into sector 0 means
erasing sector 0 first. The instant that erase completes, the device's vector table is
gone. It cannot take an interrupt, it cannot take a fault, and — more to the point —
the code doing the erasing is the application, which is being driven over USB by the
host, and USB is interrupt-driven in the application. There is no sequence in which a
follow-up "now program these bytes" command could ever arrive, because there is no
longer a working USB interrupt to deliver it.

This is not a matter of being careful. It is structural: any host-driven write to sector
0 destroys the machinery that would have delivered the next command. The write has to
come from somewhere that does not depend on CodeFlash at all, and that is the CH579's
ROM ISP.

**Installing firmware afterwards needs no jumper.** That asymmetry is the whole point of
the bootloader: it lives below `0x3E00` and never rewrites itself, so every subsequent
firmware update is a plain USB operation.

### And why you cannot flash `bootloader.bin` on its own

The same ROM ISP that makes the install possible also constrains what you can hand it.
`wchisp flash` — and every equivalent — **erases all of CodeFlash and writes from address
`0`**. There is no "write these sectors only" mode. Give it `bootloader.bin` by itself and
the erase takes the application with it: the device comes back with a working bootloader,
no firmware, and a fast-blinking LED — recoverable, but only by installing firmware over
USB, which is a detour you did not need.

So the image you flash has to contain the bootloader **and** an application, joined into
one file. That is what step 4 builds, and the natural source for the application half is
**your own backup from step 3** — which is why the backup is a prerequisite rather than a
precaution. It is also why no ready-to-flash image can be published: the application half
is vendor firmware, and this project does not redistribute that.

---

## What you need

- **`bootloader.bin`** — downloaded from a release, or built from source as
  `build/bootloader.bin` (see [BUILDING.md](BUILDING.md)). Either way it is 7,056 bytes.
  Its sha256 depends on the compiler that built it: for a download, the value to check is
  the one in that release's `SHA256SUMS`.
- **`tools/build_composite.py`**, and a Python 3.8-or-newer interpreter (standard library
  only).
- **A full CodeFlash dump of *your* device**, from `docs/backup-codeflash.py` — step 3.
  Either the default (application-sized) or `--all` (whole array) dump is accepted.
- **[`wchisp`](https://github.com/ch32-rs/wchisp)**, the ISP flashing tool. It publishes
  ready-built binaries for macOS (arm64/x64), Linux (x64/aarch64) and Windows (x64), so
  there is nothing to compile: download the one for your platform and put it on your `PATH`
  or next to `install.py`. Two commands are used here — `wchisp info` and `wchisp flash
  FILE`, and `flash` erases, writes, verifies and resets by default. It has been used
  successfully on the one device this project was tested on.

  wchisp is the only ISP tool this project drives. If it publishes nothing for your
  platform, [RECOVERY.md](RECOVERY.md#what-recovery-depends-on) covers driving another one
  by hand and the one thing to check before you do — some of them rewrite the user
  configuration byte, which is what H1 recovery depends on. WCH's own WCHISPTool on
  Windows is untested here.
- **Host permission to talk to the hardware.** On Linux the ISP tool needs libusb access to
  `4348:55E0` — a udev rule, or `sudo` — and the Python scripts need read/write on the
  serial port, commonly via the `dialout` group. Find this out during the step-2 rehearsal,
  not during a recovery.
- **Physical access to the H1 pads on the PCB**, and something to short them with.
- pyserial, for the two scripts in `docs/`: `python3 -m pip install pyserial`. On a
  PEP 668 host (Debian 12+, Ubuntu 23.04+, Fedora, Homebrew Python) that reports
  *externally-managed-environment*; use the system package, e.g. `sudo apt install
  python3-serial`, or a virtualenv.

---

## Step 1 — check that you need this

```sh
python3 docs/check-bootloader-region.py
# or, the same check through the installer:  python3 install.py --check
```

Read-only: it sends `QUERY_FW_INFO` (`0xA1`) and `GET_VARIABLE` (`0xAD`) to the running
application and reads `0x0000..0x3DFF` back. Nothing is written, no jumper is involved,
the device is not reset. Close FlashGBX first — only one process can hold the serial port.

**Confirm the verdict is `NO BOOTLOADER`.** If something is already there, stop and find
out what it is before writing over it. If firmware updates already work on your device,
you do not need this at all.

`--quick` samples one word per 512-byte sector instead of reading the whole region.
`--dump FILE` keeps the bytes, but that file is the bootloader region only and **is not a
backup** — step 3 is.

---

## Step 2 — prove you can enter ISP mode

**Do this before you write anything.** Every recovery from a bad install goes through the
ROM ISP, and the time to discover you cannot reach it is not after the write. Entering ISP
mode writes nothing and erases nothing; the device comes back to its normal firmware on the
next ordinary power-on.

1. **Unplug USB.**
2. **Short the two H1 pads.** (H1 is the ISP strap, not the U22 user button.)
3. **Plug USB back in while the short is held.** No LEDs light — that is correct.
4. **Remove the short.**
5. **Confirm `4348:55E0`** — `lsusb`, `system_profiler SPUSBDataType`, or Device Manager.
6. Unplug and plug back in normally. The application boots as before.

Run the ISP tool far enough to see it recognise the chip, too — `wchisp info` reports the
chip in one line. A tool you have never run is not a recovery path.

If `4348:55E0` did not appear, the short was not held across the moment power arrived.
Repeat until it does — nothing has been written, and nothing is at risk. If you cannot get
there at all, **stop here**: [RECOVERY.md](RECOVERY.md) explains what that means.

---

## Step 3 — take a full backup

```sh
python3 docs/backup-codeflash.py backup/device_full.bin
# or:  python3 install.py --backup backup/device_full.bin
```

Both read the same bytes with the same primitive and produce the same file; on the one
device this was run against, byte for byte. The installer additionally runs 21
restorability checks on the result and prints the `--restore` command that would put the
device back from it.

Read-only, no jumper, a few minutes. Do not skip it, for two independent reasons:

- the ISP write in step 5 erases the whole of CodeFlash, and this file is the only copy of
  your device's current firmware that you control;
- **step 4 builds the composite out of it.** Without a backup there is nothing to put in
  the application half, so there is no image to flash.

The script verifies the dump it just took and tells you if it is not usable.
[RECOVERY.md](RECOVERY.md) explains each check and why an unchecked dump is not a backup.

---

## Step 4 — build the composite install image

The ROM ISP writes CodeFlash from address `0` as a whole, so the image you flash must
contain the bootloader **and** an application. Build it from your own dump so that the
application half is exactly what your device already has:

The normal case, where your own backup is the only image you have:

```sh
python3 tools/build_composite.py \
    --bootloader build/bootloader.bin \
    --backup     backup/device_full.bin \
    --out        composite.bin
```

`--backup` is shorthand for `--app BACKUP --baseline BACKUP`. It ends in:

```
RESULT: PASS with 1 check(s) SKIPPED -- 25 ran, and the skipped ones are named above
```

**That is the expected result for this command, not a degraded one.** The skipped check
is `--compare`, which wants a second, independently obtained copy of the application; see
the box below.

If you *do* have the vendor `fw.bin` matching the version your device currently runs, add
it, and all 26 checks run:

```sh
python3 tools/build_composite.py \
    --bootloader build/bootloader.bin \
    --backup     backup/device_full.bin \
    --compare    /path/to/vendor/fw.bin \
    --out        composite.bin
```

```
RESULT: PASS -- composite is consistent
```

Either way, these four are the ones to look at with your own eyes:

```
[PASS] 0x0000 is NOT a copy of the application's vector table
[PASS] reset vector at 0x0004 is the bootloader's
       0x000000BD (app's would be 0x000040A5)
[PASS] composite differs from device_full.bin ONLY below 0x3E00 (note: same file as --app, so this is by construction)
       14978 bytes differ, all in [0x0000, 0x3E00); first 0x0004, last 0x3DFF
[PASS] application payload CRC is unchanged from the device image
```

"Differs only below `0x3E00`" is the property that makes this a bootloader install and
not a firmware change. If any byte at or above `0x3E00` differs, you are about to change
your firmware as well, and you should find out why before continuing.

> **How the tool treats missing and skipped inputs.** No argument has a default except
> `--bootloader`, which searches for `bootloader.bin` next to the script and then in
> `build/`. Every other named file must exist: a `--compare` or `--baseline` path that is
> not there is a **hard error** with the path quoted, exit 2, and no output file — not a
> silent omission. An *omitted* optional input is different: it is printed as `[SKIP]`,
> named again in the `RESULT:` line, and counted, so a weaker verification can never look
> like the full one. Omitting `--baseline` additionally prints *"Nothing has checked this
> image against the flash now on your device."* Check counts: 26 with everything, 25 with
> `--backup` alone (no `--compare`), 22 with a bare `--app` and no baseline.
>
> `--compare` pointed at the same file `--app` already names is reported as a `[SKIP]`
> rather than a `[PASS]`: comparing a file with itself is not a second copy, and counting
> it would clear the skip list and upgrade the `RESULT:` line on the strength of a
> tautology. If you have no independent copy, omit the flag deliberately and read the
> `LFBG`, payload-CRC and boot-info checks with extra care — they are what is left.

Either backup shape is accepted: the application-sized default and the whole-array
`--all` dump produce the same composite, byte for byte. With `--all`, the erased tail past
the end of the application is ignored and the run says so. If anything in that tail is
*not* erased the tool stops, because `--code-flash` would destroy it.

**Read that parenthesis, because it is telling you something true.** In the command
above `--app` and `--baseline` are the same file (`--backup` sets both), so "differs only
below `0x3E00`" cannot
fail: the tool copied the application half out of that file verbatim and put it back
unchanged. The tool says so rather than letting you read a tautology as evidence.

That is still the configuration you want here, and the property is still guaranteed —
just by construction rather than by measurement. Taking the application half from your
own dump is what makes this install leave your firmware alone, and it is why a wrong
`--compare` or a misaligned image shows up in the *other* checks (`LFBG` at `0x3E02`, the
payload CRC, the boot-info gates) rather than this one.

The check becomes a real measurement only when the application half comes from somewhere
else — a fresh vendor `fw.bin` passed to `--app`, with your device dump still in
`--baseline`. Then it is comparing two independently sourced things, and a difference at
or above `0x3E00` means you are changing firmware as well as installing a bootloader.
That is a legitimate thing to do deliberately; it is not what this page is about.

The layout block at the end should read like this:

```
layout
  0x0000-0x1B8F  bootloader   00 80 00 20 bd 00 00 00
  0x1B90-0x3DFF  0xFF fill    (8816 bytes)
  0x3E00-0xB51F  application  ff ff 4c 46 42 47 7d d2
```

`4c 46 42 47` is `LFBG`, the boot-info tag. Its presence at `0x3E02` is how you know the
application half is aligned correctly.

---

## Step 5 — write the composite over ISP

Enter ROM ISP mode exactly as you rehearsed it in step 2:

1. **Unplug USB.**
2. **Short the two H1 pads** to each other. (H1 is the ISP strap. It is a different pin
   from the U22 user button — do not confuse them.)
3. **Plug USB back in while the short is held.** No LEDs light. That is correct and
   expected: the ROM ISP does not drive them.
4. **Remove the short.** It is only needed at power-on.

The device is now enumerated as USB `4348:55E0`. Confirm it before going further —
`lsusb` on Linux, `system_profiler SPUSBDataType` on macOS, Device Manager on Windows.
If you do not see `4348:55E0`, the strap was not held at the right moment. Unplug and
repeat; nothing has been written and nothing is at risk.

Then write:

```sh
wchisp flash composite.bin
```

That erases the array, writes from address `0`, **verifies by reading back**, and resets —
so a success report means the write is good rather than merely attempted. (`-E`, `-V` and
`-R` turn off the erase, the verify and the reset respectively. Do not use them here.) It
does not touch the configuration registers at all; those are a separate `wchisp config`
subcommand.

**One command per ISP entry.** The ROM bootloader accepts a single session per
power-on. A second command in the same session fails with *"Chip is hosed. Reset or
power cycle it."* — that message is harmless and means exactly what it says. Unplug,
re-jumper, retry.

**Never write the user configuration word** — `wchisp config set`, `wchisp config reset`,
or any equivalent in another tool. Nothing in this project needs it, and that word is what
keeps H1 working. (`wchisp config info` only reads.)

---

## Step 6 — verify

**Remove the jumper if it is still on.** Unplug and plug back in normally, with nothing
shorting H1.

### (a) The device boots its application

The ACT LED should light steadily a moment after power-on, exactly as before. That means
the bootloader validated the application and handed off. Query it with any tool that
sends `QUERY_FW_INFO` (`0xA1`) — FlashGBX itself, a `gbflash_info`-style script, or the
first few lines of [`check-bootloader-region.py`](check-bootloader-region.py), which
prints the same block:

```
firmware: L15, PCB version 13, built 2026-06-03 10:45:02, 'GBFlash'
```

It should report the same firmware version, build timestamp and PCB revision it reported
before the install. The install did not touch the application, so nothing here should
have changed.

### (b) The bootloader is actually there

```sh
python3 docs/check-bootloader-region.py
```

```
initial SP at 0x0000    : 0x20008000
reset vector at 0x0004  : 0x000000BD
0x00B8..0x3DFF (complete): ... non-zero bytes of 15688 read

VERDICT: A BOOTLOADER IS PRESENT.
```

The reset vector at `0x0004` is now the bootloader's — below `0x3E00` — where before the
install it was the application's, at `0x4000` or above. That single word is the clearest
evidence the install landed.

### (c) The U22 button path — optional, NOT a pass/fail gate

> This is a second route into update mode, not a check on the install. Steps (a), (b) and
> (d) are the gates that decide whether the install succeeded — do not re-flash because of
> anything you see here.

Hold the **U22** button while plugging the device in, and keep holding for a second or
two. The ACT LED is expected to settle into a repeating **triple blink**: three short
blinks, then a pause, repeating on a 1.6-second cycle — update mode, entered by the
button.

If it is not what you see, the useful next step is to report that, not to change anything:
(d) below is the path every verified update actually used.

Either way, release, unplug and plug back in normally, and the device returns to the
application. The bootloader cannot trap you in update mode: the magic word is cleared on
the way through on every boot, and the button is only sampled at power-on.

### (d) A real firmware update

The final proof is an actual update. Run FlashGBX's Firmware Updater in the normal way,
or a serial updater script, and install the stock vendor `fw.bin`. If the device comes back
running the version you installed, it worked.

Do not read FlashGBX's own success dialog as evidence, though: its finalize error handling
reports success on failure. Every update quoted in this project was instead confirmed by
dumping `0x3E00` upward and comparing it byte-for-byte against the `fw.bin` installed, which
is the check to reach for if you ever need to be certain.

While the update runs, the ACT LED blinks **twice** per cycle rather than three times —
that is the pattern for update mode entered by the host — and visibly slows during the
transfer, because each 512-byte sector erase and program pauses the core.

When finalize succeeds the bootloader validates the new image and resets into it by
itself. The device disappears from USB for a fraction of a second and comes back running
the new firmware. No power cycle, no jumper.

---

## What each failure looks like

Wherever a row below says "write your backup back", the shortest way to do that is
`python3 install.py --restore backup/device_full.bin`, which guides the jumper, checks
the image first and issues one ISP command. The manual equivalent is in
[RECOVERY.md](RECOVERY.md).

| symptom | what it means | what to do |
|---|---|---|
| `4348:55E0` never appears | the H1 short was not held across power-on | unplug, repeat the entry sequence in step 5; nothing was written |
| *"Chip is hosed. Reset or power cycle it."* | a second command in one ISP session | unplug, re-jumper, retry with one command |
| the ISP tool reports a verify mismatch | the write did not take | retry from step 5. If it repeats, the image or the tooling is at fault, not the device — the device is still in ISP-recoverable state |
| after install: **dark board, no LED at all** | the application half of the composite is wrong, or the composite was misaligned | re-enter ISP and write your backup image back. Then rebuild the composite and read step 4's checks again |
| after install: ACT LED blinking **fast and continuously**, never pausing | the bootloader is running and has decided there is **no valid application**. It is sitting in update mode waiting for one | this is a recoverable state: install a stock `fw.bin` over USB with any updater. Or write your backup back |
| after install: ACT LED **triple-blinks** on every boot | U22 is being read as held — a stuck button, or you are still holding it | release the button and power-cycle. If it persists with nothing touching the button, that is a genuine bug worth reporting; the device is still fully usable over USB from update mode |
| after install: application boots, but `check-bootloader-region.py` still says `NO BOOTLOADER` | the ISP write did not land where you think, or you flashed a full-image build that copied the application's vectors to `0x0000` | check that you built with `tools/build_composite.py` and not a general full-image tool. See BUILDING.md |
| after install: firmware updates still fail | the bootloader is present but something else is wrong | run an update with a serial updater rather than the GUI so you can see the wire traffic, and check [PROTOCOL.md](PROTOCOL.md) for what each response should be |

Every row above is recoverable **if you can get back into ISP mode and you have your
backup** — the ROM ISP is in mask ROM and does not care what is in CodeFlash, so the entry
point is always there, but the entry point is not the whole of it. That is what steps 2
and 3 were for. See [RECOVERY.md](RECOVERY.md).

---

## Afterwards — updating firmware

From here on, firmware updates are an ordinary USB operation: **no jumper, no ISP tool, no
composite.** Run FlashGBX's Firmware Updater the way you would on any working GBFlash, or
a serial updater script, and point it at a stock vendor `fw.bin`. The bootloader receives
it, programs it, verifies it by reading flash back, and resets into it by itself.

`tools/build_composite.py` and the H1 jumper are install-time only. You should not need
either again unless you want to remove the bootloader, which is
[RECOVERY.md](RECOVERY.md)'s last section.
