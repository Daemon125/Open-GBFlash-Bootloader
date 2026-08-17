#!/usr/bin/env bash
# Compose the GitHub Release body from the logs of the build that produced the
# artefacts, so the numbers in the release are the numbers CI actually ran
# rather than figures copied from documentation.
#
#   release_body.sh TAG BUILD_LOG CHECK_LOG HOST_LOG DIST_DIR > body.md
#
# Every quoted figure is grepped out of a log written by this run.  Nothing
# here is hard-coded except the prose.
set -euo pipefail

TAG=${1:?tag}
BUILD_LOG=${2:?build log}
CHECK_LOG=${3:?check log}
HOST_LOG=${4:?host log}
DIST=${5:?dist dir}

# The checksum block is read with $(cat ...) inside a heredoc, where a failure
# does NOT abort the script -- it would quietly emit a release body with an
# empty checksum block, which is worse than no release. Check it here instead.
[ -r "$DIST/SHA256SUMS" ] || {
    echo "release_body.sh: $DIST/SHA256SUMS is missing or unreadable;" >&2
    echo "  run 'make dist' before composing the body." >&2
    exit 1
}

# The size line make emits after objcopy, and the checker's verdict.
size_line=$(grep -E '^\s*IMAGE' "$BUILD_LOG" | tail -1 | sed 's/^[[:space:]]*//')
check_line=$(grep -E '^RESULT:' "$CHECK_LOG" | tail -1)
# The per-suite summaries. '^fuzz:' deliberately does not match the "fuzzing
# bl_proto_feed:" progress line, and the two '[0-9]' anchors keep the per-suite
# totals while dropping the per-case narration those suites also print.
host_lines=$(grep -E 'assertions|^fuzz:|^rehearse_update: [0-9]|^test_install: [0-9]' "$HOST_LOG")
cc_line=$(grep -m1 -E 'arm-none-eabi-gcc \(' "$BUILD_LOG" || echo "unrecorded")

cat <<EOF
GBFlash CH579M update-mode bootloader — \`$TAG\`

Built from source by GitHub Actions from the tag's commit. No vendor firmware
is contained in this release, and none is used to build it.

## How to install this

**Download \`gbflash-bootloader.zip\`, extract it, and open a terminal in the
folder it makes.** Everything \`install.py\` needs is in there. (The same files
are attached individually below if you would rather pick them out; put them all
in one directory.)

Linux:

\`\`\`sh
sha256sum -c SHA256SUMS
python3 -m pip install -r requirements.txt
python3 install.py
\`\`\`

macOS:

\`\`\`sh
shasum -a 256 -c SHA256SUMS
python3 -m pip install -r requirements.txt
python3 install.py
\`\`\`

Windows (PowerShell):

\`\`\`powershell
Get-Content SHA256SUMS | ForEach-Object {
  \$h, \$f = \$_ -split '\s+', 2
  \$a = (Get-FileHash -Algorithm SHA256 \$f).Hash.ToLower()
  if (\$a -eq \$h) { "OK   \$f" } else { "FAIL \$f" }
}
py -m pip install -r requirements.txt
py install.py
\`\`\`

If pip reports **externally-managed-environment** — Debian 12+, Ubuntu 23.04+,
Fedora, or Homebrew Python — install the system package instead, for example
\`sudo apt install python3-serial\`, or use a virtualenv.

\`install.py\` is the whole procedure. It checks whether your device needs the
bootloader at all, backs your device up, builds the install image out of the
bootloader and *your own* firmware, talks you through the one jumper step,
writes it, and checks what landed. It stops rather than continue past anything
it cannot verify, and it finds the other files automatically when they are
beside it.

You also need [\`wchisp\`](https://github.com/ch32-rs/wchisp), which flashes the
chip for the one jumper step. It ships ready-built binaries for macOS, Linux and
Windows, so there is nothing to compile; put it on your \`PATH\` or next to
\`install.py\`. If you have not got it, \`install.py\` offers to download it and
will not do so unless you say yes — which is the easiest route, and on macOS it
also avoids Gatekeeper. If you download it by hand there instead, clear the
quarantine flag or it will refuse to run:
\`xattr -d com.apple.quarantine ./wchisp\`. It has been used successfully on the
one device this project was tested on.

To see exactly what it will ask you, before you have a board in front of you:

\`\`\`sh
python3 install.py --dry-run
\`\`\`

If a device ever stops booting, that same script is the way back:

\`\`\`sh
python3 install.py --restore YOUR-BACKUP.bin
\`\`\`

## What to download

| file | what it is |
| --- | --- |
| \`gbflash-bootloader.zip\` | **everything below, in one download — start here** |
| \`install.py\` | **the guided installer.** Also \`--check\`, \`--backup\` and \`--restore\` |
| \`bootloader.bin\` | the bootloader image. **Not flashable on its own** — see below |
| \`requirements.txt\` | the one Python dependency (pyserial) |
| \`build_composite.py\` | combines \`bootloader.bin\` with your backup into the image you flash |
| \`check-bootloader-region.py\` | read-only: tells you whether you need this at all |
| \`backup-codeflash.py\` | read-only: takes the full-flash backup you must have first |
| \`README.txt\` | what to run, in the folder itself |
| \`LICENSE\` | MIT |
| \`SHA256SUMS\` | checksums for everything above |

\`\`\`
$(cat "$DIST/SHA256SUMS")
\`\`\`

The three helper scripts also run standalone, and \`install.py\` uses
\`build_composite.py\` as an independent second opinion on the image it builds.
If you would rather do the whole thing by hand, \`docs/INSTALLING.md\` in the
repository is the same procedure step by step, with the reasoning.

## Before you write anything: recovery depends on reaching H1

Installing means erasing all of CodeFlash and writing it again from address 0.
When that goes wrong there is one way back: short the **H1** pads on the PCB at
power-on to reach the CH579's ROM ISP, and write a good image over it. That
entry point is in mask ROM and cannot be erased — but an entry point you cannot
physically reach is not a recovery path.

**So recovery is possible provided you can reach H1 and flash over ISP.** That
is a condition, not a guarantee. You need to be able to open the case, something
to bridge two pads with, and an ISP tool you have installed and run.
\`install.py\` checks \`wchisp\` exists before it asks you to touch the board,
and it will not write without a backup that verifies, but it cannot check
whether you can open your case. **Rehearse entering ISP mode before your first
write; if \`4348:55E0\` never appears, do not install this.**

Never write the CH579 user configuration word — \`CFG_BOOT_EN\` in it is what
makes H1 work, and clearing it removes the recovery path permanently. Do not run
\`wchisp config set\` or \`wchisp config reset\`, or any equivalent.
(\`wchisp config info\` only reads, and is safe.) \`install.py\` refuses to pass
any such argument to the ISP tool.

## Why there is no ready-to-flash image here

The CH579's ROM ISP erases CodeFlash and writes it from address 0 — that is true
of \`wchisp flash\` and of every equivalent. There is
no "write these sectors only" mode. So the image you flash has to contain the
bootloader **and** an application — flashing \`bootloader.bin\` by itself erases
your firmware along with everything else and leaves a device with a bootloader
and nothing to boot.

The application half is your device's firmware, which this project has no right
to redistribute. So the image is built locally, out of your own backup, which
\`install.py\` takes for you as step 4 of the install. Your own dump is the
correct source anyway: it is by definition the firmware that device is already
running. The application half is copied out of it **verbatim** — nothing is
recomputed, re-CRC'd or re-padded — so nothing at or above \`0x3E00\` can
change. That is guaranteed by construction rather than measured, and both tools
label it as such rather than presenting it as evidence. What *is* measured is
that the image is sound: the bootloader's vector table, the boot-info record's
own CRCs, the \`LFBG\` tag at \`0x3E02\`, the payload CRC, and that \`0x0000\`
holds the bootloader's vector table and not a copy of the application's.

To build it by hand instead, from the directory holding these files:

\`\`\`sh
python3 build_composite.py --backup my-device-backup.bin --out install.bin
\`\`\`

(\`bootloader.bin\` is found next to the script; no path needed.) That run ends
in:

\`\`\`
RESULT: PASS with 1 check(s) SKIPPED -- 25 ran, and the skipped ones are named above
\`\`\`

**That is the expected result, not a fault.** The skipped check is the optional
\`--compare\`, which wants a *second, independently obtained* copy of your
application (a vendor \`fw.bin\`) to cross-check against. Most people have only
their own backup, and comparing that file with itself would prove nothing, so
the tool reports the check as skipped instead of quietly counting it as a pass.
A run that fails writes no output file and exits non-zero, so there is nothing
to flash by accident.

## Gates this build passed

Toolchain: \`$cc_line\`

\`\`\`
$size_line
$check_line
$host_lines
\`\`\`

<details>
<summary>full host suite log</summary>

\`\`\`
$(cat "$HOST_LOG")
\`\`\`

</details>

The end-to-end rehearsal is driven in CI by **synthetic** images from
\`host/make_synthetic_fw.py\`, not by vendor firmware. Where its output says
"stock-fw.bin", that describes the image shape. The developer-run version of
this suite against real firmware is described in the repository.

\`bootloader.bin\`'s checksum is a property of the compiler that built it. A
different \`arm-none-eabi-gcc\` produces a different, equally valid image, so
the value above is a record of this build and not a constant to match.

## Testing limitations — please read

Hardware verification for this project was performed on **one device**: a PCB
v1.3 GBFlash with a CH579M. Four firmware updates, one complete \`install.py\`
install and one \`install.py --restore\` were completed on it, each verified by
reading flash back and comparing byte for byte. Both routes into update mode
were exercised: the software request, and U22 held at power-on.

That is one device. Other PCB revisions, other CH579 variants and other host
setups are **untested**.

Specific gaps:

- The USB and flash suites test the code against models written from the same
  analysis as the code itself, so a misreading of the hardware could be
  reproduced identically on both sides.
- This CI run built and tested the code. **It did not touch a device.**

Take the backup. Confirm you can get into ISP mode before you write anything.
EOF
