# Makefile — GBFlash CH579M update-mode bootloader, stages 0 and 1
#
# OWNED BY: comp:integrate
#
#   make            build build/bootloader.elf, .bin, .map
#   make size       section sizes + headroom against the 15,872 B budget
#   make disasm     full annotated disassembly -> build/bootloader.lst
#   make check      offline validation of the produced .bin (see tools/check_image.py)
#   make clean      remove build/
#   make host-test  run the native protocol harness in host/ (proto.c only)
#   make dist       stage the release zip into build/dist/
#
# To INSTALL the result on a device, use the guided installer at the tree root:
#   python3 install.py             the whole procedure, with every precondition
#                                  enforced rather than described
#   python3 install.py --dry-run   rehearse it against a simulated device
#   python3 install.py --restore B put a device back from a backup
# docs/INSTALLING.md is the same procedure by hand, with the reasoning.
#
# Knobs:
#   KEEP_UNUSED=1   (default) retain proto.c and flash.c in the image even though
#                   nothing calls them yet, so the reported size is the real
#                   stage-0+1 footprint.  See the note at LDKEEP below.
#   KEEP_UNUSED=0   let --gc-sections drop them; gives the minimal stage-0 image.
#   BL_USB_ECHO=0   (default from stage 4 on) the protocol owns the receive path.
#                   BL_USB_ECHO=1 restores the stage-3 loopback for a pyserial
#                   round-trip diagnostic; with echo on the echo pump drains the
#                   receive staging and the framer is never fed, so the device
#                   will NOT answer the update protocol.  usb.h still defaults
#                   this to 1 for a standalone compile; the build sets it here so
#                   every translation unit agrees.
#   EXTRA_CFLAGS=   passed through to every C compile.  The intended use is
#                     make EXTRA_CFLAGS=-DBL_DRY_RUN
#                   which builds a parse-and-ack-only image (proto.c short-
#                   circuits every flash op; nothing is erased or programmed).
#                   BL_DRY_RUN must reach proto.c, not just boot.c, which is why
#                   it is a build flag and not a #define in a header.
#   V=1             echo every command.

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
NM      := $(CROSS)nm
READELF := $(CROSS)readelf

# tools/check_image.py needs nothing but the standard library, so the system
# interpreter is the right default. Override with `make check PYTHON=...` if
# `python3` is not on PATH or points somewhere unhelpful.
PYTHON  ?= python3

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------
BUILD   := build
TARGET  := bootloader
LDSCRIPT := ld/bootloader.ld

# vectors.S must be first on the link line so .vectors lands at 0x00000000.
# (The linker script pins it anyway, but keeping the order explicit means a
# broken script shows up as a wrong address rather than as luck.)
ASRC := src/vectors.S src/start.S
# src/timebase.c is the SysTick millisecond clock that replaced the
# BL_LOOP_POLLS_PER_MS / BL_LED_POLLS_PER_MS iteration-count estimates.  Both
# boot.c and led.c call into it, so it is not optional and there is no knob.
# It is deliberately NOT in KEEP_SYMS below: bl_time_deinit() genuinely has no
# caller and --gc-sections is right to drop it, because bl_jump_to_app() clears
# SYST_CSR/SYST_CVR itself on the handoff path (check_image.py section 10
# asserts exactly that, so the day those stores disappear is a FAIL, not a
# silent regression).  Adding it to KEEP_SYMS would buy nothing but bytes.
CSRC := src/boot.c src/flash.c src/proto.c src/usb.c src/usb_desc.c src/led.c \
        src/timebase.c

OBJ := $(addprefix $(BUILD)/,$(notdir $(ASRC:.S=.o)) $(notdir $(CSRC:.c=.o)))

# Bootloader flash budget: 0x0000..0x3DFF.  0x3E00 is the boot-info record.
FLASH_BUDGET := 15872

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
ARCHFLAGS := -mcpu=cortex-m0 -mthumb

# Stage 4 turns the stage-3 loopback off: bl_update_mode() now drains the
# receive staging into the protocol framer, and the echo pump would consume it
# first.  Set BL_USB_ECHO=1 on the command line to rebuild the stage-3
# diagnostic image.  Must be assigned BEFORE CFLAGS, which uses `:=`.
BL_USB_ECHO ?= 0

# -fno-builtin / -fno-tree-loop-distribute-patterns are not cosmetic: without
# them GCC rewrites hand-written byte loops into calls to memcpy/memset, which
# do not exist under -nostdlib.  proto.c's owner hit exactly this.
# -fno-unwind-tables keeps .ARM.exidx empty; the linker script KEEPs those
# sections deliberately so a stray entry shows up as size rather than vanishing.
# -fno-jump-tables is the same class of problem, found in stage 3: GCC 15 turns
# a dense switch into a Thumb-1 dispatch table and calls __gnu_thumb1_case_sqi,
# which lives in libgcc and is unresolved under -nostdlib.  usb.c's ep0_standard
# carries a per-function attribute for this; the global flag makes the whole
# tree immune and makes that attribute redundant rather than load-bearing.
CFLAGS := $(ARCHFLAGS) \
          -Os \
          -ffreestanding -fno-builtin -fno-common \
          -fno-tree-loop-distribute-patterns \
          -fno-jump-tables \
          -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra \
          -DBL_USB_ECHO=$(BL_USB_ECHO) \
          -Iinclude -MMD -MP $(EXTRA_CFLAGS)

ASFLAGS := $(ARCHFLAGS) -x assembler-with-cpp -Iinclude -MMD -MP

LDFLAGS := $(ARCHFLAGS) -nostdlib -nostartfiles \
           -T $(LDSCRIPT) \
           -Wl,--gc-sections \
           -Wl,--no-warn-rwx-segments \
           -Wl,-Map=$(BUILD)/$(TARGET).map \
           -Wl,--print-memory-usage

# This list exists because through stages 0-3 nothing in the image called into
# proto.c or flash.c, --gc-sections would have discarded them, and the reported
# size would then have understated the footprint stage 4 had to fit into.
#
# STAGE 4 CHANGED THAT.  bl_update_mode() now drives the framer against the
# real flash driver, so almost everything below is reached on its own merits
# and the knob is no longer load-bearing.  As of this build only four entries
# still have no caller anywhere in the image:
#
#   bl_proto_feed_buf   the buffer-at-a-time convenience wrapper; the update
#                       loop feeds bl_proto_feed() a byte at a time instead
#   bl_flash_lock       belt-and-braces public re-lock; flash_command() already
#                       re-locks inline on every path
#   bl_flash_is_erased  blank-check helper, used by the host suite only
#   bl_usb_configured   stage-4 was expected to gate on it; the loop turned out
#                       not to need it (bl_usb_rx returns 0 until configured)
#
# They cost 96 bytes of flash together, they cannot execute, and they are all
# tested public API — so they are kept by default rather than pruned, and
# tools/check_image.py §9 PRINTS this exact list every run so the number stays
# visible instead of turning into folklore.  `make KEEP_UNUSED=0` drops them.
#
# bl_flash_erase_sector and bl_flash_program have no textual call site either,
# but they are NOT orphans: they are reached through bl_update_flash_ops, which
# check_image.py §9 resolves entry by entry.  Do not remove them from the list
# on the strength of a grep.
#
# Delete an entry only when something in the image genuinely calls it directly;
# tools/check_image.py §8 asserts the USB API is present under KEEP_UNUSED=1,
# so building with KEEP_UNUSED=0 will (correctly) warn about bl_usb_configured.
KEEP_UNUSED ?= 1
KEEP_SYMS := bl_proto_reset bl_proto_bind bl_proto_feed bl_proto_feed_buf \
             bl_frame_build bl_proto_finalized bl_crc16 bl_crc16_update \
             bl_flash_erase_sector bl_flash_program bl_flash_program_word \
             bl_flash_lock bl_flash_is_erased \
             bl_usb_tx bl_usb_configured
ifeq ($(KEEP_UNUSED),1)
LDKEEP := $(foreach s,$(KEEP_SYMS),-Wl,--undefined=$(s))
else
LDKEEP :=
endif

ifeq ($(V),1)
Q :=
else
Q := @
endif

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------
.PHONY: all clean size disasm check host-test syms dist
.DEFAULT_GOAL := all

all: $(BUILD)/$(TARGET).bin

$(BUILD):
	$(Q)mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	@echo "  AS      $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJ) $(LDSCRIPT) | $(BUILD)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(LDKEEP) $(OBJ) -o $@
	@# -nostdlib does not stop the link from succeeding with dangling weak refs;
	@# make an unresolved symbol loud rather than a silent runtime hang.
	$(Q)if $(NM) -u $@ | grep -q .; then \
	    echo "ERROR: unresolved symbols in $@:"; $(NM) -u $@; exit 1; fi

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	@echo "  OBJCOPY $@"
	$(Q)$(OBJCOPY) -O binary $< $@
	@sz=$$(wc -c < $@); \
	 echo "  IMAGE   $$sz bytes / $(FLASH_BUDGET) budget ($$(( $(FLASH_BUDGET) - $$sz )) free)"; \
	 if [ $$sz -gt $(FLASH_BUDGET) ]; then \
	    echo "ERROR: image exceeds the $(FLASH_BUDGET)-byte bootloader region"; exit 1; fi

size: $(BUILD)/$(TARGET).bin
	@$(SIZE) -A -x $(BUILD)/$(TARGET).elf
	@echo
	@$(SIZE) $(BUILD)/$(TARGET).elf
	@echo
	@sz=$$(wc -c < $(BUILD)/$(TARGET).bin); \
	 echo "flash image      : $$sz bytes (0x$$(printf %x $$sz))"; \
	 echo "flash budget     : $(FLASH_BUDGET) bytes (0x3E00)"; \
	 echo "headroom         : $$(( $(FLASH_BUDGET) - $$sz )) bytes"; \
	 echo "used             : $$(( $$sz * 100 / $(FLASH_BUDGET) ))%"
	@echo
	@echo "per-module .text (KEEP_UNUSED=$(KEEP_UNUSED)):"
	@for o in $(OBJ); do \
	   printf "  %-22s %6s\n" "$$(basename $$o)" \
	     "$$($(SIZE) -A -d $$o | awk '/^\.text/ {t+=$$2} END {print t+0}')"; \
	 done

disasm: $(BUILD)/$(TARGET).elf
	@echo "  DISASM  $(BUILD)/$(TARGET).lst"
	$(Q)$(OBJDUMP) -h $< >  $(BUILD)/$(TARGET).lst
	$(Q)echo             >> $(BUILD)/$(TARGET).lst
	$(Q)$(OBJDUMP) -d -z --source-comment='// ' $< >> $(BUILD)/$(TARGET).lst
	$(Q)$(OBJDUMP) -s -j .vectors $< > $(BUILD)/$(TARGET).vectors.txt
	@echo "  DISASM  $(BUILD)/$(TARGET).vectors.txt"

syms: $(BUILD)/$(TARGET).elf
	@$(NM) -n -S $<

# check_image.py section 9(c2) reads proto.c's range_ok() window out of its
# literal pool.  The -DBL_DRY_RUN variant makes proto.c's flash wrappers return
# before the flash ops, so GCC inlines them and range_ok() with them and there
# is no pool left to read.  Tell the checker, so it can downgrade exactly those
# two claims to warnings instead of failing a legitimate variant build.
#
# The flag is derived from EXTRA_CFLAGS rather than being a knob of its own, so
# it cannot get out of step with the build it describes, and check_image.py
# does not simply believe it: it first confirms the image really carries the
# dry-run fingerprint (fl_erase, fl_program and fl_read ALL gone).  A shipping
# build with the guard reverted keeps those three, so passing this flag to one
# cannot silence the failure.
DRY_RUN_BUILD := $(if $(findstring BL_DRY_RUN,$(EXTRA_CFLAGS)),1,0)

check: $(BUILD)/$(TARGET).bin
	$(Q)$(PYTHON) tools/check_image.py \
	    --bin $(BUILD)/$(TARGET).bin \
	    --elf $(BUILD)/$(TARGET).elf \
	    --objdump $(OBJDUMP) \
	    --nm $(NM) \
	    --keep-unused $(KEEP_UNUSED) \
	    --dry-run-build $(DRY_RUN_BUILD)

host-test:
	$(Q)$(MAKE) -C host test

# ---------------------------------------------------------------------------
# Release staging
# ---------------------------------------------------------------------------
# WHAT A RELEASE CAN AND CANNOT CONTAIN.
#
# `wchisp flash` erases CodeFlash and writes from address 0, so
# bootloader.bin ALONE IS NOT FLASHABLE: writing it by itself erases the
# application with it and leaves a device with a bootloader and nothing to
# boot.  The flashable artefact is the composite (bootloader + an application),
# and the application half is vendor firmware, which this project does not
# have the right to redistribute and must never acquire.
#
# So a ready-made flashable image cannot be published, and the release ships
# the ingredients plus the tool that combines them instead: the user runs
# build_composite.py against THEIR OWN backup and gets the composite locally.
# That is not a workaround -- the user's own dump is the correct source for the
# application half anyway, because it is by definition the firmware that device
# is already running, which is what makes the install a bootloader-only change.
#
# The two read-only helpers ride along because the install sequence needs them
# before anything is written: check-bootloader-region.py says whether the
# device needs this at all, and backup-codeflash.py produces the backup that
# build_composite.py then consumes.
#
# install.py is the front door: it performs that whole sequence itself, refuses
# to continue past any precondition it cannot verify, and carries --restore for
# a device that will not boot.  It is a single file needing only the standard
# library and pyserial, so it works from this flat directory exactly as it does
# from a checkout.  The three scripts above still work standalone, and the docs
# still reference them individually, which is why they are shipped separately
# rather than folded in.
#
# The staging itself lives in tools/stage_release.py: it copies, stamps
# install.py's BL_SHA256 with the digest of the bootloader.bin staged beside it,
# proves the stamp took, writes SHA256SUMS, and produces both a zip and the
# loose files.  That is more than a `cp` and a checksum should be doing in a
# recipe, and it needs to fail loudly rather than half-succeed.
DISTDIR := $(BUILD)/dist

# Put the real repository URL in the payload's README.txt.  CI passes
# GITHUB_REPOSITORY; a local `make dist` gets the placeholder and says so.
DIST_REPO_URL ?= $(if $(GITHUB_REPOSITORY),https://github.com/$(GITHUB_REPOSITORY),)

dist: $(BUILD)/$(TARGET).bin
	$(Q)$(PYTHON) tools/stage_release.py \
	    --bootloader $(BUILD)/$(TARGET).bin \
	    --out $(DISTDIR) \
	    $(if $(DIST_REPO_URL),--repo-url $(DIST_REPO_URL),)

clean:
	$(Q)rm -rf $(BUILD)

-include $(OBJ:.o=.d)
