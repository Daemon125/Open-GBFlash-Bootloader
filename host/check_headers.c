/* check_headers.c — cross-module constant agreement check.
 *
 * THREE headers describe the same flash geometry:
 *   include/proto.h     — the updater's view, portable, <stdint.h> only
 *   include/boot.h      — the validator's view
 *   include/bl_config.h — the reference/documentation header
 *
 * proto.h and boot.h BOTH define the application-length window — BL_APP_MAX_LEN
 * (both `#ifndef`-wrapped) and BL_APP_MIN_LEN (proto.h wraps it; boot.h does
 * not, so a TU including both gets a hard redefinition diagnostic the moment
 * the two values stop being spelled identically — a second, independent
 * tripwire on top of this program). For the `#ifndef` pair:
 *   - no redefinition warning is ever emitted;
 *   - in any translation unit that includes both, whichever header comes FIRST
 *     silently wins.
 * They once disagreed 8x (0x7600 vs 0x3A800), which is an update-breaking
 * defect; that is why this program exists.
 *
 * bl_config.h IS included elsewhere — src/led.c, src/usb.c and src/timebase.c
 * take GPIO, USB and clock constants from it — but nothing else compiles its
 * flash-geometry or boot-info half, so nothing else can notice when that half
 * drifts. It did drift: it kept BL_BI_MAX_APP_LEN at the defective 0x7600
 * through the whole fix. This program is the only reader of those constants.
 *
 * Exit status: this used to always return 0 and merely print a warning. It now
 * returns 1 on any disagreement, so `make -C host test` fails instead of
 * scrolling past. All three headers agree today; a future divergence is a
 * defect, not a matter of taste.
 */

#include <stdio.h>
#include <stdint.h>

/* timebase.h keeps a PRIVATE copy of Fsys (BL_TIME_FSYS_HZ) rather than
 * including bl_config.h, because boot.c includes timebase.h and must not have
 * bl_config.h dragged in behind it. src/timebase.c _Static_asserts the two
 * agree, so a target build already catches a divergence — but this program is
 * the one place that compares the two headers directly, so the comparison
 * belongs here too. A wrong Fsys does not fail
 * to build: it silently rescales every timeout in the image, which is the
 * exact failure the poll-count estimate produced. */
#include "timebase.h"
static const uint32_t tb_fsys    = BL_TIME_FSYS_HZ;
static const uint32_t tb_cyc_us  = BL_TIME_CYCLES_PER_US;
static const uint32_t tb_max_gap = BL_TIME_MAX_GAP_MS;
static const uint32_t tb_csr_run = BL_TIME_CSR_RUN;
static const uint32_t tb_tickint = BL_TIME_CSR_TICKINT;

#include "proto.h"
static const uint32_t proto_app_max   = BL_APP_MAX_LEN;
static const uint32_t proto_app_min   = BL_APP_MIN_LEN;
static const uint32_t proto_app_base  = BL_APP_BASE;
static const uint32_t proto_img_base  = BL_IMAGE_BASE;
static const uint32_t proto_sector    = BL_SECTOR;
static const uint32_t proto_fl_end    = BL_FLASH_END;
static const uint32_t proto_img_end   = BL_IMAGE_END;
/* The set of boot-info fields hdr_check() + app_gates() inspect before finalize
 * may answer SUCCESS.  See the long note in boot.h: the two lengths below were
 * comparable as constants, the marker gate was CODE and was comparable by
 * nothing, and it diverged for exactly that reason.  Both headers also
 * _Static_assert this equality (whichever is included second carries it), so a
 * divergence normally fails to COMPILE here; the runtime comparison prints the
 * two sets and survives a compiler without _Static_assert. */
static const uint32_t proto_gates     = BL_WRITER_GATES;

/* Drop proto.h's definitions so boot.h's own #ifndef fallbacks are visible. */
#undef BL_APP_MAX_LEN
#undef BL_APP_MIN_LEN
#undef BL_APP_BASE
#undef BL_FLASH_END

#include "boot.h"
static const uint32_t boot_app_max  = BL_APP_MAX_LEN;
static const uint32_t boot_app_min  = BL_APP_MIN_LEN;
static const uint32_t boot_app_base = BL_APP_BASE;
static const uint32_t boot_limit    = BL_BOOT_LIMIT;
static const uint32_t boot_sector   = BL_FLASH_SECTOR;
static const uint32_t boot_cf_end   = BL_CODEFLASH_END;
static const uint32_t boot_btn_mask = BL_BTN_MASK;
static const uint32_t boot_pb_pin   = BL_R32_PB_PIN;
static const uint32_t boot_bi_base  = BL_BOOTINFO_BASE;
/* ...and the set bl_app_valid() inspects before the device will boot it. */
static const uint32_t boot_gates    = BL_VALIDATOR_GATES;
static const uint32_t gate_marker   = BL_GATE_MARKER;

/* Now drop everything boot.h and bl_config.h have in common, so bl_config.h's
 * own values are what we read back rather than boot.h's. */
#undef BL_APP_BASE
#undef BL_BOOTINFO_BASE
#undef BL_BOOTINFO_LEN
#undef BL_BTN_MASK
#undef BL_CODEFLASH_END
#undef BL_R32_PB_CLR
#undef BL_R32_PB_DIR
#undef BL_R32_PB_OUT
#undef BL_R32_PB_PD_DRV
#undef BL_R32_PB_PIN
#undef BL_R32_PB_PU
#undef BL_SRAM_BASE

#include "bl_config.h"
static const uint32_t cfg_app_base = BL_APP_BASE;
static const uint32_t cfg_cf_end   = BL_CODEFLASH_END;
static const uint32_t cfg_sector   = BL_FLASH_SECTOR_SIZE;
static const uint32_t cfg_bi_base  = BL_BOOTINFO_BASE;
static const uint32_t cfg_app_max  = BL_APP_MAX_SIZE;
static const uint32_t cfg_bi_max   = BL_BI_MAX_APP_LEN;
static const uint32_t cfg_btn_mask = BL_BTN_MASK;
static const uint32_t cfg_pb_pin   = BL_R32_PB_PIN;
static const uint32_t cfg_fsys     = BL_FSYS_HZ;

int main(void)
{
    int warn = 0;

    printf("header constants:\n");
    printf("  image base / boot limit : proto 0x%05X   boot 0x%05X\n",
           proto_img_base, boot_limit);
    printf("  app base                : proto 0x%05X   boot 0x%05X\n",
           proto_app_base, boot_app_base);
    printf("  sector                  : proto 0x%05X   boot 0x%05X\n",
           proto_sector, boot_sector);
    printf("  CodeFlash end           : proto 0x%05X   boot 0x%05X\n",
           proto_fl_end, boot_cf_end);
    printf("  BL_APP_MAX_LEN          : proto 0x%05X   boot 0x%05X\n",
           proto_app_max, boot_app_max);
    printf("  BL_APP_MIN_LEN          : proto 0x%05X   boot 0x%05X\n",
           proto_app_min, boot_app_min);
    printf("  writable window end     : proto 0x%05X   (CodeFlash 0x%05X)\n",
           proto_img_end, proto_fl_end);
    printf("  boot-info gate set      : writer 0x%02X    validator 0x%02X"
           "   (marker gate 0x%02X: %s)\n",
           proto_gates, boot_gates, gate_marker,
           (proto_gates & gate_marker) ? "ON" : "off, both sides");
    printf("  bl_config.h (geometry half compiled only here):\n");
    printf("    app base 0x%05X  CodeFlash end 0x%05X  sector 0x%05X\n",
           cfg_app_base, cfg_cf_end, cfg_sector);
    printf("    boot-info base 0x%05X  APP_MAX_SIZE 0x%05X  BI_MAX_APP_LEN 0x%05X\n",
           cfg_bi_base, cfg_app_max, cfg_bi_max);
    printf("    button mask 0x%08X  R32_PB_PIN 0x%08X\n",
           cfg_btn_mask, cfg_pb_pin);
    printf("  time base               : timebase.h Fsys %u Hz   "
           "bl_config.h %u Hz\n", tb_fsys, cfg_fsys);
    printf("    %u cycles/us, one counter lap = %u ms, SYST_CSR run value %u\n",
           tb_cyc_us, tb_max_gap, tb_csr_run);

    if (tb_fsys != cfg_fsys) {
        printf("  ** timebase.h's Fsys (%u Hz) disagrees with bl_config.h\n"
               "     (%u Hz). Nothing fails to build and nothing is out of\n"
               "     range — every timeout in the image is simply scaled by\n"
               "     the ratio. That is precisely the class of defect the\n"
               "     poll-count estimate was: a 50 ms framer timeout that\n"
               "     fires at 186 ms and a settle window that outlasts its\n"
               "     own documented bound.\n", tb_fsys, cfg_fsys);
        warn++;
    }
    if ((tb_csr_run & tb_tickint) != 0u) {
        printf("  ** BL_TIME_CSR_RUN (%u) has TICKINT set. SysTick would raise\n"
               "     exception 15, which this vector table trampolines into the\n"
               "     APPLICATION's table at 0x4000 — erased during an update.\n"
               "     The fetch returns 0xFFFFFFFF, the range guard rejects it,\n"
               "     and the core spins with CodeFlash unlocked: H1-jumper\n"
               "     recovery only.\n", tb_csr_run);
        warn++;
    }
    if (tb_cyc_us == 0u || (tb_cyc_us & (tb_cyc_us - 1u)) != 0u) {
        printf("  ** cycles-per-microsecond (%u) is not a power of two.\n"
               "     bl_time_ms() converts with a shift because Cortex-M0 has\n"
               "     no divide and -nostdlib has no __aeabi_uidiv.\n", tb_cyc_us);
        warn++;
    }

    if (cfg_app_base != boot_app_base || cfg_cf_end != boot_cf_end
        || cfg_sector != boot_sector || cfg_bi_base != boot_bi_base) {
        printf("  ** bl_config.h's flash geometry disagrees with boot.h\n");
        warn++;
    }
    if (cfg_app_max != boot_app_max || cfg_bi_max != boot_app_max) {
        printf("  ** bl_config.h's application-length ceiling (APP_MAX_SIZE 0x%X,\n"
               "     BI_MAX_APP_LEN 0x%X) disagrees with BL_APP_MAX_LEN 0x%X.\n"
               "     This is the update-breaking divergence class: an updater\n"
               "     stricter than the validator erases the boot-info sector and\n"
               "     then refuses the rest of the image.\n",
               cfg_app_max, cfg_bi_max, boot_app_max);
        warn++;
    }
    if (cfg_btn_mask != boot_btn_mask || cfg_pb_pin != boot_pb_pin) {
        printf("  ** bl_config.h's U22/GPIO description disagrees with boot.h.\n"
               "     A wrong button pin reading as always-pressed traps the\n"
               "     device in update mode.\n");
        warn++;
    }

    if (proto_img_base != boot_limit) {
        printf("  ** the write floor disagrees between the two headers\n");
        warn++;
    }
    if (proto_app_base != boot_app_base || proto_sector != boot_sector
        || proto_fl_end != boot_cf_end) {
        printf("  ** a geometry constant disagrees between the two headers\n");
        warn++;
    }
    if (proto_app_max != boot_app_max) {
        printf("  ** WARNING: BL_APP_MAX_LEN disagrees. proto.c refuses a 0x24\n"
               "     packet that would take the image past 0x%X, but boot.c\n"
               "     would happily validate and boot an application up to\n"
               "     0x%X. The updater is the STRICTER of the two, so a\n"
               "     firmware between those sizes is NAKed forever (FlashGBX\n"
               "     retries a NAK at 1 s intervals with no limit) rather than\n"
               "     written. Both headers guard the macro with #ifndef, so a\n"
               "     translation unit including both silently takes whichever\n"
               "     header it included first — verified, no warning either\n"
               "     way. Pick one value and define it in one place.\n",
               proto_app_max, boot_app_max);
        warn++;
    }
    if (proto_app_min != boot_app_min) {
        printf("  ** WARNING: BL_APP_MIN_LEN disagrees. proto 0x%X, boot 0x%X.\n"
               "     This is the MIRROR of the BL_APP_MAX_LEN divergence and\n"
               "     it is just as much a defect. h_final() applies this bound\n"
               "     in hdr_check() and bl_app_valid() applies it at boot; if\n"
               "     the updater is LOOSER, the device answers 0x01 SUCCESS\n"
               "     for an image the boot decision then refuses forever, and\n"
               "     sits in update mode reporting a successful update. If the\n"
               "     updater is STRICTER, a legitimate image is refused at\n"
               "     finalize after the boot-info sector has already been\n"
               "     erased. Pick one value; the writer must be neither\n"
               "     stricter nor looser than the validator.\n",
               proto_app_min, boot_app_min);
        warn++;
    }
    if (proto_gates != boot_gates) {
        printf("  ** WARNING: the boot-info GATE SETS disagree. writer 0x%02X,\n"
               "     validator 0x%02X, differing bits 0x%02X. This is the same\n"
               "     defect class as the two length divergences above, one\n"
               "     level up: hdr_check() + app_gates() in proto.c and\n"
               "     bl_app_valid() in boot.c must inspect the SAME fields of\n"
               "     the same 14-byte record. A writer that is STRICTER refuses\n"
               "     a legitimate image at finalize, after packet 1 has already\n"
               "     erased the boot-info sector — the device is then in update\n"
               "     mode with no valid application. A writer that is LOOSER\n"
               "     answers 0x01 SUCCESS for an image the boot decision then\n"
               "     refuses forever. The marker whitelist (bit 0x%02X) was\n"
               "     exactly the first case and is why this comparison exists.\n",
               proto_gates, boot_gates, proto_gates ^ boot_gates, gate_marker);
        warn++;
    }
    if (proto_img_end > proto_fl_end) {
        printf("  ** the updater's writable window (0x%X) extends past\n"
               "     CodeFlash (0x%X). range_ok() bounds every erase, program\n"
               "     and read with that single constant.\n",
               proto_img_end, proto_fl_end);
        warn++;
    }
    if (!warn) {
        printf("  all cross-module constants agree\n");
        return 0;
    }
    printf("  ** %d disagreement(s) — this is a build failure, not a note\n", warn);
    return 1;
}
