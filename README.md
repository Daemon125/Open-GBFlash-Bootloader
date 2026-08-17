# GBFlash CH579M update-mode bootloader

Some GBFlash cartridge flashers ship with the bootloader region blank. On those devices
FlashGBX's Firmware Updater has nothing to talk to, so firmware updates fail. This puts a
bootloader back. Your firmware is left exactly as it is, and updates go through FlashGBX
the ordinary way afterwards.

## Does your device need it?

If firmware updates already work, no. Nothing here improves a working device.

This is for devices where FlashGBX's Firmware Updater fails — usually a parse error, or no
answer from the device. To find out for certain:

```sh
python3 install.py --check
```

Read-only. No jumper, a few seconds, nothing written.

## No warranty

This software writes flash on hardware you own. It is provided as-is, with no warranty of
any kind. **The author is not liable for any damage, data loss, or non-working device
resulting from its use.** You install it at your own risk.

That is not boilerplate here. Installing erases all of CodeFlash, and recovery depends on
your being able to reach the H1 pads and flash over ISP — read the next section before you
write anything, and take the backup.

## Before you install: you must be able to reach H1

Installing rewrites the whole of the chip's CodeFlash. If that goes wrong there is exactly
one way back: short the **H1** pads on the PCB while plugging the device in, and flash a
good image over USB. So, before anything is written:

- [ ] you can open the case and physically reach the H1 pads
- [ ] you have something to bridge them with — jumper, tweezers, wire
- [ ] with H1 shorted as you plug in, the device appears as USB `4348:55E0`

**Try that last one now**, while the device still works. It writes nothing, and the device
boots normally on the next ordinary power-on. Six steps, in
[docs/RECOVERY.md §1](docs/RECOVERY.md#1-prove-you-can-enter-isp-mode-before-you-write-anything).

**H1 is not the U22 button.** **No LEDs light in that mode** — a dark board is correct.

**If `4348:55E0` never appears, do not install this.**

Never write the CH579 user configuration word — `wchisp config set`, `wchisp config reset`
or any equivalent. It is what makes H1 work, and clearing it removes the way back
permanently. Nothing here writes it.

## What you need

- **Python 3.8 or newer**, and pyserial: `python3 -m pip install -r requirements.txt`.
  If that reports *externally-managed-environment* — Debian 12+, Ubuntu 23.04+, Fedora,
  Homebrew Python — install the system package instead, e.g.
  `sudo apt install python3-serial`, or use a virtualenv.
- **[`wchisp`](https://github.com/ch32-rs/wchisp)** — it flashes the chip over USB for the
  one jumper step. Prebuilt binaries for macOS, Linux and Windows; nothing to compile.
  Simplest is to let `install.py` download it when it offers. Fetching it by hand on macOS
  needs the quarantine flag cleared first, or Gatekeeper blocks it:
  `xattr -d com.apple.quarantine ./wchisp`. Put it on your `PATH` or next to `install.py`.
- **The files.** From a release: download **`gbflash-bootloader.zip`** and extract it —
  that is everything, in one folder. From source: `make all`, see
  [docs/BUILDING.md](docs/BUILDING.md).

To check the download, from inside that folder: `sha256sum -c SHA256SUMS` on Linux,
`shasum -a 256 -c SHA256SUMS` on macOS. PowerShell has no equivalent one-liner —
`Get-FileHash` only prints hashes, it does not read `SHA256SUMS` — so on Windows:

```powershell
Get-Content SHA256SUMS | ForEach-Object {
  $h, $f = $_ -split '\s+', 2
  $a = (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower()
  if ($a -eq $h) { "OK   $f" } else { "FAIL $f" }
}
```

## Install

Close FlashGBX, plug in **one** GBFlash and nothing else, open a terminal **in the folder
you extracted**, then:

```sh
python3 install.py
```

On Windows that is `py install.py` — there, `python3` opens the Microsoft Store instead of
running Python.

It checks whether you need it, backs the device up, builds the install image from the
bootloader and your own firmware, walks you through the one jumper step, writes it, and
reads back what landed. It stops rather than continue past anything it cannot verify.

**Keep the backup file it leaves you.** It lands beside `install.py` as
`gbflash-backup-<date>.bin`, and it is the way back — move it somewhere that is not your
Downloads folder.

`python3 install.py --dry-run` runs the same thing against a simulated device and touches
nothing physical. Doing it by hand instead: [docs/INSTALLING.md](docs/INSTALLING.md).

## Afterwards

Use FlashGBX's Firmware Updater as normal. **No jumper, ever again.** Any serial updater
script that speaks the stock protocol works too.

During an update the ACT LED blinks twice per cycle and visibly slows during the transfer.
At the end the device drops off USB for a moment and comes back on the new firmware — no
power cycle needed.

FlashGBX's dialog can report success even when an update failed. The device coming back on
the new firmware version is the reliable sign.

## If something goes wrong

```sh
python3 install.py --restore YOUR-BACKUP.bin
```

That puts your backup back. It needs the jumper and `wchisp`, and it works on a device that
will not boot at all — it asks the device nothing before writing.

**A fast, continuous ACT blink is a good state.** The bootloader is running and waiting for
firmware. Install a stock `fw.bin` over USB with FlashGBX; no jumper needed.

### FlashGBX shows a hardware warning after installing

Not caused by the bootloader, and not a failed install. Some devices ship with modified
application firmware as well as a missing bootloader. Installing an official firmware
release replaces that modified application with the vendor's own, which can surface a
hardware-registration check the modified firmware did not perform. That check is part of
the application, not the bootloader.

If updates complete and the device boots the firmware you installed, the bootloader is
working. A third-party project exists for research into registration:
<https://github.com/v1b3myC0d3/gbflash_unlocker> (not affiliated with this one).

### The update runs but the device does not come back

The bootloader will not jump to an image that fails its CRCs, so it stays in update mode
instead. The device is still there and still listening — send the firmware again.

A power cycle always returns to whatever valid application is installed.

### Update mode never starts

Hold **U22** at power-on instead. That route does not depend on the application running at
all.

### An update reported success but the device looks dead

Unplug and plug back in. FlashGBX reports success even when finalize fails, so its dialog
is not evidence. If it does not come up, restore your backup.

---

Anything else — a dark board, other blink patterns, an interrupted update, *"Chip is
hosed"*, removing the bootloader again:
[docs/RECOVERY.md](docs/RECOVERY.md#recovering-from-specific-states).

**Getting help.** Open an issue on this repository's Issues tab. There is no other support
channel and no warranty; see [LICENSE](LICENSE). Include the output of `install.py
--check`, your PCB revision and firmware version, the command that failed and its full
output, and whether you have a backup. Problems with FlashGBX itself belong with
[FlashGBX](https://github.com/lesserkuma/FlashGBX).

## Commands

| | |
|---|---|
| `python3 install.py` | the guided install |
| `python3 install.py --check` | read-only: is a bootloader present? |
| `python3 install.py --backup FILE` | read-only: back up your device |
| `python3 install.py --restore FILE` | put a backup back |
| `python3 install.py --dry-run` | run against a simulated device, touching nothing |
| `python3 install.py --help` | every option |

## Status

**Tested on exactly one device: PCB v1.3, CH579M.** No other board revision has run any of
this. If yours is different, take the backup seriously.

On that device:

- four complete firmware updates with unmodified stock vendor `fw.bin`, both directions of
  the size change, each confirmed by reading flash back and comparing byte for byte;
- two complete `install.py` installs, run end to end, confirmed the same way — the second
  from a CI-built download, which is a different compiler's image entirely;
- `install.py --restore`, writing a backup back over ISP;
- entering update mode both ways — the software request, and U22 held at power-on;
- `wchisp` used successfully, both to identify the chip and to write a full image.

Full ledger, including every known limitation:
[docs/RELEASE-NOTES.md](docs/RELEASE-NOTES.md).

## Why this exists

I couldn't update my GBFlash. The bootloader region was empty — the device shipped with no
update mode at all — so FlashGBX's firmware updater had nothing to talk to, and every route
back to working firmware meant opening the case, fitting a jumper and flashing over ISP.

So I reverse-engineered the update protocol and wrote a replacement. It cost about 52% of a
week's Claude Max credits, which I consider a fair exchange rate.

## Attribution

- **[FlashGBX](https://github.com/lesserkuma/FlashGBX)** (lesserkuma) — the host software.
  Its source is the authority for the wire protocol, and its bundled `fw.bin` is the
  firmware this bootloader installs.
- **GBFlash hardware and original firmware** — simonkwng.
- **`gbflash_unlocker`** (v1b3myC0d3) — an independent third-party project whose README
  documented the boot-info record and the update protocol, and whose
  `gbflash_serial_update.py` drove three of the four verified updates.
- **[`wchisp`](https://github.com/ch32-rs/wchisp)** (ch32-rs) — the ISP tool this project
  drives, and the only one. [`isp55e0`](https://github.com/frank-zago/isp55e0) (Frank Zago)
  is the reference if you would rather drive the ROM ISP by hand — see
  [docs/RECOVERY.md](docs/RECOVERY.md#what-recovery-depends-on) first.
- The protocol was additionally cross-checked against `getserial.py` from the original
  author's archived site.

None of the above endorse this project or are responsible for it.

**MIT** — [LICENSE](LICENSE), which also records what the grant does not reach: the USB
descriptors in `src/usb_desc.c` and the CH340 VID/PID are transcribed from the GBFlash
firmware. Detail in the [considerations](TECHNICAL_DETAILS.md#licensing-considerations).

## Everything else

| | |
|---|---|
| [docs/INSTALLING.md](docs/INSTALLING.md) | the same install **by hand**, step by step |
| [docs/RECOVERY.md](docs/RECOVERY.md) | backups, `--restore`, and getting back from specific states |
| [TECHNICAL_DETAILS.md](TECHNICAL_DETAILS.md) | how it works, and why it is built this way |
| [docs/RELEASE-NOTES.md](docs/RELEASE-NOTES.md) | what is verified, what is not, every known limitation |
| [docs/BUILDING.md](docs/BUILDING.md) | toolchain, make targets, the host suites |
| [docs/DESIGN.md](docs/DESIGN.md) | the design reasoning in full |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | the wire protocol, byte-exact |

The three scripts `install.py` uses also run on their own:
[`build_composite.py`](tools/build_composite.py) builds the install image,
[`backup-codeflash.py`](docs/backup-codeflash.py) takes a backup, and
[`check-bootloader-region.py`](docs/check-bootloader-region.py) answers whether a
bootloader is present.
