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

# The zip is the only asset, and its digest is printed in this body rather than
# attached as a second file. Compute it HERE rather than inside the heredoc: a
# command substitution that fails in there does not abort the script, and would
# quietly publish a release whose checksum line is blank.
ZIP=$(ls "$DIST"/*.zip 2>/dev/null | head -1) || true
[ -n "${ZIP:-}" ] && [ -r "$ZIP" ] || {
    echo "release_body.sh: no zip in $DIST;" >&2
    echo "  run 'make dist' before composing the body." >&2
    exit 1
}
ZIP_NAME=$(basename "$ZIP")
ZIP_SIZE=$(( ( $(wc -c < "$ZIP") + 1023 ) / 1024 ))
if command -v sha256sum >/dev/null 2>&1; then
    ZIP_SHA=$(sha256sum < "$ZIP" | cut -d' ' -f1)
else
    ZIP_SHA=$(shasum -a 256 < "$ZIP" | cut -d' ' -f1)
fi

# The size line make emits after objcopy, and the checker's verdict.
size_line=$(grep -E '^\s*IMAGE' "$BUILD_LOG" | tail -1 | sed 's/^[[:space:]]*//')
check_line=$(grep -E '^RESULT:' "$CHECK_LOG" | tail -1)
# The per-suite summaries. '^fuzz:' deliberately does not match the "fuzzing
# bl_proto_feed:" progress line, and the two '[0-9]' anchors keep the per-suite
# totals while dropping the per-case narration those suites also print.
host_lines=$(grep -E 'assertions|^fuzz:|^rehearse_update: [0-9]|^test_install: [0-9]' "$HOST_LOG")
cc_line=$(grep -m1 -E 'arm-none-eabi-gcc \(' "$BUILD_LOG" || echo "unrecorded")

cat <<EOF
Some GBFlash cartridge flashers ship with the bootloader region blank. On those,
FlashGBX's Firmware Updater fails — usually a parse error, or no answer from the
device. This puts a bootloader back. Your firmware is left exactly as it is.

**If your firmware updates already work, you don't need this.** Nothing here
improves a working device.

### 1. Download

**\`$ZIP_NAME\`** (${ZIP_SIZE} KB), below. Extract it and open a terminal in that
folder.

Its sha256 is \`$ZIP_SHA\`.

You need Python 3.8 or newer and pyserial. You also need
[\`wchisp\`](https://github.com/ch32-rs/wchisp), which flashes the chip for the one
jumper step — \`install.py\` offers to download it for you.

### 2. Check the H1 pads first — this is the only way back

Installing erases all of CodeFlash. If that goes wrong, there is exactly one way
back: short the **H1** pads on the PCB while plugging the device in, and flash a
good image over USB with \`wchisp\`. So, while your device still works, check all
three:

- [ ] you can open the case and reach the H1 pads
- [ ] you have something to bridge them with — jumper, tweezers, wire
- [ ] with H1 shorted as you plug in, the device appears as USB \`4348:55E0\`

**If \`4348:55E0\` never appears, stop here. Do not install.**

**H1 is not the U22 button.** No LEDs light in that mode — a dark board is correct.

**Never write the CH579 user configuration word** — no \`wchisp config set\`, no
\`config reset\`. That word is what makes H1 work, and clearing it removes the way
back permanently. Nothing here writes it.

### 3. Install

Close FlashGBX, plug in one GBFlash and nothing else, then:

\`\`\`sh
python3 -m pip install -r requirements.txt
python3 install.py --check     # read-only, a few seconds: do you need this?
python3 install.py             # the install
\`\`\`

On Windows use \`py\` instead of \`python3\` — there, \`python3\` opens the Microsoft
Store. To rehearse the whole thing against a simulated device, touching no
hardware: \`python3 install.py --dry-run\`.

\`install.py\` backs your device up, builds the image from the bootloader and your
own firmware, walks you through the one jumper step, writes it, and reads back
what landed. **Keep the backup file it leaves you** — it is the way back:

\`\`\`sh
python3 install.py --restore YOUR-BACKUP.bin
\`\`\`

Afterwards, use FlashGBX's Firmware Updater as normal. No jumper, ever again.

---

**Tested on exactly one device: PCB v1.3, CH579M.** Other board revisions are
untested. Provided as-is, with no warranty; the author is not liable for damage,
data loss or a non-working device. You install it at your own risk.

Full instructions, troubleshooting and recovery are in the README — and in
\`README.txt\` inside the zip.

<details>
<summary>Gates this build passed</summary>

Built from source by GitHub Actions from the tag's commit. No vendor firmware is
contained in this release, and none is used to build it. This run built and
tested the code — **it did not touch a device.**

Compiler: \`$cc_line\`

\`\`\`
$size_line
$check_line
$host_lines
\`\`\`
</details>
EOF
