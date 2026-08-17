# Release notes

The honest inventory: what this is, what has been shown to work and on what hardware, what
has never been exercised, and where the verification is weaker than it looks. Every
limitation below is one that could change what you decide to do.

---

## What this is

An update-mode bootloader for the WCH **CH579M** on **GBFlash** cartridge flashers.

Some GBFlash devices ship with flash `0x00B8..0x3DFF` programmed with zeros — no
bootloader at all. On those devices the application's `BOOTLOADER_RESET` command writes
its magic word, resets, and lands in nothing, so firmware updates cannot run. This
project fills that region and makes updates work.

It occupies `0x0000..0x3DFF` (15,872 bytes; the image uses 7,056 of them). It owns the
reset vector, validates the application at `0x4000`, and hands off. On request it
enumerates as a CH340 serial device, receives an unmodified vendor `fw.bin`, programs it,
verifies it by reading flash back, and reboots into it.

It does not modify, patch, re-sign or inspect the application beyond the validity gates.
It does not write DataFlash, InfoFlash or the user configuration word.

**The build is deterministic for a given toolchain.** `build/bootloader.bin` is 7,056
bytes; with the toolchain named in [BUILDING.md](BUILDING.md) its sha256 is
`4cf2a387bf62f1d64dfbbb145681fd0e107b0698d812e23686ce31b66d00d4cd`. A different
`arm-none-eabi-gcc` produces a different, equally valid image, so that value is a record
of one build rather than something to match. The digest of a released binary is the one
in that release's `SHA256SUMS`.

---

## Verified on hardware

**On exactly one device: PCB v1.3, CH579M.** Everything in this section means "on that
device". See [Limitations](#limitations) for what that does and does not license you to
conclude.

### Four complete firmware updates, with unmodified stock vendor `fw.bin`

| # | transition | driven by | result |
|---|---|---|---|
| 1 | L15 -> L14 | `gbflash_serial_update.py` (third party) | flash byte-identical to the stock file |
| 2 | L14 -> L15 | `gbflash_serial_update.py` | flash byte-identical to the stock file |
| 3 | L15 -> L14 | `gbflash_serial_update.py` | flash byte-identical to the stock file |
| 4 | L14 -> L15 | FlashGBX's own GUI Firmware Updater | flash byte-identical to the stock file |

Every one was checked **by reading flash back and comparing byte-for-byte against the
stock file**, not by trusting the updater's success message. That distinction is
load-bearing: FlashGBX's finalize error handling reports success on failure, so its
dialog is not evidence of anything.

Both directions of the size change are covered (`applen` `0x7520` vs `0x7514`), so length
and CRC handling are genuinely exercised rather than the same bytes being rewritten.

No power cycle and no jumper were in the loop. Update mode was entered in software, and
after finalize the bootloader validated the new image and reset into it by itself —
confirmed by `R8_RESET_STATUS` reading `RST_FLAG_SW` on each new firmware's first boot.

### Also confirmed on silicon

- **The boot path and handoff.** The bootloader owns `0x0000`, validates the application
  and enters it; reset vector read back as `0x000000BD`.
- **The IPSR exception trampoline.** ARMv6-M has no VTOR, so exceptions are forwarded to
  the application's table at `0x4000` by a shared dispatcher. USB is IRQ6, so every byte
  of verification traffic to the running application went through it.
- **The update-mode magic path.** `0xAA55BB01` at `0x20000090` plus `SYSRESETREQ` enters
  update mode, and the magic is cleared on the way through — `0x20000090` reads zero
  afterwards every time, so the device cannot be trapped in update mode.
- **The brown-out NMI disarm** in `start.S`, and the application re-arming it after boot.
- **CH340 enumeration** as `1A86:7523`.
- **Both host implementations' quirks.** FlashGBX sends `0x21` twice with the same
  sequence number and discards the first response; it also re-sends stray `F1`/`01`
  trigger bytes after `BootloaderReset()`. Both are absorbed by the rolling intro hunt,
  and both happened on real hardware during update #4.
- **A complete `install.py` install, run end to end**, on the same device: check, backup,
  composite, the jumper step, the write, and the post-install read-back. Confirmed the same
  way the updates were — all of flash read back and compared byte for byte against the
  image written.
- **ISP flashing with `wchisp`.** `wchisp info` identified the part, and `wchisp flash` of a
  full composite erased 47 sectors, wrote 47,104 bytes, verified, and left the bootloader
  intact at `0x0004` with the application byte-identical to the stock file. H1 has worked
  after every write.
- **`install.py --restore`**, writing a backup back to the device over ISP and verifying it.
  This is the mode you reach for when something has already gone wrong, so it mattered that
  it was exercised rather than only simulated.
- **A second, independently compiled build.** Everything else in this section was clang
  15.3.1 output. A CI build — GCC on `ubuntu-latest`, 7,144 bytes against the developer
  build's 7,056, a wholly different image — was downloaded as a workflow artifact and
  installed on the same device with `install.py`, then read back and compared byte for
  byte. So the bootloader does not depend on one compiler's codegen, and the release
  payload has been installed on hardware exactly as a downloader would receive it: the
  zip's checksums, the repository URL substituted into its `README.txt`, and the
  `BL_SHA256` stamp binding that `install.py` to that `bootloader.bin` were all verified
  on the downloaded copy.
- **Both routes into update mode.** The software request (magic word plus `SYSRESETREQ`) is
  what all four verified updates used. Holding **U22 at power-on** was also tried on the
  device: the board stayed dark, which is update mode entered correctly, and it returned to
  the application on the next ordinary power-on with the magic word still clear — so the
  button path does not leave anything latched behind it.

---

## Not verified on hardware

### Other board revisions

Nothing here has run on a v1.2 or earlier board, or on any CH579 part other than the
CH579M on this one PCB. That is the limitation that matters: everything above is a
statement about one device.

### `src/led.c` is never compiled natively

The one shipping module with no host-side test. Its failure mode is cosmetic — a wrong
blink pattern — and `make check` asserts that the LED is driven only from update mode.
But it is untested code.

### The publishing pipeline itself, on its first release

The staged payload itself has been exercised: CI builds it with the same `make dist` a
release uses, and one such artifact was downloaded and installed on the device (see the
second-compiler entry above). What has **never run** is `.github/workflows/release.yml`
end to end — the publish step, `gh release create`, and the assertion that reads the
attached asset list back from the API. Those fire for the first time with the first tag.
The gates ahead of them are real, and CI covers every one of them, but a gate that has
never fired in its own workflow is a gate nobody has watched work.

The binary in a release is also **built by CI, not on the developer's machine**, so its
sha256 is not the one quoted in [BUILDING.md](BUILDING.md) — and neither is its size.
That is expected: the digest to check a download against is the one in that release's own
`SHA256SUMS`, and the `install.py` published alongside carries the same value stamped
into it.

---

## How strong the offline verification actually is

The host suites are thorough, and it would be easy to over-read them. Two honest caveats.

### The USB and flash models were written from the same analysis as the code

`test_usb` runs the real `src/usb.c` against a model of the SIE. `test_flash` runs the
real `src/flash.c` against a register-level model of the CH579 flash controller. Those
models are good — they are register-accurate, they inject faults, and `test_flash`'s
single-fault sweep NAKs all 667 injected failures — but **they were written by the same
process, from the same datasheet reading and the same reverse-engineering notes, as the
driver they are testing.**

A shared misunderstanding of the silicon therefore does not show up as a test failure. It
shows up as a model and a driver that agree with each other and both differ from the
part. That class of error is exactly what hardware testing exists to catch, and it is why
the four verified updates matter more than the 208 flash assertions.

What bounds the risk is the design rather than the tests: every erase is followed by a
full-sector blank check and every programmed word by a read-back compare, so a wrong
assumption surfaces as a NAK the host retries rather than as silent corruption.

Where the evidence is genuinely independent, it is worth saying so: `test_proto` replays
byte streams recorded from **three real host implementations** (FlashGBX's updater
library, FlashGBX's GUI path, and a third-party serial updater) and requires the device's
responses to match what those hosts actually accepted. That is not a model of a host; it
is the hosts.

One qualification on that, since it is easy to over-read too. The *hosts* are real; the
*firmware image* they were driven with when the fixtures were recorded is **synthetic**,
built by `host/make_synthetic_fw.py`, because this repository ships no vendor firmware.
The protocol layer never inspects payload meaning — only framing, CRCs, sequencing and
lengths — so a structurally valid image exercises the same paths, and every recorded
conversation is 63 device frames of the real thing. But what is pinned is how those hosts
behave, not how they behave against one particular vendor build. The four hardware updates
are the only place a real vendor `fw.bin` was end-to-end.

### The offline rehearsal is a rehearsal

`rehearse_update` runs three consecutive full updates end to end — real `proto.c`, real
`flash.c`, flash-controller model, real `bl_app_valid()` — and asserts the resulting array
is byte-identical to the source image and would boot. It is the strongest offline suite
here, and it is still running against the model above.

It is also the one suite that does **not** run by default, because it needs two real
`fw.bin` images this repository does not carry. The 80 checks quoted below are real, but
they are only reproducible by someone who supplies their own images. A plain `make -C host
test` prints `rehearse_update: SKIPPED` and names the paths it looked for.

---

## Limitations

- **One device.** PCB v1.3, CH579M. Everything in "Verified on hardware" is a statement
  about that board.
- **Installing the bootloader requires the H1 jumper.** This does not go away. Sector 0
  holds the live vector table, and erasing it from the host destroys the machinery that
  would deliver the next command — the code doing the erasing is the application, driven
  over interrupt-driven USB. So the first write must come from somewhere that does not
  depend on CodeFlash at all, which is the CH579's ROM ISP. Installing *firmware*
  afterwards needs no jumper; that asymmetry is the whole point.
- **You need physical access to the H1 pads**, and `wchisp`
  (<https://github.com/ch32-rs/wchisp>) — prebuilt binaries for every platform this runs
  on, no compiler. It is the tool `install.py` drives, and it will fetch a release binary
  over HTTPS if you answer yes when it asks; it never downloads anything otherwise. It is
  the only ISP tool this project drives; anything else has to be driven by hand, and
  [RECOVERY.md](RECOVERY.md#what-recovery-depends-on) says what to check first. WCH's
  WCHISPTool is untested here. An ISP tool is also the precondition for every recovery
  procedure — see [What recovery depends on](#what-recovery-depends-on) below.
- **`bootloader.bin` is not flashable on its own.** The ROM ISP erases all of
  CodeFlash and writes from address 0, so the image you flash must carry an application
  as well or the device comes back with a bootloader and nothing to boot. That composite
  is built locally against **your own backup** — which is a second reason the backup is
  mandatory rather than advisory — by `install.py` as part of the guided install, or by
  `tools/build_composite.py` if you are doing it by hand. No composite is published,
  because publishing one would mean redistributing someone else's firmware.
  See [INSTALLING.md](INSTALLING.md).
- **Only one process can hold the serial port.** Close FlashGBX before running
  `install.py` or any script in `docs/`, and vice versa.
- **The ROM ISP accepts one command per power-on.** A second in the same session fails
  with *"Chip is hosed. Reset or power cycle it."* Unplug, re-jumper, retry.
- **`wchisp flash` does not touch the configuration registers at all** — they are a
  separate `wchisp config` subcommand, which `install.py`'s ISP guard forbids. Other ISP
  tools are not all like this; some rewrite the user configuration byte on every run. If
  you drive one by hand, read
  [RECOVERY.md](RECOVERY.md#what-recovery-depends-on) first. Never write an explicit
  configuration value with any tool.
- **One host suite needs firmware images from outside the tree.** `rehearse_update` runs
  three full updates against two real `fw.bin` files supplied via `FW_L14=`/`FW_L15=`. The
  repository ships none, and must not acquire any. Without them that one suite is skipped
  and `make -C host test` still exits 0, saying what it skipped.
- **MIT licensed**, except the 83 bytes of USB descriptors transcribed from the GBFlash
  firmware, which are not this project's to license. See [LICENSE](../LICENSE).

---

## Known issues

### Resolved: the PCB revision your device reports can change between boots

**Not an installer defect.** `install.py` reports what the device tells it, and the
device does not always say the same thing.

**Reproduced.** Reading the revision across eight consecutive boots of one board gave
`12` once and `13` seven times. On every boot the two independent sources — the
`QUERY_FW_INFO` reply and the RAM byte at `0x200000AC` — agreed with each other. So
nothing is being misparsed or cached; the firmware genuinely latches a different value
on some boots.

**Cause.** The application detects the board revision by sampling **PB22** as an input
with a pull-down, ~1 ms after configuring it, and calls a high reading v1.3
(`sub_5CAC`). The byte is `.data`-initialised to 12 and only raised to 13 by that
sample. A pull-up that has not fully settled within the 1 ms window reads low, and the
board is recorded as v1.2 for that boot.

**What it affects.** Nothing about installing or updating — `pcb_ver` has no bearing on
whether a write landed, and the bootloader never touches PB22. It does affect the
*application*: `pcb_ver >= 13` gates the cart/LED task, and the cart presence-switch
path returns early below it ("older boards have no switch"). So on a boot that reads 12,
a v1.3 board behaves as a v1.2 one until it is power-cycled.

**If you see it.** A revision that disagrees with your board, or with itself between two
runs, is this. Power-cycle and it will most likely read correctly again. `install.py`
never acts on the value — it prints it, and reports a before/after disagreement as a
warning — so it is not a reason to restore a backup. It is worth knowing about, but it is
a property of the stock firmware and the board, not something this project introduced or
can fix from the bootloader.

---

## What recovery depends on

This changes the risk calculus of everything above, so it is worth stating precisely
rather than reassuringly.

**Recovery is possible provided you can reach H1 and flash over ISP.** That is a
conditional, and the condition is about you and your bench, not about the silicon:

- physical access to the H1 pads, which means opening the case;
- a jumper, a wire or tweezers to bridge them;
- an ISP tool you have installed and run — `wchisp`
  (<https://github.com/ch32-rs/wchisp>), which needs no compiler, is what `install.py`
  drives, and has been used successfully on the test device;
- a backup image to write, because ISP writes CodeFlash from address 0 as a whole.

Miss any of those and a bad write leaves you with a device you cannot get back. **If you
cannot satisfy all four, do not install this.**

What the part guarantees is narrower and still valuable: the CH579's ISP bootloader lives
in **mask ROM**, cannot be erased, and does not depend on anything writable. Shorting H1
to GND at power-on enumerates it as USB `4348:55E0` whatever CodeFlash contains, including
completely blank. So no failed install, corrupted image, interrupted update or bootloader
that spins at reset can take the *entry point* away. The recovery path cannot be destroyed
by software — which is not the same as your having one.

The single thing that *would* destroy it is clearing `CFG_BOOT_EN` in the user
configuration word. Nothing here writes it. `make check` asserts that the value that
would make InfoFlash writable is never materialised in any function that can reach the
protection register, and that the configuration word's address appears nowhere in the
image at all.

So, before you write anything, in this order:

1. **Prove you can enter ISP mode.** Short H1, plug in while shorted, confirm
   `4348:55E0`, unplug, boot normally. It writes nothing and costs one minute, and it is
   the step that turns a later mistake into a five-minute fix. **No script can do this
   one for you** — it is the reason the warning is in [README.md](../README.md) as well
   as here.
2. **Take a full backup.**

```sh
python3 install.py --backup backup/device_full.bin
# the same read, standalone:  python3 docs/backup-codeflash.py backup/device_full.bin
```

The guided install (`python3 install.py`) takes that backup itself and refuses to write
without one that verifies, and `python3 install.py --restore backup/device_full.bin` is
what puts it back. [RECOVERY.md](RECOVERY.md) is the long form of both, and of every
recovery procedure that depends on them.

---

## Verification you can reproduce

From a clean tree, with no hardware:

```
make all
    FLASH:        7056 B      15872 B     44.46%
    RAM:          3420 B        32 KB     10.44%
    IMAGE   7056 bytes / 15872 budget (8816 free)

make check
    RESULT: PASS — 68 checks, 0 failures, 0 warnings

make -C host test
    77 assertions, 158446 executions passed, 0 failed          (timebase)
    test_flash: 208 assertions / 241 executions, 0 failures
    193 assertions, 305 executions passed, 0 failed, 1 notes   (usb)
    216 assertions, 1836 executions passed, 0 failed, 0 notes  (proto)
    fuzz: 30000 iterations, 41314040 bytes fed, 524562 response bytes,
          109645 idle notifications, 0 failures
    rehearse_update: 80 checks, 0 failures
    test_install: 540 checks, 0 failures

sha256sum build/bootloader.bin        (shasum -a 256 on macOS)
    4cf2a387bf62f1d64dfbbb145681fd0e107b0698d812e23686ce31b66d00d4cd
```

Every line above except `rehearse_update` runs in a plain checkout with nothing supplied.
Only `rehearse_update` needs firmware images from outside the tree, via
`FW_L14=`/`FW_L15=`; without them it is skipped with a printed note naming the paths it
looked for, and `make -C host test` still exits 0. Another binary, `check_headers`, runs
first and prints the shared constants it is comparing across the two headers; only the
suites above report assertion counts.

`test_install` is the odd one out: it is Python rather than C, it drives `install.py`'s
own safety logic against simulated devices, and it needs no compiler, no cross toolchain,
no pyserial and no device. It can therefore be run on its own even when the C build is
broken:

```
make -C host test-install
    test_install: 540 checks, 0 failures
```

Without `build/bootloader.bin` it uses a synthetic bootloader image; with it, it
additionally pins that file's size and sha256.

You can also rehearse the whole install against a simulated device, which writes nothing
outside a temporary directory:

```
python3 install.py --dry-run
python3 install.py --list-sims       # every device --sim can simulate
```

```
make clean
make EXTRA_CFLAGS=-DBL_DRY_RUN all check
    RESULT: PASS — 65 checks, 0 failures, 2 warnings
```

65/0/2 is the dry-run build's correct result, not a regression — three image-level
call-site checks have nothing left to read once the flash wrappers are inlined away. The
`make clean` is required: `EXTRA_CFLAGS` is not part of the dependency graph, so without
it `make` reuses the shipping objects and you get 68/0/0 from a build that is not the one
you asked for. See [BUILDING.md](BUILDING.md).

---

## No vendor material is redistributed

`host/vendor/` — an unmodified script and a firmware image belonging to the GBFlash
creator — **has been removed**, and the test harness no longer depends on it.
`host/gen_streams.py` builds a synthetic image with `host/make_synthetic_fw.py` instead,
and the committed fixtures in `host/fixtures/` were recorded from that synthetic image, so
**the repository carries no vendor firmware and must not acquire any.** The `getserial`
replay is skipped with a printed note; the other three replays run in full.

---

## Attribution

- **[FlashGBX](https://github.com/lesserkuma/FlashGBX)** (lesserkuma) — the host
  software. Its source is the authority for the wire protocol, and its bundled `fw.bin`
  is the firmware this bootloader installs.
- **GBFlash hardware and original firmware** — simonkwng.
- **`gbflash_unlocker`** (v1b3myC0d3) — an independent third-party project whose README
  documented the boot-info record and the update protocol, and whose
  `gbflash_serial_update.py` drove three of the four verified updates.
- **[`wchisp`](https://github.com/ch32-rs/wchisp)** (ch32-rs) — the ISP utility
  `install.py` drives and offers to download, because it ships prebuilt binaries for every
  platform.
- **[`isp55e0`](https://github.com/frank-zago/isp55e0)** (Frank Zago) — a CH55x/CH57x ISP
  utility, the reference for anyone driving the ROM ISP by hand. Not driven by anything
  here.
- The protocol was additionally cross-checked against `getserial.py` from the original
  author's archived site.

None of the above endorse this project or are responsible for it.
