# Recovery

**Device will not boot, and you have a backup? This is the command:**

```sh
python3 install.py --restore YOUR-BACKUP.bin
```

You need the H1 jumper, `wchisp`, and the backup file. It talks you through the jumper,
checks the image, writes it, and confirms the device came back. It works on a device that
is completely dark.

No backup, and a fast continuously blinking ACT LED? That is the bootloader waiting for
firmware — [go here](#the-act-led-blinks-fast-and-continuously-never-pausing), no jumper
needed.

Below: what `--restore` checks, the manual equivalent, and specific symptoms.

---

Every recovery here is the same procedure — put the device into the CH579's ROM ISP mode
and write a known-good image over it. It works from any flash state, including completely
blank, and it has two prerequisites. Neither is optional.

| | |
|---|---|
| **1. You can reach H1 and flash over ISP.** | Physical access to the H1 pads, something to short them with, an ISP tool installed and working, and having done it once so you know it works. |
| **2. You have a backup of your device's flash.** | ISP writes CodeFlash from address 0 as a whole. Without an image to write, being in ISP mode gets you nowhere. |

Establish both **before** the first write. A sealed case you will not open, no ISP tool, no
way to bridge two pads — any of those and a bad write leaves you with a device you cannot
recover, so do not install this.

---

# 1. PROVE YOU CAN ENTER ISP MODE BEFORE YOU WRITE ANYTHING

Entering ISP mode is **read-only in itself** — nothing is written, nothing is erased, and
the device comes back to its normal firmware on the next ordinary power-on. Rehearse it
while the device still works:

1. **Unplug USB.**
2. **Short the two H1 pads** to each other. (H1 is the ISP strap, not the U22 user
   button — do not confuse them.)
3. **Plug USB back in while shorted.** No LEDs light. That is correct.
4. **Remove the short.**
5. **Confirm the device enumerates as USB `4348:55E0`** — `lsusb` on Linux,
   `system_profiler SPUSBDataType` on macOS, Device Manager on Windows.
6. Unplug and plug back in normally. The application boots as before.

If you saw `4348:55E0` at step 5, prerequisite 1 is satisfied and you know it rather than
assume it.

**Run the ISP tool far enough to see it recognise the chip, too.** A tool you have never
run is not a recovery path. [`wchisp`](https://github.com/ch32-rs/wchisp) needs no
compiler — download the build for your platform, and while the device is in ISP mode:

```sh
wchisp info
```

It should report `Chip: CH579`. It has been used successfully on the one device this
project was tested on, and it is the only ISP tool this project drives. If wchisp publishes
nothing for your platform, see
[driving another ISP tool by hand](#what-recovery-depends-on) — some of them write the user
configuration word, and that matters here more than anywhere else.

If you did **not** see `4348:55E0`, find out why now, while the device is still healthy and
there is nothing to recover from. The usual cause is that the short was not held across the
moment power arrived.

> **Do this before the backup, not after.** The backup is what you write; ISP entry is how
> you write it. A backup you cannot flash is not a recovery path either.

---

# 2. TAKE A BACKUP BEFORE YOU WRITE ANYTHING

**Read your device's flash to a file, and keep that file, before you install the
bootloader or run any experiment that writes CodeFlash.**

It is read-only. It needs no jumper. It takes a few minutes. It is the only copy of
*your* device's firmware that you control, and every recovery procedure on this page
assumes you have it.

```sh
python3 install.py --backup backup/device_full.bin
# the same read, standalone:  python3 docs/backup-codeflash.py backup/device_full.bin
```

Either is the command. `install.py --backup` runs 21 restorability checks on the file and
prints the `--restore` line that would put the device back from it; it is a mode on its
own, so it takes the backup and stops. The guided install takes the same backup as its
step 3 and will not go on without one that verifies.

[`docs/backup-codeflash.py`](backup-codeflash.py) reads flash over
USB with the read-only `GET_VARIABLE` primitive, needs no jumper, and never writes
anything. By default it reads the boot-info record first, takes the application length
from it, and reads `0x0000` up to the end of the application — `0x0000..0xB51F`, 46,368
bytes, on a stock L15 device. Save the file somewhere that is not the device.

**A dump you have not checked is not a backup**, so the script checks it for you and
prints the result. It confirms the file is the expected length, that `LFBG` — the
boot-info tag — is at `0x3E02`, that the boot-info record's own CRC checks out, that the
application payload CRC matches the CRC recorded in that record, that the application's
initial SP is SRAM-shaped, and that everything past the application is erased `0xFF`. If
any of that fails it says so and tells you not to rely on the file.

It writes incrementally, so an interrupted read leaves a short file rather than nothing,
and it checks the destination is writable *before* it starts reading rather than after.

> **`check-bootloader-region.py --dump` is not a substitute.** That one writes only
> `0x0000..0x3DFF` — the bootloader region — which is a diagnostic, not a restore image.
> You cannot put a device back from it.

Expect the read to take a few minutes: it is one USB round trip per 32-bit word.

---

## What recovery depends on

**Recovery is possible provided you can reach H1 and flash over ISP.** That condition is
the whole of it. Where it holds, nothing on this page is frightening; where it does not,
nothing on this page helps.

Read that as a prerequisite, not as reassurance. It is not a property of the device on its
own — it is a property of the device *plus* your access to it, your tools and your backup.
The three failure modes it does not cover are ordinary rather than exotic:

- **you cannot open the case, or will not** — H1 is on the PCB;
- **you have no ISP tool that works**, or have never run the one you have;
- **you have no image to write**, because you skipped the backup.

Any of those turns a bad write into a device you cannot get back. All three are cheap to
fix in advance and impossible to fix afterwards, which is why sections 1 and 2 above come
first and are written as gates rather than as advice.

**What the device does guarantee, and it is worth having.** The CH579's ISP bootloader
lives in **mask ROM**. It is not in CodeFlash, it cannot be erased, and it does not depend
on anything you can write. Shorting **H1** to GND at power-on enumerates the part as USB
`4348:55E0` regardless of what state CodeFlash is in — including completely blank. So no
sequence of flash writes can take the *entry point* away from you: a failed install, a
corrupted image, an interrupted update, a bootloader that spins at reset are all the same
recoverable situation, fixed by re-entering ISP and writing a known-good image.

The value of that is real and it is also narrow. It means the recovery path cannot be
destroyed by software. It does not mean you have one.

There is one thing that *can* destroy it: clearing `CFG_BOOT_EN` in the CH579's **user
configuration word**, which is what makes H1 work.

> **Never run `wchisp config set` or `wchisp config reset`, or any other tool's
> equivalent, that writes the user configuration word.** (`wchisp config info` only reads
> it.)

**Nothing in this project writes it.** The bootloader's flash driver is structurally
incapable of reaching it: writing InfoFlash requires setting `RB_ROM_DATA_WE` in
`R8_FLASH_PROTECT`, and the driver only ever writes two values to that register, neither
of which sets it. `make check` asserts that the value `0x8C` is never materialised in any
function that can reach that register, and that the user configuration word's address
appears nowhere in the image at all.

**The ISP tool is a separate matter**, because it is what you use to install the bootloader
and to recover.

**`wchisp flash` does not touch the configuration registers at all.** Reading and writing
them is a separate `wchisp config` subcommand, so a plain flash cannot disturb
`CFG_BOOT_EN`.

**If you drive some other ISP tool by hand, check this about it first.** Not every tool
leaves the configuration word alone the way wchisp does. The best-known alternative,
[`isp55e0`](https://github.com/frank-zago/isp55e0), **writes the configuration byte on
every `--code-flash` run** — `isp55e0.c` calls `write_config()` immediately after
`send_key()` and before erasing. What it writes is the block it just *read back from the
device*, with one bit forced: on CH579 it clears `CFG_ROM_READ` (bit 7), because the part
refuses to flash otherwise. `CFG_BOOT_EN` (bit 6) is echoed back exactly as the device
reported it, so H1 survives. That is the shape of question to ask of any tool you reach
for: does it write the configuration word, and if so, what does it put in bit 6?

`wchisp flash` on the test device left the bootloader intact at `0x0004` and the
application byte-identical, and H1 worked every time afterwards.

What you must never do with any tool is pass an explicit configuration value. Those
subcommands replace the byte outright with whatever you give them, and a value with bit 6
clear removes the ISP recovery path permanently.

---

## The restore

This is the procedure that gets you back from any flash state, given the two prerequisites
at the top of this page.

```sh
python3 install.py --restore backup/device_full.bin
```

**This works on a device that will not boot.** Everything up to and including the write
opens no serial port, imports no pyserial, and asks the running application nothing,
because on a dark device there is no application to ask. The ROM ISP is in mask ROM and H1
reaches it whatever is in CodeFlash. What it needs is the jumper, `wchisp`, and the file.

What it does, in order:

1. reads your image and prints its size;
2. runs the same gates the bootloader applies on-chip before it hands off to an
   application — `LFBG` at `0x3E02`, both CRCs, the length field against the payload
   actually present, an SRAM-shaped initial SP, a Thumb reset vector inside the image —
   **and the same kind of gates over the boot region below `0x3E00`**, which is the half
   the part actually starts executing and which no CRC in the image covers (see
   *[What is checked below 0x3E00](#what-is-checked-below-0x3e00)*). It **refuses to write
   a file that fails them**;
3. checks `wchisp` is present *before* asking you to touch the board — and offers to
   download it if it is not;
4. refuses if more than one device here could be a GBFlash, because the ISP write cannot
   be aimed at a particular board;
5. makes you confirm by typing `RESTORE`; a bare Enter is not a yes;
6. talks you through the jumper and waits for `4348:55E0` to appear, with a timeout;
7. issues exactly one flashing command — `wchisp flash` — and nothing else;
8. asks you to power-cycle, then confirms the device answers `QUERY_FW_INFO` and reads the
   whole image back to compare it against your file.

Step 8 is the only part that uses a serial port. If it cannot run — pyserial not
installed, or FlashGBX holding the port — **the restore is still reported as a success**,
and exits zero, with a note saying how to check it independently later. A restore that
worked is never announced as a failure, because the natural response to that message is to
erase the board and write it again. The write was verified by the ISP as it was made; a
port this computer cannot open afterwards says nothing about the flash.

### What is checked below `0x3E00`

The CH579 fetches its initial stack pointer from `0x0000` and its reset vector from
`0x0004` before a single instruction of anybody's code runs. Those 15,872 bytes are 34% of
a CodeFlash image and **nothing inside the image covers them** — the boot-info record's CRC
spans the application only. A word garbled during the serial dump therefore leaves a file
that looks like a perfect backup and restores to a dark board.

So `--restore` classifies the region as one of three shapes and refuses anything else:

| shape | what it is | restoring it gives you |
|---|---|---|
| bootloader | code from `0x0000`, then erased `0xFF` fill | a device that boots into the bootloader |
| stock | a vector table at `0x0000..0x00B7`, zeros to `0x3DFF` | a device that boots straight into the application |
| erased | the whole region is `0xFF` | **nothing — refused** |

and within the recognised shapes it requires an SRAM-shaped SP, a reset vector with the
Thumb bit that lands somewhere that exists, and every vector in the table pointing
somewhere plausible. When the bootloader is the published build it says so by sha256.

**The one thing it cannot check:** a bootloader carries no checksum over itself, so a
single flipped byte inside the *code* of a bootloader that is not the published build is
not detectable from the image alone. The run says so when it happens rather than implying
otherwise.

### Restoring over a device that still works

`--restore-unverified` justifies itself on the grounds that the device is already dead and
a bad image cannot make things worse. When the board is answering on a serial port that
argument is simply false, so the flag is refused on its own there: writing a known-broken
image over working firmware destroys the only good copy of it in existence, and someone
reaching for that flag by definition does not have another. The run tells you to back the
live device up first — `python3 install.py --backup my-device.bin` — which still works,
precisely because the device is alive. `--restore-over-a-working-device` overrides it if
you genuinely mean it.

It never passes `config`, `--user-config` or any equivalent to the ISP tool. That is
enforced in code rather than by convention: every ISP invocation goes through a guard that
raises if such an argument is present.

If the device is *already* dead and the file you have is your only copy, `--restore-unverified`
writes an image that failed step 2 anyway. That is a reasonable thing to try on a device
that is not booting: the H1 path is still there afterwards no matter what the write
produced, so a better image can always be written later.

### The same thing by hand

If you would rather drive the tools yourself, or the script cannot run where you are:

1. **Unplug USB.**
2. **Short the two H1 pads.** (H1 is the ISP strap, not the U22 user button.)
3. **Plug USB back in while shorted.** No LEDs light — that is correct.
4. **Remove the short.** It is only needed at power-on.
5. Confirm the device is enumerated as `4348:55E0`.
6. Write your backup:

```sh
wchisp flash backup/device_full.bin        # erases, writes, verifies and resets
```

   (If you are using a different ISP tool, read
   [what some of them do to the configuration byte](#what-recovery-depends-on) first.)

7. Unplug, plug back in normally, and confirm the device answers `QUERY_FW_INFO`
   (`0xA1`) with the firmware version and build timestamp your backup was taken from.
   [`check-bootloader-region.py`](check-bootloader-region.py) prints that block as its
   first two lines, and so does any `gbflash_info`-style script.

Steps 1 to 5 are exactly the rehearsal from section 1 — if you did that, you have already
done the part that can surprise you, and only step 6 is new.

Doing it this way, nothing checks the image before it is written. That is the one thing
the script adds that you cannot easily do by eye, so read
[INSTALLING.md](INSTALLING.md#step-4--build-the-composite-install-image) on what a sound
image looks like, or run `python3 install.py --restore FILE` far enough to see the check
list and stop at the confirmation prompt.

### Two things that will bite you

- **One command per ISP entry.** The ROM bootloader accepts a single session per
  power-on. A second command fails with *"Chip is hosed. Reset or power cycle it."* —
  harmless, and exactly what it says. Unplug, re-jumper, retry. If you need to restore
  CodeFlash *and* DataFlash, that is two power cycles. `install.py` is built around this:
  a restore issues one ISP command and no more.
- **Close FlashGBX first.** Only one process can hold the serial port, on either side of
  the recovery.

`wchisp flash` verifies after writing unless you pass `-V`, so a run that reports success
is a write that is good.

---

## Recovering from specific states

### The board is dark — no LED at all, no USB device

The application is not running. Either there is no valid application, or the bootloader
itself is not running either.

```sh
python3 install.py --restore backup/device_full.bin
```

[The restore](#the-restore) works from any flash state, including blank — the ROM ISP does
not read CodeFlash to decide whether to run, and `--restore` deliberately does not ask the
device anything before writing, so a dark board is not an obstacle to it.

### The ACT LED blinks fast and continuously, never pausing

**This is the bootloader telling you it found no valid application.** It is running
correctly, it has entered update mode on its own, and it is waiting for firmware. This
is a *good* state to be in — it is exactly the state the design intends after an
interrupted update.

You do not need the jumper. Install a stock vendor `fw.bin` over USB with FlashGBX's
Firmware Updater or a serial updater script, and the device will validate it and reboot
into it.

If you would rather go back to your backup,
`python3 install.py --restore backup/device_full.bin` works too.

### The ACT LED blinks twice, or three times, and then pauses

The bootloader is in update mode deliberately: twice means the host asked for it, three
times means U22 was held at power-on. Neither is a fault.

Unplug and plug back in without touching the button. The magic word that requests update
mode is cleared on the way through on every boot, so the device cannot be trapped there.

### An update was interrupted part-way through

This is the case the design is built around, and you should be fine — but read the caveat
at the end of this section before treating that as a guarantee.

The first data packet of an update erases the boot-info record *before* anything else is
written, so from that instant there is no valid application and any interruption lands
back in update mode rather than booting a half-written image. The boot-info record is only
written back, and only becomes valid, once the whole image has been received, CRC-checked
and read back from flash.

**The caveat:** that ordering is verified offline — by the argument in
[DESIGN.md](DESIGN.md) and by `t_interrupted_update` in the host test suite — but no update
has ever actually been interrupted on real hardware. Nobody has pulled the cable mid-write
on a device. The safety property rests on the flash model behaving as the real controller
does, and that model was written from the same analysis as the driver it tests. Sound
reasoning, no silicon behind it.

The device will come up in update mode with the fast blink. Run the update again.

### The installer stopped and said a bootloader is already present

Something is already in `0x0000..0x3DFF`, and it may not be this one. Find out what it is
before replacing it — and if firmware updates already work on your device, you do not need
this project at all. `--overwrite-existing-bootloader` exists for when you are certain;
read its help text first.

### Update mode never starts

The request is not reaching the bootloader. Holding **U22** at power-on is the alternative
route and does not depend on the application running at all. If neither route works, write
your backup back and check [PROTOCOL.md](PROTOCOL.md) for what each response should be.

### The update runs but the device does not come back

The bootloader refuses to jump to an image that fails its CRCs or whose vector table is
implausible; it stays in update mode instead. The device is still there and still
listening — send the firmware again.

A power cycle always returns to whatever valid application is installed, because the
update-mode magic word is cleared on the way through.

### An update reported success but the device looks dead

Unplug and plug back in. If it comes up, this was a host-side reporting artifact.

If it does not, `python3 install.py --restore backup/device_full.bin`, then read
`python3 install.py --check`'s output and [PROTOCOL.md](PROTOCOL.md) before retrying — a
genuine failure here is worth understanding rather than repeating.

### FlashGBX shows a hardware warning after installing

Not caused by the bootloader, and not a failed install. Some devices ship with modified
application firmware as well as a missing bootloader; installing an official firmware
release replaces that modified application with the vendor's own, which can surface a
hardware-registration check the modified firmware did not perform. That check is part of
the application, not the bootloader. If updates complete and the device boots the firmware
you installed, the bootloader is working.

Registration is out of scope for this project. A third-party community project exists for
research into it: <https://github.com/v1b3myC0d3/gbflash_unlocker> (not affiliated with
this project).

### The bootloader is installed and you want it gone

```sh
python3 install.py --restore backup/device_full.bin
```

Your original full-CodeFlash backup has the application's vector table at `0x0000` and zeros in
`0x00B8..0x3DFF`, so your **CodeFlash** returns to the state your backup captured —
including being unable to take firmware updates.

Two things are outside that, and both are documented on this page: the **user configuration
word**, which nothing here restores (and which some ISP tools rewrite on every run — see
[what recovery depends on](#what-recovery-depends-on)), and **DataFlash**, which this
project does not back up, read or write. "As it shipped" is therefore accurate about the
firmware and not about every byte in the part.

---

## What is not covered

**Nothing here helps if you cannot get into ISP mode.** Every procedure on this page ends
in one flashing command — including `install.py --restore`, which runs exactly that one
command and adds checks around it rather than replacing it. It needs the H1 pads, a jumper
or a wire, a working ISP tool, and an image to write. A device whose flash is wrong and
whose owner has none of those is not recoverable by anything in this repository, and no
script changes that. Section 1 exists so that you find this out at a moment when it costs
nothing.

Nothing here restores the CH579 **user configuration word**. That is deliberate: nothing in
this project's own code writes it and `wchisp flash` does not touch it, so it should never
need restoring. If it ever did — because some other tool cleared `CFG_BOOT_EN` — H1 would
no longer work and every procedure on this page would stop applying, permanently and for
good. That is the one failure that no amount of physical access or tooling gets you out of,
which is why the warning above is the strongest one in these docs.

Nothing here restores **DataFlash** (`0x3E800..0x3EFFF`). The bootloader does not read
it, write it, or depend on it in any way. If you have reason to back it up, do so
separately; restoring it is a separate ISP session.
