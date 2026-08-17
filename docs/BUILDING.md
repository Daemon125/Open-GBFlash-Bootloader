# Building

Everything here runs offline. Nothing in this document touches a device.

**You may not need to build at all.** A release publishes one zip holding the bootloader
and the Python tools, built from source by CI, with its sha256 on the release page.
Downloading that and skipping to [INSTALLING.md](INSTALLING.md) is a supported route — you
still composite it against your own backup locally, because no ready-made flashable image
can be published without redistributing someone else's firmware. This page is for building
it yourself, and for understanding what the gates assert.

---

## The expected build

```sh
make all
```

```
Memory region         Used Size  Region Size  %age Used
           FLASH:        7056 B      15872 B     44.46%
             RAM:        3420 B        32 KB     10.44%
  IMAGE       7056 bytes / 15872 budget (8816 free)
```

Both numbers are properties of the compiler, not of the source: a different
`arm-none-eabi-gcc` produces a different image, of a different size, and nothing in the
build or the test suite asserts either figure — only that the image fits the 15,872-byte
region, which is the linker script's job. With the exact toolchain named below they come
out as:

```
build/bootloader.bin   7056 bytes
sha256 4cf2a387bf62f1d64dfbbb145681fd0e107b0698d812e23686ce31b66d00d4cd
```

**That is a record of one build, not a constant to match.** A different `arm-none-eabi-gcc`
produces a different, equally valid image — which is why nothing in the build or the test
suite asserts it. If you are checking a *download* rather than your own build, the value
to compare against is the one printed on that release's page, not this one: release
binaries are built by CI, on a different toolchain from this.

---

## Toolchain

| | |
|---|---|
| Compiler | `arm-none-eabi-gcc` **15.3.Rel1** (Arm GNU Toolchain), Cortex-M0 |
| Binutils | the matching `arm-none-eabi-objcopy`, `objdump`, `size`, `nm`, `readelf` |
| Python | **3.8 or newer**, standard library only, for `make check` and `tools/build_composite.py`. The serial helpers in `docs/` also need 3.8 (they use `bytes.hex(sep)`) plus `pyserial` |
| C compiler for the host suites | any C99 compiler; the suites are built with `cc` |

Any recent `arm-none-eabi` GCC with Cortex-M0 support should build this, but 15.3.Rel1
is the toolchain every size, cycle count and hash in this repository was produced with.
Cycle counts quoted in the source comments were counted out of the emitted listing for
that toolchain; a different one may emit a different number of cycles for the same C.

Override the toolchain prefix with `CROSS=`, and the Python interpreter with `PYTHON=`:

```sh
make CROSS=arm-none-eabi- PYTHON=python3 all check
```

The build links `-nostdlib -nostartfiles`. There is no libc, no `crt0`, and no runtime
of any kind; `arm-none-eabi-nm -u build/bootloader.elf` printing nothing is a gate, not
a courtesy, and the link rule fails the build if it prints anything.

---

## Targets

| target | what it does |
|---|---|
| `make` / `make all` | build `build/bootloader.elf`, `.bin` and `.map`; fail if the `.bin` exceeds 15,872 bytes |
| `make size` | section sizes, headroom against the budget, and per-module `.text` |
| `make disasm` | `build/bootloader.lst` (full annotated disassembly) and `build/bootloader.vectors.txt` |
| `make check` | 68 offline assertions against the produced `.bin` and `.elf` (`tools/check_image.py`) |
| `make syms` | symbol table sorted by address, with sizes |
| `make host-test` | the host suites in `host/` (same as `make -C host test`) |
| `make -C host test-install` | the installer suite alone — Python only, no compiler, no device |
| `make dist` | stage the release zip into `build/dist/` |
| `make clean` | remove `build/` |

`make dist` builds `build/dist/gbflash-bootloader.zip` and nothing else — that one file is
the whole release. Inside it are `bootloader.bin`, `install.py`, `requirements.txt`, the
three standalone helper scripts, `LICENSE`, a `README.txt` and a `SHA256SUMS` covering
them. `install.py` finds its siblings in either that flat layout or a source checkout, so
unzipping it somewhere and running `python3 install.py --dry-run` rehearses the release
exactly as a downloader would get it:

```sh
make dist && (cd build/dist && unzip -q gbflash-bootloader.zip)
cd build/dist/gbflash-bootloader && python3 install.py --dry-run
```

`make dist` also stamps the staged `install.py` with the sha256 of the `bootloader.bin`
staged beside it, so a release vouches for its own binary whichever compiler built it.

Add `V=1` to echo every command.

### Build knobs

| knob | default | effect |
|---|---|---|
| `KEEP_UNUSED` | `1` | keep the tested public API in the image even where nothing calls it, so the reported size is the real footprint. `0` lets `--gc-sections` drop it. |
| `BL_USB_ECHO` | `0` | `1` restores a raw USB loopback for a pyserial round-trip diagnostic. **With echo on the device will not answer the update protocol** — the echo pump drains the receive staging before the framer sees it. |
| `EXTRA_CFLAGS` | empty | passed to every C compile. The intended use is `make EXTRA_CFLAGS=-DBL_DRY_RUN`, which builds a parse-and-ack-only image: every frame is parsed, CRC-checked and answered exactly as normal, and no flash operation is ever issued. |

The dry-run build passes as well, but with different numbers, and the difference is
expected rather than a defect:

```sh
make clean
make EXTRA_CFLAGS=-DBL_DRY_RUN all check
```

```
RESULT: PASS — 65 checks, 0 failures, 2 warnings
  [WARN] range_ok() was inlined away by -DBL_DRY_RUN, so its window cannot be read from the image
  [WARN] proto.c's flash wrappers were inlined away by -DBL_DRY_RUN, so their call sites cannot be read from the image
```

With the flash operations stubbed out, `fl_erase`, `fl_program`, `fl_read` and
`range_ok()` all become small enough for GCC to inline, and `arm-none-eabi-nm` confirms
none of the four survives the dry-run link (all four are present in the shipping build).
Three of the 68 checks are *image-level call-site* claims about exactly those symbols, so
they have nothing left to read and are reported as warnings instead of silently passing.
The checker will not take `-DBL_DRY_RUN` on trust: it applies the exemption only after
confirming the image really carries the dry-run fingerprint, so passing the flag to a
shipping build with the write guard removed cannot silence anything.

The dry-run variant is a first-class build, not a debug hack — but 65/0/2 is its correct
result, and 68/0/0 would mean something had changed.

> **The `make clean` is required, not tidiness.** `EXTRA_CFLAGS` is not part of the
> dependency graph, so with objects already present from a shipping build `make` finds
> nothing to do and `make check` then reports **68/0/0** — against the shipping binary,
> not the dry-run one. Getting the shipping numbers out of a command you thought was a
> dry-run build is the confusing failure here, so clean between the two in either
> direction.

---

## What `make check` asserts

`tools/check_image.py` reads the linked `.elf` and the emitted `.bin` and makes claims
about the *image*, not about the source. It disassembles, resolves symbols, walks
literal pools, and does a raw word-aligned scan of the binary. Ten sections, 68 checks,
and it exits non-zero on any failure.

**1. Size.** The image fits the 15,872-byte region and is at least one vector table
long.

**2. Vector table.** 36 entries / 0x90 bytes. Word 0 is `0x20008000`. Word 1 has the
Thumb bit set, points inside the bootloader region, and resolves to `Reset_Handler`.
Every entry 1..35 is non-zero and carries the Thumb bit — a cleared bit 0 in a vector
entry is an instant HardFault on every interrupt. Entry 2 points at `bl_nmi_handler`;
entries 3..35 all point at the shared dispatcher.

**3. Dispatcher encoding.** The trampoline is checked as *machine code*: its 36 bytes
are compared against the expected encoding, it is disassembled and compared instruction
by instruction, its two literals are confirmed to be `0x00004000` and
`R8_FLASH_PROTECT`, it is confirmed 4-byte aligned (PC-relative literal loads on
ARMv6-M compute `Align(PC,4) + imm`, so a 2-mod-4 entry would miss the pool), the NMI
prologue is confirmed to fall through into it, and it is confirmed to clobber only
`r0`/`r1` — `r4`-`r11` are not exception-stacked on ARMv6-M and must not be touched.

**4. No stray pointers into application space (`0x3E00..0xB51F`).** Every literal-pool
word that lands in that range must be a documented constant, and a raw word-aligned scan
of the whole `.bin` must find no un-whitelisted value in range. The range starts at the
boot-info record rather than at `0x4000`, and most of the whitelist is the record's own
field addresses (`0x3E02`, `0x3E06`, `0x3E08`, `0x3E0C`) as read by `bl_app_valid()` and
`feed_one()`. It ends at `0xB51F`, the end of the stock application image, not at the end
of CodeFlash.

**5. The CH579 user configuration word and InfoFlash.** No literal anywhere in
InfoFlash's range; no InfoFlash address anywhere in a raw scan; the user configuration
word address `0x00040010` appears nowhere at all; and `0x8C` — the `R8_FLASH_PROTECT`
value that would additionally enable `RB_ROM_DATA_WE` and make InfoFlash writable — is
never materialised in any function that can reach that register, and is not present as
a literal either. **This is the check that keeps H1 recovery working.**

**6. Write floor and self-protection.** `range_writable()` is present as its own symbol;
its window is pinned to exactly `[0x3E00, 0x3E800)` by constants read out of the emitted
code; every flash entry point calls it *before* touching the controller; and no
flash-driver literal names an address below `0x3E00`.

**7. Link hygiene.** No unresolved symbols. `.ARM.exidx` empty. The `.bss` zeroing loop
provably cannot reach the update magic at `0x20000090`. `.data` starts above the
reserved no-init hole. `__flash_image_end` matches the `.bin` length.

**8. Polled USB.** The USB layer is linked in and actually reached from update mode.
**`NVIC_ISER` (`0xE000E100`) is referenced by no literal pool and appears nowhere in a
raw scan** — no interrupt can ever be enabled. `NVIC_ICER` and `NVIC_ICPR` are present,
so IRQ6 is positively masked rather than merely not enabled. The device and
configuration descriptors are present verbatim and decode correctly (`1A86:7523`; one
interface, three endpoints, bulk `0x82` IN / `0x02` OUT), and the 26-byte canned CH340
vendor-IN table is present verbatim. Static RAM leaves at least 1 KB of stack.

**9. Protocol, flash driver and LED in update mode.** Everything here is a *call-site*
claim, not a symbol-table claim, because `KEEP_UNUSED=1` makes "the symbol exists" prove
nothing. Update mode drives the framer; `bl_proto_idle()` has a caller; the three flash
ops bound into `proto.c` are the real entry points, in order, with the Thumb bit set;
`range_ok()` and `bl_update_flash_read()` each have their window pinned to
`[0x3E00, 0x3E800)` with the `addr >= END` test still separate rather than folded back
into one subtraction; every `proto.c` flash wrapper calls `range_ok()` before reaching
the ops table; the LED is driven from update mode **and from nowhere else**, so the
normal boot path never touches PB12; and `SCB_AIRCR` with the `0x05FA0004` reset key
appears only inside the handover function.

**10. Time base.** SysTick is in use (CSR, RVR and CVR all referenced), **`TICKINT` is
never set** so SysTick raises no exception, `NVIC_ISER` is still absent, and
`bl_jump_to_app()` still clears all three SysTick registers so the application receives
the whole peripheral in its reset state.

---

## The host suites

```sh
make -C host test
```

No hardware. Five shipping modules are compiled natively and are themselves
the code under test — `src/proto.c` straight from `src/`, and `flash.c`, `usb.c`,
`timebase.c` and `boot.c` through bounded redirections that replace MMIO with a model.

| suite | covers |
|---|---|
| `test_timebase` | the SysTick millisecond accumulator, including wrap |
| `test_flash` | the real `src/flash.c` against a register-level model of the CH579 flash controller |
| `test_usb` | the real `src/usb.c` against a model of the SIE |
| `test_proto` | the framer and the three handlers, byte-for-byte against real updater output |
| `fuzz_proto` | 30,000 iterations of random and semi-structured input into the framer |
| `rehearse_update` | a complete offline end-to-end update: `proto.c` -> `flash.c` -> flash-controller model -> `boot.c`'s `bl_app_valid()` |
| `test_install` | `install.py`'s safety logic, against simulated devices in thirteen states |

`test_install` is the odd one out and is described [below](#the-installer-suite).

`rehearse_update` is the one worth understanding. It runs three consecutive full updates
with two real firmware images (L14 -> L15 -> L14), through the real protocol code and
the real flash driver, and after each one asserts that the resulting flash array is
byte-identical to the source file and that `bl_app_valid()` accepts it and would hand
off to the right MSP and PC. That is what makes an offline pass meaningful.

### The installer suite

```sh
make -C host test-install
```

`host/test_install.py` is Python rather than C, and it tests
[`install.py`](../install.py) rather than the firmware. It imports the installer as a
module and drives its real code paths against a simulated device, so the safety logic
that runs is the logic that would run on a bench.

It needs **no compiler, no cross toolchain, no pyserial and no device**, which makes it
the one suite that still runs when the C build is broken. Without `build/bootloader.bin`
it substitutes a synthetic bootloader image; with it, it additionally pins that file's
size and sha256.

The thirteen simulated device states are the ones the installer has to refuse or handle: a
device that needs the bootloader, one that already has one, a region erased to `0xFF`
rather than zeroed, nothing connected, a device already in ROM ISP mode, flash that reads
back corrupt, a boot region that reads back garbled while the application half is perfect,
no ISP tool installed, a corrupt `bootloader.bin`, `4348:55E0` never
appearing after the jumper step, an ISP write that fails, two devices attached at once,
and a dead board with no serial port at all.
`--check`, `--backup` and `--restore` are covered separately, including a restore
against a device with nothing in flash and no serial port.

Three assertions in it are worth knowing about, because they are what keep the docs and
the installer honest:

- it asserts **where** a refused run stops, not merely that it stopped — an earlier
  version of the suite could not tell a removed "no backup, no install" rule from a
  working one, because the composite gate caught the same image one step later;
- it audits **every ISP command the run issued** and requires each to be a plain
  `wchisp flash FILE` and nothing else, which is how "the user configuration word is never
  written" is enforced rather than asserted;
- it proves `--restore` is independent of pyserial by putting a `serial` module on
  `PYTHONPATH` that raises on import, and requiring the restore to complete anyway.

You can also drive the installer directly against a simulated device:

```sh
python3 install.py --dry-run              # the normal install, start to finish
python3 install.py --list-sims            # every device --sim can simulate
python3 install.py --dry-run --sim installed   # one that must refuse
```

Nothing outside a temporary directory is written, and nothing physical is touched.

### What a fresh clone can and cannot run

**Everything except `rehearse_update`, with nothing supplied.**

This repository ships **no vendor firmware and must not acquire any.** What it does ship
is a generator: `host/make_synthetic_fw.py` builds a structurally valid `fw.bin` — real
boot-info record, real CRCs, correct layout — and `host/gen_streams.py` uses it by default.
The committed fixtures in `host/fixtures/` were recorded by driving the three real host
implementations against that synthetic image, and `fixtures/image.bin` is the image
itself, committed alongside them so the fixture set is self-describing.

The consequence is that `test_proto` needs nothing external: it replays the recorded
response streams *and* checks the reconstructed flash array against `fixtures/image.bin`
*and* runs `bl_proto_selftest_image()`. The protocol layer never inspects payload meaning
— only framing, CRCs, sequencing and lengths — so a synthetic image exercises the same
paths a vendor one would.

| | plain checkout | with `FW_L14=`/`FW_L15=` supplied |
|---|---|---|
| `test_proto` replays | full: response streams byte for byte, reconstructed image, finalize CRC | unchanged |
| `test_proto` image self-test | full, against `fixtures/image.bin` | unchanged |
| `rehearse_update` | **skipped**, with a printed note naming the paths it looked for | three full end-to-end updates, 80 checks |
| everything else | runs in full | runs in full |

So `make -C host test` exits 0 in a plain checkout, with one suite skipped and one line
saying so. To run that one too, point it at two images you supply yourself:

```sh
make -C host test FW_L14=/path/to/L14/fw.bin FW_L15=/path/to/L15/fw.bin
```

`FW_L14` and `FW_L15` default to paths *outside* this tree. If files happen to exist
there, the rehearsal runs against them silently — so read the paths it prints rather than
assuming which images the numbers came from.

`GBFLASH_FW=/path/to/fw.bin` points `test_proto`'s image checks at a real image instead of
`fixtures/image.bin`. It is only meaningful together with re-recorded fixtures, since the
recorded streams were produced from the synthetic image; on its own it does not add a
check that is currently skipped.

The recorded fixtures are **committed**, and `test` replays them as-is — it does not
regenerate them, so the suites need neither Python nor any host implementation at run
time. `make -C host clean` therefore removes only `build/` and leaves them alone.

Other targets: `make -C host fixtures` **re-records** the streams by driving the three
real host implementations, which needs FlashGBX and `gbflash_serial_update.py` (it
generates the synthetic image itself, and honours `FW_BIN=` to use another);
`make -C host clean-fixtures` removes the recordings first; `make -C host rebaseline`
re-pins the device-response baseline after a *deliberate* change to what the device
answers.

A fourth replay, `getserial`, is referenced by the harness and skips itself with a note.
The material it needed was third-party and has been removed from this repository; the
other three replays are unaffected.

The default build uses `-fsanitize=undefined,local-bounds` plus guard pages — the
`bl_proto` object and the fuzzer's input buffer end flush against a `PROT_NONE` page, so
one byte past either is a `SIGSEGV`. `SAN=asan` switches to AddressSanitizer on a
machine where ASan is healthy.

---

## The composite install image

### Why a composite is needed

The CH579's ROM ISP writes CodeFlash as a whole: `wchisp flash`, and every equivalent,
erases the array and writes from address `0`. There is no "write these sectors only" mode. So installing the bootloader over ISP means writing a full image
that contains **both** the bootloader and an application — otherwise the erase takes the
application with it and the device comes back with a bootloader and nothing to boot.

`tools/build_composite.py` builds that image:

```
0x0000 .. len(bl)-1   the bootloader binary
len(bl) .. 0x3DFF     0xFF fill (erased state; programming 0xFF is a no-op)
0x3E00 ..             the application image, boot-info record included
```

The application region is taken **verbatim**. Nothing in it is recomputed, re-CRC'd or
re-padded. It is copied byte for byte and then checked.

```sh
python3 tools/build_composite.py \
    --bootloader build/bootloader.bin \
    --app        /path/to/current-device-dump.bin \
    --compare    /path/to/vendor/fw.bin \
    --baseline   /path/to/current-device-dump.bin \
    --out        /path/to/composite.bin
```

`--backup DUMP` is shorthand for `--app DUMP --baseline DUMP`, which is the normal install
and the form a release download uses. `--app` accepts either a bare `fw.bin` or a full
CodeFlash dump (in which case its `0x3E00..` slice is used, and an erased tail past the
end of the application — a `--all` backup — is trimmed); the same applies to `--compare`
and `--baseline`. With no `--out` it verifies and writes nothing.

> **What has a default, and what happens when an input is absent.** Only `--bootloader`
> has one: it looks for `bootloader.bin` next to the script (a flat release download),
> then in `build/` (a checkout), then in the working directory. Nothing outside the tree
> is ever referenced. Every other named file **must exist** — a `--compare` or
> `--baseline` path that is not there is a hard error naming the path, exit 2, no output
> file. An input you simply omit is reported as `[SKIP]`, named again in the `RESULT:`
> line, and counted, so a weaker verification is never mistakable for the full one; with
> no baseline the tool also prints *"Nothing has checked this image against the flash now
> on your device."* And `--compare` aimed at the file `--app` already names is a `[SKIP]`,
> not a `[PASS]` — a file compared with itself is not a second copy.

It prints **26 checks** with every input supplied, and a `RESULT:` line. Scope any count
you quote to the invocation: 26 with everything, **25 plus one SKIP** for
`--backup` alone (the release flow), 22 plus four SKIPs for a bare `--app` with no
baseline. All three are `PASS` results; only the wording of the `RESULT:` line differs.
The checks that matter:

- the vector table in the bootloader half is well formed and every entry points inside
  the bootloader image;
- the application half passes the gates `bl_app_valid()` will apply on-chip, plus two
  deliberately stricter ones (the boot-info marker, and an all-`0xFF` header-page tail)
  that are labelled as stricter because they can only refuse an image the device would
  in fact accept;
- `0x0000` is **not** a copy of the application's vector table, and the reset vector at
  `0x0004` is the **bootloader's**;
- **the composite differs from the current device image only below `0x3E00`.** That
  single binary property is what makes this a bootloader install rather than a firmware
  change.

On that last one, note what the tool prints. When `--app` and `--baseline` are the same
file — the normal install flow, where the application half is taken from your own device
dump — the check cannot fail, and the tool appends *"(note: same file as --app, so this
is by construction)"* rather than presenting a tautology as evidence. The property still
holds; it is guaranteed by the copy rather than measured. It becomes a real comparison
only when `--app` is sourced separately (a fresh vendor `fw.bin`, say) with your device
dump still in `--baseline`, which is a deliberate firmware change as well as a bootloader
install. [INSTALLING.md](INSTALLING.md) walks through the normal case.

### Do not use the general-purpose full-image tools

`gbflash_update.py` and similar full-image builders synthesise `0x0000` by copying the
application's first `0xB8` payload bytes — the application's own vector table — because
a bootloader-less GBFlash has no other way to get vectors to address `0`. That is
correct for a device with no bootloader and **wrong the moment one exists**: it
overwrites the bootloader's vector table with the application's, so the reset vector
points straight at the application and the bootloader never runs again. Those tools have
no flag that suppresses it. `tools/build_composite.py` exists to replace that step; it
never invents vectors.

### FlashGBX's updater must not be used to install the bootloader

FlashGBX's Firmware Updater speaks the bootloader protocol and writes from `0x3E00`
upward. It cannot write below that, by design and by this bootloader's write floor. It
installs *firmware*. It cannot install the bootloader itself, and pointing it at a
composite image will either fail its own checks or write the composite's first bytes
into `0x3E00` as though they were firmware. The bootloader goes in over ISP, once. See
[INSTALLING.md](INSTALLING.md).
