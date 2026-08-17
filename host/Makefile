# host/Makefile — native test harness for the GBFlash bootloader protocol layer.
#
#   make -C host test     build everything, replay the recorded updater streams,
#                         run the suites, the fuzzer and the install.py suite.
#                         Nonzero exit on any failure. Works in a plain
#                         checkout: the fixtures are committed and are NOT
#                         regenerated here.
#   make -C host test-install
#                         run test_install.py alone — the offline suite for the
#                         guided installer, ../install.py. Needs no compiler,
#                         no cross toolchain, no pyserial and no device.
#   make -C host test FW_L14=/path/to/L14/fw.bin FW_L15=/path/to/L15/fw.bin
#                         additionally run the end-to-end rehearsal, which
#                         needs two stock vendor images this tree does not
#                         redistribute. Without them that one suite is skipped
#                         with a note and the other six still run.
#   make -C host fixtures RE-RECORD fixtures/ by driving the three real host
#                         implementations. Needs FlashGBX and
#                         gbflash_serial_update.py, neither of which is here.
#                         It generates the synthetic image it drives them with;
#                         set FW_BIN= to use a real fw.bin instead.
#   make -C host rebaseline
#                         re-pin fixtures/d2h-baseline.json after a DELIBERATE
#                         change to what the device answers. See the note above
#                         that target before using it.
#   make -C host clean    remove build/ only; the fixtures and the pinned
#                         baseline survive.
#   make -C host clean-fixtures
#                         remove the recorded streams too, for re-recording.
#
#   make -C host test SAN=asan     use AddressSanitizer instead of the guard
#                                  pages. See the note below before you do.
#   make -C host test FUZZ_ITERS=200000
#
# Nothing here touches hardware. Five of the shipping modules are compiled
# natively and are themselves the code under test — proto.c straight from src/,
# and flash.c, usb.c, timebase.c and boot.c through the bounded redirections
# described at each target below. flash_stub.c is a stand-in for the flash
# SERVICE, used only by the proto.c suite and the ctypes library so that
# proto.c's own guards are the thing being exercised; the flash driver's suite
# and the end-to-end rehearsal run the real src/flash.c instead.
#
# SANITIZERS — read this.
#   AddressSanitizer does NOT work on this machine. On macOS 27.0 (26A5406e)
#   with Apple clang 17.0.0, any -fsanitize=address binary hangs at startup:
#       ==N==AddressSanitizer: libc interceptors initialized
#       ==N==FindDynamicShadowStart, space_size = 0x0fffffffffff
#   and then spins at 100% CPU forever. Reproduced with a three-line
#   hello-world, so it is the toolchain, not this code.
#
#   The default build therefore uses:
#     -fsanitize=undefined,local-bounds  — UBSan works normally here, and
#       array-bounds catches every out-of-range index into the fixed-size
#       arrays inside bl_proto, which is precisely the overflow class the
#       framer could have. Verified to fire on a deliberate struct-member
#       overrun.
#     guard pages (guard.c) — the bl_proto object and the fuzzer's input
#       buffer end flush against a PROT_NONE page, so one byte past either is
#       a SIGSEGV.
#   `SAN=asan` restores -fsanitize=address,undefined for a machine where ASan
#   is healthy; nothing else changes.

CC        ?= cc
PYTHON    ?= python3

SRCDIR    := ../src
INCDIR    := ../include

CPPFLAGS  := -I$(INCDIR) -I. -DBL_HOST
WARN      := -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
             -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
CFLAGS    := -std=c99 -g -O1 $(WARN)

SAN       ?= guard
ifeq ($(SAN),asan)
SANFLAGS  := -fsanitize=address,undefined -fno-omit-frame-pointer \
             -fno-sanitize-recover=all
else
SANFLAGS  := -fsanitize=undefined,local-bounds -fno-omit-frame-pointer \
             -fno-sanitize-recover=all
endif

BUILD     := build
FIXTURES  := fixtures

DUT       := $(SRCDIR)/proto.c flash_stub.c dev_api.c guard.c
HDRS      := $(INCDIR)/proto.h flash_stub.h dev_api.h guard.h

LIB       := $(BUILD)/libgbfdev.dylib
TESTBIN   := $(BUILD)/test_proto
FUZZBIN   := $(BUILD)/fuzz_proto
HDRCHK    := $(BUILD)/check_headers

# ---- src/flash.c under native test ---------------------------------------
# flash.c is the module that actually erases and programs silicon, so it is
# tested against a register-level model of the CH579 flash controller
# (flash_model.c) rather than replaced by a stub. It cannot be compiled here
# as-is: it hard-codes MMIO at 0x40001800 and dereferences flash addresses
# directly (arm64 macOS has the whole low 4 GB as __PAGEZERO, so neither can
# be mapped), it contains ARMv6-M inline assembly that does not assemble for
# any host, and it requires its source buffer to live in 0x20000000..0x20008000.
#
# So the host build GENERATES build/flash_host.c from src/flash.c with the
# fixed sed transform below. src/flash.c itself is never edited and carries no
# host-only #ifdef. The transform is bounded and checked twice:
#   * it must change EXACTLY $(FLASH_XFORM_LINES) lines and no others, and must
#     not change the line count (nothing is inserted; flash_model.h arrives via
#     -include);
#   * the PREPROCESSED result must contain no `volatile`, no `__asm` and no
#     0x400018 literal, i.e. no path back to real MMIO survives.
# Everything the tests exercise — every guard, every ordering, every status
# check — is byte-identical to the shipping source. See flash_model.h.
FLASHSRC  := $(SRCDIR)/flash.c
FLASHGEN  := $(BUILD)/flash_host.c
FLASHPP   := $(BUILD)/flash_host.i
FLASHOBJ  := $(BUILD)/flash_host.o
TESTFLASH := $(BUILD)/test_flash

FLASH_XFORM_LINES := 15

# ---- src/usb.c under native test -----------------------------------------
# usb_model.c #includes ../src/usb.c verbatim after redefining BL_REG8/16/32 to
# model accessors, so the translation unit under test is the shipping file byte
# for byte — no sed transform, no host-only #ifdef. It is built with
# BL_USB_ECHO=0 (the stage-4 configuration; the echo pump would drain the
# receive staging the framer needs) and BL_USB_PLL_POWER_ON=0 (Thumb inline
# assembly for the safe-access window, which cannot assemble natively).
# src/usb.c is named as a prerequisite even though it is not on the command
# line: it arrives through the #include, and without it here a change to usb.c
# would not rebuild the suite that tests it.
TESTUSB   := $(BUILD)/test_usb

# ---- src/timebase.c under native test ------------------------------------
# Same construction as usb.c: timebase_model.c #includes ../src/timebase.c
# verbatim after redefining BL_REG32 to a model of SysTick, so the translation
# unit under test is the shipping file byte for byte — no sed transform, no
# host-only #ifdef. The model's clock advances ONLY when the test says so, so a
# 4.6 ms flash blackout, a full 524.288 ms lap of the 24-bit counter and a
# single cycle are all exactly reproducible.
#
# src/timebase.c is named as a prerequisite even though it is not on the
# command line: it arrives through the #include, and without it here a change
# to timebase.c would not rebuild the suite that tests it.
TESTTIME  := $(BUILD)/test_timebase

# ---- the end-to-end update rehearsal -------------------------------------
# Drives complete stock fw.bin updates through src/proto.c into the CH579 flash
# controller model (flash_model.c + the same generated build/flash_host.c the
# flash suite uses), i.e. through the REAL driver rather than flash_stub.c.
# This is the offline dress rehearsal for the hardware update; see
# rehearse_update.c.
REHEARSE  := $(BUILD)/rehearse_update

FUZZ_ITERS ?= 30000

# fixtures/manifest.json is the stamp: gen_streams.py rewrites it on success.
STAMP     := $(FIXTURES)/manifest.json

# ---- install.py under test -----------------------------------------------
# test_install.py drives the REAL logic in ../install.py against simulated
# devices -- application mode, ISP mode, absent, a device that already has a
# bootloader, a backup that does not verify, a jumper that never takes, an ISP
# write that fails, an ISP write that lands wrong -- and asserts the guided
# installer refuses what it must refuse. install.py keeps everything that
# touches the outside world behind one object, and only that object is
# replaced; every gate and every branch above it is the shipping code.
#
# It needs neither a compiler, a cross toolchain, pyserial nor a device: it
# uses a synthetic bootloader image where ../build/bootloader.bin is absent,
# and separately pins the real binary's SHA-256 when it is there. It does shell
# out to ../tools/build_composite.py, deliberately -- the installer's
# independent cross-check is one of the things under test.
TESTINSTALL := test_install.py

.PHONY: all test test-install fixtures rebaseline clean clean-fixtures

all: $(LIB) $(TESTBIN) $(FUZZBIN) $(HDRCHK) $(TESTFLASH) $(FLASHPP) \
     $(TESTUSB) $(TESTTIME) $(REHEARSE)

$(BUILD):
	@mkdir -p $(BUILD)

# The ctypes library is built WITHOUT sanitizers: a sanitized dylib cannot be
# dlopen'd into a stock python3 without preloading the runtime first.
$(LIB): $(DUT) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -shared -o $@ $(DUT)

$(TESTBIN): test_proto.c $(DUT) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $(CPPFLAGS) -o $@ test_proto.c $(DUT)

$(FUZZBIN): fuzz_proto.c $(DUT) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $(CPPFLAGS) -o $@ fuzz_proto.c $(DUT)

# Reports where include/proto.h, include/boot.h and include/bl_config.h
# disagree. It now EXITS NON-ZERO on a disagreement — the three headers agree
# today, and BL_APP_MAX_LEN diverging again is an update-breaking defect, not a
# matter of taste. bl_config.h is listed as a prerequisite because this is the
# only translation unit in the tree that compiles it at all.
$(HDRCHK): check_headers.c $(INCDIR)/proto.h $(INCDIR)/boot.h $(INCDIR)/bl_config.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ check_headers.c

# --- src/flash.c -> build/flash_host.c ------------------------------------
# Each -e below is anchored on a whole statement, not on a token, so a new
# MMIO access of an unfamiliar shape is NOT silently transformed: it survives
# into the preprocessed output and trips the $(FLASHPP) check.
$(FLASHGEN): $(FLASHSRC) | $(BUILD)
	@sed \
	  -e 's/0x20000000u/fm_sram_lo()/' \
	  -e 's/0x20008000u/fm_sram_hi()/' \
	  -e 's/__asm volatile ("mrs %0, primask" : "=r" (primask));/primask = fm_primask();/' \
	  -e 's/__asm volatile ("cpsid i" ::: "memory");/fm_cpsid();/' \
	  -e 's/__asm volatile ("cpsie i" ::: "memory");/fm_cpsie();/' \
	  -e 's/R8_FLASH_PROTECT *= \(.*\);/fm_w8(FM_PROTECT, (uint8_t)(\1));/' \
	  -e 's/R8_FLASH_COMMAND *= \(.*\);/fm_w8(FM_CMD, (uint8_t)(\1));/' \
	  -e 's/R32_FLASH_ADDR *= \(.*\);/fm_w32(FM_ADDR, (uint32_t)(\1));/' \
	  -e 's/R32_FLASH_DATA *= \(.*\);/fm_w32(FM_DATA, (uint32_t)(\1));/' \
	  -e 's/(uint32_t)R16_FLASH_STATUS/(uint32_t)fm_r16(FM_STATUS)/' \
	  -e 's/\*(volatile const uint32_t \*)(\([^;)]*\))/fm_rd32(\1)/g' \
	  -e 's/\*(volatile const uint8_t \*)(\([^;)]*\))/fm_rd8(\1)/g' \
	  -e 's/\*(volatile const uint32_t \*)\([A-Za-z_][A-Za-z0-9_]*\)/fm_rd32(\1)/g' \
	  $(FLASHSRC) > $@.tmp
	@n=`awk 'NR==FNR{a[FNR]=$$0;next} a[FNR]!=$$0{c++} END{print c+0}' \
	        $(FLASHSRC) $@.tmp`; \
	 lo=`wc -l < $(FLASHSRC)`; lg=`wc -l < $@.tmp`; \
	 if [ "$$n" != "$(FLASH_XFORM_LINES)" ] || [ "$$lo" != "$$lg" ]; then \
	   echo "ERROR: the src/flash.c host transform redirected $$n lines"; \
	   echo "       (expected $(FLASH_XFORM_LINES); line count $$lo -> $$lg)."; \
	   echo "       src/flash.c changed shape. Re-read the transform in the"; \
	   echo "       host Makefile and flash_model.h BEFORE adjusting the count:"; \
	   diff $(FLASHSRC) $@.tmp || true; \
	   rm -f $@.tmp; exit 1; \
	 fi
	@mv $@.tmp $@
	@echo "  XFORM $(FLASHSRC) -> $@ ($(FLASH_XFORM_LINES) lines redirected)"

# The transform is only as good as this check: preprocess the generated file,
# keep only the lines that came from it, and fail if any MMIO, inline assembly
# or raw-address dereference survived.
$(FLASHPP): $(FLASHGEN) flash_model.h $(INCDIR)/flash.h | $(BUILD)
	@$(CC) $(CPPFLAGS) -include flash_model.h -E $(FLASHGEN) \
	  | awk '/^#[ \t]*[0-9]+[ \t]+"/ { keep = ($$0 ~ /flash_host\.c"/); next } keep' \
	  > $@
	@if grep -n 'volatile\|__asm\|0x400018' $@; then \
	   echo "ERROR: MMIO, inline asm or a raw-address dereference survived the"; \
	   echo "       src/flash.c host transform (lines above). Refusing to build."; \
	   exit 1; \
	 fi
	@echo "  CHECK $@: no MMIO, no inline asm, no raw-address dereference"

# -Wno-pointer-to-int-cast is for `(uint32_t)src` in bl_flash_program(): on the
# 32-bit target that cast is exact, on a 64-bit host it truncates. The model's
# SRAM window is a host buffer chosen so that the truncated comparison is still
# meaningful (flash_model.c checks it does not straddle a 4 GB boundary).
$(FLASHOBJ): $(FLASHGEN) $(FLASHPP) flash_model.h $(INCDIR)/flash.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $(CPPFLAGS) -Wno-pointer-to-int-cast \
	    -include flash_model.h -c -o $@ $(FLASHGEN)

$(TESTFLASH): test_flash.c flash_model.c flash_model.h $(FLASHOBJ) \
              $(INCDIR)/flash.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $(CPPFLAGS) -o $@ \
	    test_flash.c flash_model.c $(FLASHOBJ)

# --- src/usb.c under native test ------------------------------------------
# No transform: usb_model.c #includes ../src/usb.c after redefining the
# BL_REG8/16/32 accessors, so the file under test is the shipping one byte for
# byte. src/usb.c is a prerequisite even though it is not on the command line —
# it arrives through that #include, and without it here a change to usb.c would
# not rebuild its own test.
$(TESTUSB): test_usb.c usb_model.c usb_model.h $(SRCDIR)/usb.c \
            $(SRCDIR)/usb_desc.c $(INCDIR)/usb.h $(INCDIR)/bl_config.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $(CPPFLAGS) -o $@ \
	    test_usb.c usb_model.c $(SRCDIR)/usb_desc.c

# --- src/timebase.c under native test -------------------------------------
# See the TESTTIME note above. src/timebase.c arrives through timebase_model.c's
# #include, so it is a prerequisite but not a command-line source.
$(TESTTIME): test_timebase.c timebase_model.c timebase_model.h \
             $(SRCDIR)/timebase.c $(INCDIR)/timebase.h $(INCDIR)/bl_config.h \
             | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $(CPPFLAGS) -o $@ \
	    test_timebase.c timebase_model.c

# --- src/boot.c -> build/boot_host.c --------------------------------------
# Only bl_app_valid() and bl_jump_to_app() are wanted here, and only so the
# rehearsal can ask the REAL boot decision whether the image it just wrote is
# bootable. Six address-forming expressions are redirected to the flash model;
# every gate, constant and early return is untouched. Full rationale, and the
# list of what is deliberately left un-redirected, in host/boot_model.h.
BOOTSRC   := $(SRCDIR)/boot.c
BOOTGEN   := $(BUILD)/boot_host.c
BOOTOBJ   := $(BUILD)/boot_host.o

BOOT_XFORM_LINES := 6

$(BOOTGEN): $(BOOTSRC) | $(BUILD)
	@sed \
	  -e 's/(const uint8_t \*)(uintptr_t)BL_BOOTINFO_BASE/bh_p(BL_BOOTINFO_BASE)/' \
	  -e 's/(const uint8_t \*)(uintptr_t)BL_APP_BASE/bh_p(BL_APP_BASE)/' \
	  -e 's/REG32(BL_APP_BASE + 4u)/bh_r32(BL_APP_BASE + 4u)/' \
	  -e 's/REG32(BL_APP_BASE)/bh_r32(BL_APP_BASE)/' \
	  $(BOOTSRC) > $@.tmp
	@n=`awk 'NR==FNR{a[FNR]=$$0;next} a[FNR]!=$$0{c++} END{print c+0}' \
	        $(BOOTSRC) $@.tmp`; \
	 lo=`wc -l < $(BOOTSRC)`; lg=`wc -l < $@.tmp`; \
	 if [ "$$n" != "$(BOOT_XFORM_LINES)" ] || [ "$$lo" != "$$lg" ]; then \
	   echo "ERROR: the src/boot.c host transform redirected $$n lines"; \
	   echo "       (expected $(BOOT_XFORM_LINES); line count $$lo -> $$lg)."; \
	   echo "       Re-read the transform above and host/boot_model.h BEFORE"; \
	   echo "       adjusting the count — the point of this gate is that a new"; \
	   echo "       absolute access in bl_app_valid() cannot slip through"; \
	   echo "       untested:"; \
	   diff $(BOOTSRC) $@.tmp || true; \
	   rm -f $@.tmp; exit 1; \
	 fi
	@mv $@.tmp $@
	@echo "  XFORM $(BOOTSRC) -> $@ ($(BOOT_XFORM_LINES) lines redirected)"

# -DBL_HOST_TEST drops the includes of proto.h/flash.h/usb.h/led.h, the whole
# bl_update_mode() body and the MMIO half of bl_jump_to_app().
$(BOOTOBJ): $(BOOTGEN) boot_model.h $(INCDIR)/boot.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $(CPPFLAGS) -DBL_HOST_TEST \
	    -include boot_model.h -c -o $@ $(BOOTGEN)

# --- the end-to-end update rehearsal --------------------------------------
# proto.c is compiled straight from src/ (it is portable by design); flash.c
# arrives as the generated $(FLASHOBJ) over flash_model.c; boot.c as
# $(BOOTOBJ). flash_stub.c is deliberately NOT linked — the point is to run the
# real driver, not the service stub proto.c's own suite uses.
FW_L14    ?= ../../fw_L14/fw.bin
FW_L15    ?= ../../fw/fw.bin

$(REHEARSE): rehearse_update.c $(SRCDIR)/proto.c flash_model.c flash_model.h \
             boot_model.h $(FLASHOBJ) $(BOOTOBJ) $(INCDIR)/proto.h \
             $(INCDIR)/boot.h $(INCDIR)/flash.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $(CPPFLAGS) -o $@ \
	    rehearse_update.c $(SRCDIR)/proto.c flash_model.c \
	    $(FLASHOBJ) $(BOOTOBJ)

# REGENERATING THE FIXTURES IS NOT PART OF `test`, DELIBERATELY.
#
# fixtures/*.h2d.bin, *.d2h.bin, *.meta.txt and manifest.json are COMMITTED,
# and `test` replays them exactly as they are. That is the whole point of
# recording them: the C suites need neither Python nor any vendor code at run
# time, so `make test` works in a plain checkout on a machine that has never
# seen FlashGBX.
#
# gen_streams.py, by contrast, drives the three REAL host implementations, and
# they are not this project's to redistribute. It needs, outside this tree:
#   * FlashGBX's hw_GBFlash.py,
#   * the third-party gbflash_serial_update.py.
# The image it drives them with is generated (host/make_synthetic_fw.py); set
# FW_BIN= to point it at a real fw.bin instead. Making `test` depend on any of
# this would mean a fresh clone fails before running a single assertion, which
# is exactly the bug this arrangement avoids. Run `make fixtures` explicitly
# when you have those two and want to re-record.
fixtures:
	$(PYTHON) gen_streams.py

$(STAMP):
	@echo 'error: $(STAMP) is missing.'; \
	 echo '  The fixtures are committed, and "make test" replays them as-is.'; \
	 echo '  If you removed them, restore them from version control, or'; \
	 echo '  re-record with "make -C host fixtures" -- which needs FlashGBX'; \
	 echo '  and gbflash_serial_update.py.'; \
	 exit 1

# The *.d2h.bin fixtures are recorded FROM proto.c, so replaying them against
# proto.c cannot detect a changed response — the fixture was re-recorded from
# the change. fixtures/d2h-baseline.json pins the SHA-256 of each response
# stream and survives `clean`, so a change in what the device says has to be
# acknowledged here rather than silently re-baselined.
rebaseline: $(LIB)
	GBFLASH_REBASELINE=1 $(PYTHON) gen_streams.py

export ASAN_OPTIONS  = abort_on_error=1:halt_on_error=1
export UBSAN_OPTIONS = print_stacktrace=1:halt_on_error=1

# Order is cheapest-and-most-foundational first: headers, then the time base
# (every timeout in the other suites is a difference of bl_time_ms(), so a
# broken accumulator is the least useful thing to discover last), then the
# flash driver, then USB, then the protocol layer, then the fuzzer, and finally
# the end-to-end rehearsal — which is the only one that needs all of them to be
# right at once, so it is the least useful failure to read first.
test: $(STAMP) $(TESTBIN) $(FUZZBIN) $(HDRCHK) $(TESTFLASH) $(TESTUSB) \
      $(TESTTIME) $(REHEARSE)
	./$(HDRCHK)
	./$(TESTTIME)
	./$(TESTFLASH)
	./$(TESTUSB)
	./$(TESTBIN)
	./$(FUZZBIN) $(FUZZ_ITERS)
	@# The rehearsal replays two REAL stock vendor firmware images, which are
	@# not this project's to redistribute. With them it is the strongest suite
	@# here; without them it is skipped loudly rather than failing, so a fresh
	@# clone still gets the other six. Supply them with
	@#   make -C host test FW_L14=/path/to/L14/fw.bin FW_L15=/path/to/L15/fw.bin
	@if [ -f "$(FW_L14)" ] && [ -f "$(FW_L15)" ]; then \
	    ./$(REHEARSE) $(FW_L14) $(FW_L15); \
	 else \
	    echo "rehearse_update: SKIPPED — needs two stock vendor fw.bin images."; \
	    echo "  looked for FW_L14=$(FW_L14)"; \
	    echo "         and FW_L15=$(FW_L15)"; \
	    echo "  Supply them with: make -C host test FW_L14=... FW_L15=..."; \
	    echo "  They are not redistributed here. Every other suite still runs."; \
	 fi
	@# Last, because it is the outermost layer: the host-side guided
	@# installer rather than anything that runs on the device. It has no
	@# build prerequisites at all, so it is also the one suite that still
	@# runs when the C build is broken.
	$(MAKE) test-install

# Runnable on its own — it needs nothing built.
test-install:
	$(PYTHON) $(TESTINSTALL)

# NEITHER the fixtures NOR the pinned baseline are removed by `clean`.
#
# fixtures/*.bin and *.meta.txt are committed inputs that `test` replays, and
# re-recording them needs vendor material this tree does not carry — so a
# `clean` that deleted them would leave the suite unrunnable in a fresh clone
# until someone found a stock fw.bin. fixtures/d2h-baseline.json is the pinned
# record of what the device used to answer, and regenerating it as a side
# effect of `clean` would defeat its whole purpose.
clean:
	rm -rf $(BUILD)

# Deliberate removal of the recorded streams, for when you are about to
# re-record them with `make fixtures`. The pinned baseline still survives.
clean-fixtures:
	rm -f $(FIXTURES)/*.bin $(FIXTURES)/*.meta.txt $(STAMP)
