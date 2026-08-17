/* test_flash.c — native test suite for src/flash.c.
 *
 * src/flash.c is the module that actually erases and programs silicon, and it
 * had ZERO executable coverage.  It is compiled here unmodified except for the
 * fifteen-line MMIO/asm/SRAM-window redirection described in flash_model.h,
 * and run against a register-level model of the CH579 flash controller.
 *
 * The suite is organised as:
 *   A  model self-test — including a demonstration that the model has NO write
 *      floor and that driving it below 0x3E00 really does trip the tripwire.
 *      A harness that silently refuses the dangerous operation would make
 *      every guard test below pass for the wrong reason.
 *   B  bl_flash_erase_sector guards
 *   C  bl_flash_program_word guards
 *   D  bl_flash_program guards
 *   E  bl_flash_check_erased / bl_flash_is_erased guards and boundaries
 *   F  programming and erase semantics (AND, precise 512-byte bounds,
 *      endianness, partial progress)
 *   G  fault injection — the controller reports a failure, or silently does
 *      not do what it said it did
 *   H  lock and interrupt invariants
 *
 * Every guard attempt is checked four ways, not one: the exact return code,
 * that ZERO controller traffic was generated (the guard rejected it without
 * touching the hardware), that flash is left LOCKED, and that the bootloader
 * region below 0x3E00 still holds its tripwire pattern.
 *
 * COVERAGE, measured with clang -fcoverage-mapping over the generated
 * build/flash_host.c: 156 regions, 1 missed (99.36%); 11 functions, 0 missed;
 * 70 branches, 1 missed.  The single unreached region is the `len == 0u`
 * early-out in range_in_sram() at src/flash.c:100-102.  It is unreachable by
 * construction, not untested: bl_flash_program() is range_in_sram()'s only
 * caller and it has already returned BL_FLASH_ERR_PARAM for len == 0 three
 * checks earlier.  It is correct defensive code and should stay; nothing in
 * this file can drive it, and a test that pretended to would be lying.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "flash.h"
#include "flash_model.h"

/* ------------------------------------------------------------------ */
/* Tiny test framework                                                 */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;
static const char *g_group = "";

/* Distinct check() call sites executed, so the summary can report claims as
 * well as executions (loops otherwise inflate the headline number). */
static int g_sites[1024];
static int g_nsites;

static void note_site(int line)
{
    int i;
    for (i = 0; i < g_nsites; i++) {
        if (g_sites[i] == line) return;
    }
    if (g_nsites < (int)(sizeof g_sites / sizeof g_sites[0])) {
        g_sites[g_nsites++] = line;
    }
}

static void checkf(int line, int cond, const char *fmt, ...)
{
    note_site(line);
    if (cond) {
        g_pass++;
        return;
    }
    g_fail++;
    fprintf(stderr, "FAIL %s:%d [%s] ", "test_flash.c", line, g_group);
    {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
}

#define CHECK(cond, ...) checkf(__LINE__, (cond), __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Cumulative "must stay zero" safety counters                         */
/* ------------------------------------------------------------------ */

static uint32_t g_acc[FM_STAT_NCOUNTERS];

/* Re-init the model, first folding its counters into the run-wide totals so
 * that a per-group fm_init() cannot hide a safety violation. */
static void fm_reset(void)
{
    int i;
    for (i = 0; i < FM_STAT_NCOUNTERS; i++) {
        g_acc[i] += fm_stat(i);
    }
    fm_init();
}

static uint32_t acc_total(int which)
{
    return g_acc[which] + fm_stat(which);
}

/* ------------------------------------------------------------------ */
/* Guard-violation accounting                                          */
/* ------------------------------------------------------------------ */

static unsigned g_guard_attempts;
static unsigned g_guard_refused;

static void guard_result(int line, const char *expr, int rc, int want,
                         uint32_t regw_before)
{
    int ok = 1;

    g_guard_attempts++;

    if (rc != want) {
        ok = 0;
        fprintf(stderr, "  guard rc: %s -> %d, want %d\n", expr, rc, want);
    }
    if (fm_stat(FM_STAT_REG_WRITES) != regw_before) {
        ok = 0;
        fprintf(stderr, "  guard touched the controller: %s (%u reg writes)\n",
                expr, (unsigned)(fm_stat(FM_STAT_REG_WRITES) - regw_before));
    }
    if (fm_unlocked()) {
        ok = 0;
        fprintf(stderr, "  guard left flash UNLOCKED: %s\n", expr);
    }
    if (!fm_bootloader_intact()) {
        ok = 0;
        fprintf(stderr, "  guard let the bootloader region change: %s\n", expr);
    }
    if (fm_stat(FM_STAT_CMD_BELOW_FLOOR) != 0u) {
        ok = 0;
        fprintf(stderr, "  a command reached the controller below 0x3E00: %s\n",
                expr);
    }

    if (ok) g_guard_refused++;
    checkf(line, ok, "guard refused %s with %d", expr, want);
}

/* Attempt something flash.c must refuse. */
#define GUARD(want, call)                                                     \
    do {                                                                      \
        uint32_t rw_ = fm_stat(FM_STAT_REG_WRITES);                           \
        int rc_ = (call);                                                     \
        guard_result(__LINE__, #call, rc_, (want), rw_);                      \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Copy a buffer into the modelled SRAM window and return a pointer to it —
 * bl_flash_program() only accepts sources that live there. */
static void *sram_load(const void *data, uint32_t len)
{
    uint8_t *p = (uint8_t *)fm_sram();
    if (len > FM_SRAM_SIZE) abort();
    memcpy(p, data, len);
    return p;
}

static uint32_t mem32(uint32_t addr)
{
    const uint8_t *m = fm_mem();
    return (uint32_t)m[addr] | ((uint32_t)m[addr + 1u] << 8)
         | ((uint32_t)m[addr + 2u] << 16) | ((uint32_t)m[addr + 3u] << 24);
}

static int all_bytes(uint32_t addr, uint32_t len, uint8_t val)
{
    uint32_t i;
    for (i = 0u; i < len; i++) {
        if (fm_mem()[addr + i] != val) return 0;
    }
    return 1;
}

static uint8_t prand(uint32_t i)
{
    return (uint8_t)((i * 1103515245u + 12345u) >> 16);
}

/* ------------------------------------------------------------------ */
/* A. Model self-test                                                  */
/* ------------------------------------------------------------------ */

static void test_model(void)
{
    uint8_t word[4];

    g_group = "model";
    fm_init();

    CHECK(fm_mem()[0x3E00] == 0xFFu, "writable region starts erased");
    CHECK(all_bytes(BL_FLASH_WRITE_FLOOR,
                    FM_FLASH_SIZE - BL_FLASH_WRITE_FLOOR, 0xFF),
          "the whole writable region is 0xFF after init");
    CHECK(fm_mem()[0x0000] != 0xFFu && fm_mem()[0x3DFF] != 0xFFu,
          "bootloader region carries a non-0xFF tripwire pattern");
    CHECK(fm_bootloader_intact(), "tripwire reads intact after init");

    /* Every raw register sequence in this group masks interrupts around the
     * unlocked window, exactly as flash.c's irq_lock()/irq_unlock() does.
     * Without that the model would (correctly) count these as commands issued
     * with interrupts enabled and the run-wide invariant would be diluted by
     * the harness's own traffic. */
    fm_cpsid();

    /* Protect register: 0x88 unlocks and READS BACK 0x08. */
    fm_w8(FM_PROTECT, 0x88u);
    CHECK(fm_unlocked(), "0x88 enables code-flash writes");
    CHECK(fm_protect_readback() == 0x08u,
          "0x88 reads back as 0x08 (WE_MUST_10 is write-only), got 0x%02X",
          fm_protect_readback());
    fm_w8(FM_PROTECT, 0x80u);
    CHECK(!fm_unlocked(), "0x80 re-locks");
    CHECK(fm_protect_readback() == 0x00u, "0x80 reads back as 0x00");

    /* A command while locked must do nothing and report an error. */
    fm_w32(FM_ADDR, 0x4000u);
    fm_w8(FM_CMD, FM_CMD_ERASE);
    CHECK(fm_stat(FM_STAT_CMD_WHILE_LOCKED) == 1u,
          "a command issued while locked is recorded");
    CHECK((fm_status() & 0xFFu) != 0x40u,
          "a locked command does not report success");
    CHECK(fm_status() & FM_ST_ALWAYS_SET,
          "status bit 9 always reads 1 (0x%04X)", fm_status());

    /* Erase is exactly 512 bytes. */
    fm_reset();
    fm_cpsid();
    fm_fill(0x4000u, 0x00u, 0x600u);
    fm_w8(FM_PROTECT, 0x88u);
    fm_w32(FM_ADDR, 0x4200u);
    fm_w8(FM_CMD, FM_CMD_ERASE);
    fm_w8(FM_PROTECT, 0x80u);
    CHECK((fm_status() & 0xFFu) == 0x40u, "erase reports 0x40 success");
    CHECK(all_bytes(0x4200u, 512u, 0xFF), "erase blanked its 512 bytes");
    CHECK(all_bytes(0x4000u, 512u, 0x00), "erase did not reach the sector below");
    CHECK(all_bytes(0x4400u, 512u, 0x00), "erase did not reach the sector above");

    /* Programming ANDs. */
    fm_reset();
    fm_cpsid();
    fm_w8(FM_PROTECT, 0x88u);
    fm_w32(FM_ADDR, 0x4000u);
    fm_w32(FM_DATA, 0x0F0F0F0Fu);
    fm_w8(FM_CMD, FM_CMD_PROG);
    fm_w32(FM_DATA, 0xFFFFFFFFu);
    fm_w8(FM_CMD, FM_CMD_PROG);
    fm_w8(FM_PROTECT, 0x80u);
    CHECK(mem32(0x4000u) == 0x0F0F0F0Fu,
          "programming 0xFFFFFFFF over 0x0F0F0F0F is a no-op (AND), got %08X",
          mem32(0x4000u));

    /* Little-endian storage. */
    fm_reset();
    fm_cpsid();
    fm_w8(FM_PROTECT, 0x88u);
    fm_w32(FM_ADDR, 0x4000u);
    fm_w32(FM_DATA, 0x04030201u);
    fm_w8(FM_CMD, FM_CMD_PROG);
    fm_w8(FM_PROTECT, 0x80u);
    memcpy(word, fm_mem() + 0x4000u, 4u);
    CHECK(word[0] == 0x01u && word[1] == 0x02u
       && word[2] == 0x03u && word[3] == 0x04u, "words are stored little-endian");

    /* THE MODEL HAS NO WRITE FLOOR.  Prove the tripwire fires, otherwise every
     * guard test in this file could be passing because the harness is the one
     * refusing.  Counters from this block are deliberately discarded. */
    fm_reset();
    fm_cpsid();
    fm_w8(FM_PROTECT, 0x88u);
    fm_w32(FM_ADDR, 0x0000u);
    fm_w8(FM_CMD, FM_CMD_ERASE);
    fm_w8(FM_PROTECT, 0x80u);
    CHECK((fm_status() & 0xFFu) == 0x40u,
          "the silicon happily erases sector 0 when told to");
    CHECK(all_bytes(0x0000u, 512u, 0xFF), "sector 0 really was erased");
    CHECK(!fm_bootloader_intact(), "the tripwire fires on a below-floor erase");
    CHECK(fm_stat(FM_STAT_CMD_BELOW_FLOOR) == 1u,
          "the below-floor command was counted");

    fm_cpsie();
    fm_init();          /* discard: this group's violations were intentional */
    CHECK(fm_bootloader_intact(), "tripwire restored for the real tests");
}

/* ------------------------------------------------------------------ */
/* B. bl_flash_erase_sector guards                                     */
/* ------------------------------------------------------------------ */

static void test_erase_guards(void)
{
    static const uint32_t below[] = {
        0x00000000u, 0x00000200u, 0x00001000u, 0x00002000u,
        0x00003A00u, 0x00003C00u
    };
    static const uint32_t misaligned[] = {
        0x00000001u, 0x00003DFFu,               /* misaligned AND below floor */
        0x00003E01u, 0x00003E02u, 0x00003E04u,
        0x00003F00u, 0x00004001u, 0x00004100u,
        0x0003E7FFu, 0xFFFFFFFFu, 0xFFFFFFFCu
    };
    static const uint32_t above[] = {
        0x0003E800u,                            /* first byte past CodeFlash  */
        0x0003EA00u,
        0x00040000u,                            /* InfoFlash: CFG_BOOT_EN     */
        0x00040200u,
        0x80000000u,
        0xFFFFFE00u                             /* aligned, wraps if added to */
    };
    size_t i;

    g_group = "erase-guards";
    fm_reset();

    for (i = 0u; i < sizeof below / sizeof below[0]; i++) {
        GUARD(BL_FLASH_ERR_RANGE, bl_flash_erase_sector(below[i]));
    }
    for (i = 0u; i < sizeof misaligned / sizeof misaligned[0]; i++) {
        GUARD(BL_FLASH_ERR_ALIGN, bl_flash_erase_sector(misaligned[i]));
    }
    for (i = 0u; i < sizeof above / sizeof above[0]; i++) {
        GUARD(BL_FLASH_ERR_RANGE, bl_flash_erase_sector(above[i]));
    }

    CHECK(fm_stat(FM_STAT_REG_WRITES) == 0u,
          "not one register write escaped %u refused erases",
          (unsigned)(sizeof below / sizeof below[0]
                   + sizeof misaligned / sizeof misaligned[0]
                   + sizeof above / sizeof above[0]));

    /* The boundary sectors that MUST be accepted, so the guard is not simply
     * refusing everything. */
    CHECK(bl_flash_erase_sector(BL_FLASH_WRITE_FLOOR) == BL_FLASH_OK,
          "0x3E00, the first legal sector, is accepted");
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_OK,
          "0x4000, the application base, is accepted");
    CHECK(bl_flash_erase_sector(BL_FLASH_END - BL_FLASH_SECTOR_SIZE)
          == BL_FLASH_OK, "0x3E600, the last sector, is accepted");
    CHECK(!fm_unlocked(), "flash locked after the accepted erases");
    CHECK(fm_bootloader_intact(), "bootloader intact after erase guard sweep");
}

/* ------------------------------------------------------------------ */
/* C. bl_flash_program_word guards                                     */
/* ------------------------------------------------------------------ */

static void test_program_word_guards(void)
{
    static const uint32_t below[] = {
        0x00000000u, 0x00000004u, 0x00002000u, 0x00003DFCu
    };
    static const uint32_t misaligned[] = {
        0x00000001u, 0x00003DFDu,               /* misaligned AND below floor */
        0x00003E01u, 0x00003E02u, 0x00003E03u,
        0x00004001u, 0x0003E7FEu, 0xFFFFFFFFu
    };
    static const uint32_t above[] = {
        0x0003E800u, 0x00040000u, 0xFFFFFFFCu, 0x7FFFFFFCu
    };
    size_t i;

    g_group = "program-word-guards";
    fm_reset();

    for (i = 0u; i < sizeof below / sizeof below[0]; i++) {
        GUARD(BL_FLASH_ERR_RANGE, bl_flash_program_word(below[i], 0x12345678u));
    }
    for (i = 0u; i < sizeof misaligned / sizeof misaligned[0]; i++) {
        GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program_word(misaligned[i], 0u));
    }
    for (i = 0u; i < sizeof above / sizeof above[0]; i++) {
        GUARD(BL_FLASH_ERR_RANGE, bl_flash_program_word(above[i], 0u));
    }

    CHECK(fm_stat(FM_STAT_REG_WRITES) == 0u,
          "not one register write escaped the refused word programs");

    CHECK(bl_flash_program_word(BL_FLASH_WRITE_FLOOR, 0xAABBCCDDu)
          == BL_FLASH_OK, "0x3E00 is accepted");
    CHECK(bl_flash_program_word(BL_FLASH_END - 4u, 0x01020304u) == BL_FLASH_OK,
          "0x3E7FC, the last word of CodeFlash, is accepted");
    CHECK(mem32(BL_FLASH_END - 4u) == 0x01020304u, "and it landed");
    CHECK(!fm_unlocked(), "flash locked afterwards");
    CHECK(fm_bootloader_intact(), "bootloader intact");
}

/* ------------------------------------------------------------------ */
/* D. bl_flash_program guards                                          */
/* ------------------------------------------------------------------ */

static void test_program_guards(void)
{
    uint8_t pat[64];
    void *src;
    uint8_t *sram;
    uint8_t *outside;
    uint32_t i;

    g_group = "program-guards";
    fm_reset();

    for (i = 0u; i < sizeof pat; i++) pat[i] = prand(i);
    src = sram_load(pat, sizeof pat);
    sram = (uint8_t *)fm_sram();
    outside = (uint8_t *)fm_outside_sram();

    /* NULL / zero length */
    GUARD(BL_FLASH_ERR_PARAM, bl_flash_program(0x00004000u, 0, 4u));
    GUARD(BL_FLASH_ERR_PARAM, bl_flash_program(0x00004000u, src, 0u));
    GUARD(BL_FLASH_ERR_PARAM, bl_flash_program(0x00000000u, 0, 0u));

    /* Below the floor, including a range that straddles it */
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x00000000u, src, 4u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x00003DFCu, src, 4u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x00003DFCu, src, 8u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x00003C00u, src, 512u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x00000000u, src, 0x4000u));

    /* Past the end, and lengths that would overflow addr + len */
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x0003E800u, src, 4u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x0003E7FCu, src, 8u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x00040000u, src, 4u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x00003E00u, src, 0xFFFFFFFFu));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x00003E00u, src, 0xFFFFFFFCu));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0xFFFFFFFCu, src, 8u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0xFFFFFFFFu, src, 4u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_program(0x80000000u, src, 4u));

    /* Misaligned destination address (range-legal, so ALIGN not RANGE) */
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00003E01u, src, 4u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00003E02u, src, 4u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00003E03u, src, 4u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00004001u, src, 32u));

    /* Misaligned length — must be REJECTED, never rounded or
     * read-modify-written: programming cannot set a bit back to 1. */
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00004000u, src, 1u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00004000u, src, 2u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00004000u, src, 3u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00004000u, src, 5u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00004000u, src, 6u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00004000u, src, 7u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00004000u, src, 63u));
    GUARD(BL_FLASH_ERR_ALIGN, bl_flash_program(0x00004002u, src, 2u));

    /* Source outside the SRAM window.  These pointers are real, mapped and
     * readable, so a guard failure shows up as a wrong return code rather than
     * as a crash that could be mistaken for a harness bug. */
    CHECK((uint32_t)(uintptr_t)outside + FM_SRAM_SIZE == fm_sram_lo(),
          "the outside-SRAM probe really is adjacent and below the window");
    GUARD(BL_FLASH_ERR_SRC, bl_flash_program(0x00004000u, outside, 4u));
    GUARD(BL_FLASH_ERR_SRC, bl_flash_program(0x00004000u, outside + 0x7FFCu, 4u));
    GUARD(BL_FLASH_ERR_SRC,
          bl_flash_program(0x00004000u, sram + FM_SRAM_SIZE, 4u));
    GUARD(BL_FLASH_ERR_SRC,               /* straddles the top of SRAM */
          bl_flash_program(0x00004000u, sram + FM_SRAM_SIZE - 4u, 8u));
    GUARD(BL_FLASH_ERR_SRC,
          bl_flash_program(0x00004000u, sram + FM_SRAM_SIZE - 4u, 64u));

    CHECK(fm_stat(FM_STAT_REG_WRITES) == 0u,
          "not one register write escaped the refused bl_flash_program calls");

    /* Sources that must be ACCEPTED: the last word of SRAM, and an unaligned
     * source (the driver assembles byte-wise on purpose). */
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_OK, "setup erase");
    memcpy(sram + FM_SRAM_SIZE - 4u, pat, 4u);
    CHECK(bl_flash_program(0x00004000u, sram + FM_SRAM_SIZE - 4u, 4u)
          == BL_FLASH_OK, "a source ending exactly at the top of SRAM is fine");
    memcpy(sram + 1u, pat, 32u);
    CHECK(bl_flash_program(0x00004020u, sram + 1u, 32u) == BL_FLASH_OK,
          "an unaligned source is fine (assembled byte-wise)");
    CHECK(memcmp(fm_mem() + 0x4020u, pat, 32u) == 0,
          "and the bytes landed in order");
    CHECK(!fm_unlocked(), "flash locked afterwards");
    CHECK(fm_bootloader_intact(), "bootloader intact");
}

/* ------------------------------------------------------------------ */
/* E. blank-check guards and boundaries                                */
/* ------------------------------------------------------------------ */

static void test_blank_check(void)
{
    uint8_t buf[8];
    void *src;

    g_group = "blank-check";
    fm_reset();

    /* Guards.  bl_flash_check_erased has no controller traffic at all, so the
     * GUARD macro's "no register writes" arm is trivially satisfied; the
     * return code and the predicate form are what matter here. */
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0x00000000u, 512u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0x00003DFFu, 1u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0x00003DFCu, 8u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0x00003E00u, 0u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0x0003E800u, 4u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0x0003E7FFu, 2u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0x00003E00u, 0xFFFFFFFFu));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0xFFFFFFFCu, 8u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0xFFFFFFFFu, 1u));
    GUARD(BL_FLASH_ERR_RANGE, bl_flash_check_erased(0x00040000u, 4u));

    /* The predicate form must answer EXACTLY 0 for every one of those — never
     * a negative code, which would read as TRUE in an `if`. */
    CHECK(bl_flash_is_erased(0x00000000u, 512u) == 0,
          "is_erased(below floor) == 0 exactly");
    CHECK(bl_flash_is_erased(0x00003E00u, 0u) == 0,
          "is_erased(len 0) == 0 exactly");
    CHECK(bl_flash_is_erased(0xFFFFFFFFu, 1u) == 0,
          "is_erased(wrapping) == 0 exactly");
    CHECK(bl_flash_is_erased(0x0003E7FFu, 2u) == 0,
          "is_erased(running off the end) == 0 exactly");

    /* Boundaries on real content. */
    CHECK(bl_flash_check_erased(0x00003E00u, BL_FLASH_END - 0x3E00u)
          == BL_FLASH_OK, "the whole writable region is blank after init");
    CHECK(bl_flash_is_erased(0x0003E7FFu, 1u) == 1,
          "the very last byte is blank, byte path, == 1 exactly");
    CHECK(bl_flash_check_erased(0x00003E01u, 3u) == BL_FLASH_OK,
          "an unaligned range takes the byte path and passes");

    memset(buf, 0x5Au, sizeof buf);
    src = sram_load(buf, sizeof buf);
    CHECK(bl_flash_program(0x00004100u, src, 4u) == BL_FLASH_OK, "setup");

    CHECK(bl_flash_check_erased(0x00004000u, 512u) == BL_FLASH_ERR_VERIFY,
          "a sector containing one programmed word is not blank");
    CHECK(bl_flash_is_erased(0x00004000u, 512u) == 0, "predicate agrees, == 0");
    CHECK(bl_flash_check_erased(0x00004000u, 256u) == BL_FLASH_OK,
          "the part below the programmed word is still blank");
    CHECK(bl_flash_check_erased(0x00004104u, 508u) == BL_FLASH_OK,
          "the part above it is still blank");
    CHECK(bl_flash_check_erased(0x00004100u, 4u) == BL_FLASH_ERR_VERIFY,
          "exactly the programmed word is not blank");
    CHECK(bl_flash_check_erased(0x000040FFu, 1u) == BL_FLASH_OK,
          "the byte immediately below is blank");
    CHECK(bl_flash_check_erased(0x000040FFu, 2u) == BL_FLASH_ERR_VERIFY,
          "a byte-path range that just clips it is not blank");
    CHECK(bl_flash_check_erased(0x00004104u, 1u) == BL_FLASH_OK,
          "the byte immediately above is blank");

    /* Across the boot-info / application sector boundary. */
    CHECK(bl_flash_check_erased(0x00003FFEu, 4u) == BL_FLASH_OK,
          "a range spanning 0x3FFF..0x4000 is blank");
    CHECK(bl_flash_program_word(0x00003FFCu, 0u) == BL_FLASH_OK, "setup");
    CHECK(bl_flash_check_erased(0x00003FFEu, 4u) == BL_FLASH_ERR_VERIFY,
          "and stops being blank when the sector below is written");
    CHECK(bl_flash_check_erased(0x00004000u, 4u) == BL_FLASH_OK,
          "without contaminating the sector above");

    CHECK(fm_stat(FM_STAT_READ_OOR) == 0u, "no read left the 250 KB array");
    CHECK(fm_stat(FM_STAT_READ_BELOW_FLOOR) == 0u,
          "no read touched the bootloader's own sectors");
    CHECK(fm_stat(FM_STAT_READ_MISALIGNED) == 0u,
          "no unaligned word read (ARMv6-M would fault)");
    CHECK(fm_bootloader_intact(), "bootloader intact");
}

/* ------------------------------------------------------------------ */
/* F. Programming and erase semantics                                  */
/* ------------------------------------------------------------------ */

static void test_semantics(void)
{
    uint8_t buf[1024];
    void *src;
    uint32_t i;

    g_group = "semantics";
    fm_reset();

    /* AND semantics, in the four cases that matter. */
    CHECK(bl_flash_program_word(0x00004000u, 0x0F0F0F0Fu) == BL_FLASH_OK,
          "program into erased flash");
    CHECK(mem32(0x4000u) == 0x0F0F0F0Fu, "value landed");
    CHECK(bl_flash_program_word(0x00004000u, 0x0F0F0F0Fu) == BL_FLASH_OK,
          "re-programming the SAME value succeeds (no erase needed)");
    CHECK(bl_flash_program_word(0x00004000u, 0x0F0F0F00u) == BL_FLASH_OK,
          "programming a bit-SUBSET succeeds");
    CHECK(mem32(0x4000u) == 0x0F0F0F00u, "the subset landed");
    CHECK(bl_flash_program_word(0x00004000u, 0xFFFFFFFFu)
          == BL_FLASH_ERR_VERIFY,
          "programming 0xFFFFFFFF over non-blank flash fails the read-back");
    CHECK(mem32(0x4000u) == 0x0F0F0F00u,
          "and the AND left the contents unchanged");
    CHECK(bl_flash_program_word(0x00004000u, 0x10000000u)
          == BL_FLASH_ERR_VERIFY, "so does setting a bit that reads 0");
    CHECK(mem32(0x4000u) == 0x00000000u,
          "though the AND still happened — 0x0F0F0F00 & 0x10000000 == 0");
    CHECK(!fm_unlocked(), "locked after a verify failure");

    /* Erase is precisely bounded when driven through the API. */
    fm_reset();
    fm_fill(0x00004000u, 0x00u, 0x600u);
    CHECK(bl_flash_erase_sector(0x00004200u) == BL_FLASH_OK, "erase middle");
    CHECK(all_bytes(0x4200u, 512u, 0xFF), "512 bytes blanked");
    CHECK(fm_mem()[0x41FFu] == 0x00u, "the byte below is untouched");
    CHECK(fm_mem()[0x4400u] == 0x00u, "the byte above is untouched");

    /* Programming across a sector boundary. */
    fm_reset();
    for (i = 0u; i < sizeof buf; i++) buf[i] = prand(i + 7u);
    src = sram_load(buf, 8u);
    CHECK(bl_flash_program(0x000041FCu, src, 8u) == BL_FLASH_OK,
          "an 8-byte write across 0x4200 succeeds");
    CHECK(memcmp(fm_mem() + 0x41FCu, buf, 8u) == 0, "and lands byte-exactly");

    /* A realistic block: two sectors erased, 1 KB programmed. */
    fm_reset();
    CHECK(bl_flash_erase_sector(0x00008000u) == BL_FLASH_OK, "erase 1");
    CHECK(bl_flash_erase_sector(0x00008200u) == BL_FLASH_OK, "erase 2");
    src = sram_load(buf, sizeof buf);
    CHECK(bl_flash_program(0x00008000u, src, sizeof buf) == BL_FLASH_OK,
          "1 KB programmed");
    CHECK(memcmp(fm_mem() + 0x8000u, buf, sizeof buf) == 0,
          "1 KB read back byte-identical");
    CHECK(fm_stat(FM_STAT_PROGRAMS) == sizeof buf / 4u,
          "exactly %u program commands were issued, saw %u",
          (unsigned)(sizeof buf / 4u), fm_stat(FM_STAT_PROGRAMS));
    CHECK(!fm_unlocked(), "locked afterwards");
    /* Each flash_command() unlocks and re-locks; bl_flash_program() then locks
     * once more on the way out.  Counting proves that final belt-and-braces
     * lock is really executed and not just present in the source. */
    CHECK(fm_stat(FM_STAT_LOCKS) == fm_stat(FM_STAT_UNLOCKS) + 1u,
          "one lock more than unlocks: the trailing bl_flash_lock() ran "
          "(%u locks, %u unlocks)",
          fm_stat(FM_STAT_LOCKS), fm_stat(FM_STAT_UNLOCKS));

    /* Programming over flash that is not blank: stops at the first bad word,
     * earlier words stay programmed.  This is the documented behaviour and the
     * reason a caller must erase first. */
    fm_reset();
    fm_fill(0x0000C010u, 0x00u, 4u);     /* one stale word inside a blank sector */
    src = sram_load(buf, 32u);
    CHECK(bl_flash_program(0x0000C000u, src, 32u) == BL_FLASH_ERR_VERIFY,
          "programming over a stale word fails the read-back");
    CHECK(memcmp(fm_mem() + 0xC000u, buf, 16u) == 0,
          "the four words before it were programmed");
    CHECK(all_bytes(0x0000C014u, 12u, 0xFF),
          "and nothing after the failure was attempted");
    CHECK(fm_stat(FM_STAT_PROGRAMS) == 5u,
          "five program commands issued (four good, one that ANDed to nothing), saw %u",
          fm_stat(FM_STAT_PROGRAMS));
    CHECK(!fm_unlocked(), "locked after a mid-buffer failure");
    CHECK(fm_bootloader_intact(), "bootloader intact");
}

/* ------------------------------------------------------------------ */
/* G. Fault injection                                                  */
/* ------------------------------------------------------------------ */

static void test_faults(void)
{
    uint8_t buf[16];
    void *src;
    uint32_t i;

    g_group = "faults";

    for (i = 0u; i < sizeof buf; i++) buf[i] = (uint8_t)(0xA0u + i);

    /* --- erase reports a controller error --- */
    fm_reset();
    fm_fill(0x00004000u, 0x00u, 512u);
    fm_inject(FM_FAULT_ERASE_STATUS, 1u, 0x02u);          /* CMD_ERR */
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_ERR_STATUS,
          "erase failure is reported, not swallowed");
    CHECK(all_bytes(0x4000u, 512u, 0x00), "and nothing was erased");
    CHECK(!fm_unlocked(), "locked after a failed erase");
    CHECK(fm_stat(FM_STAT_INJECTED) == 1u, "the fault was delivered");

    /* TOUT alongside ADDR_OK — the other half of flash.c's first status test */
    fm_reset();
    fm_inject(FM_FAULT_ERASE_STATUS, 1u, 0x41u);          /* ADDR_OK | TOUT */
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_ERR_STATUS,
          "TOUT is reported even with ADDR_OK set");

    /* No error bit, but not the success signature either — flash.c's SECOND
     * status test, the one that would be dead code without this. */
    fm_reset();
    fm_inject(FM_FAULT_ERASE_STATUS, 1u, 0x00u);
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_ERR_STATUS,
          "a cleared ADDR_OK with no error bit is still a failure");
    fm_reset();
    fm_inject(FM_FAULT_ERASE_STATUS, 1u, 0x60u);          /* ADDR_OK | junk */
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_ERR_STATUS,
          "an unexpected status bit is a failure, not a success");

    /* --- erase claims success but did not fully take --- */
    fm_reset();
    fm_inject(FM_FAULT_ERASE_SILENT, 1u, 0u);
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_ERR_VERIFY,
          "a silently incomplete erase is caught by the post-check");
    CHECK(!fm_unlocked(), "locked after a failed erase verify");

    /* --- program reports a controller error --- */
    fm_reset();
    fm_inject(FM_FAULT_PROG_STATUS, 1u, 0x02u);
    CHECK(bl_flash_program_word(0x00004000u, 0x11223344u)
          == BL_FLASH_ERR_STATUS, "program failure is reported");
    CHECK(mem32(0x4000u) == 0xFFFFFFFFu, "and nothing was written");
    CHECK(!fm_unlocked(), "locked after a failed program");

    /* --- program claims success but did not take --- */
    fm_reset();
    fm_inject(FM_FAULT_PROG_SILENT, 1u, 0u);
    CHECK(bl_flash_program_word(0x00004000u, 0x11223344u)
          == BL_FLASH_ERR_VERIFY, "a silent program failure fails read-back");
    CHECK(mem32(0x4000u) == 0xFFFFFFFFu, "and the word is still blank");

    /* --- the unlock does not take: the command runs against locked flash --- */
    fm_reset();
    fm_fill(0x00004000u, 0x00u, 512u);
    fm_inject(FM_FAULT_UNLOCK_IGNORED, 1u, 0u);
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_ERR_STATUS,
          "a command against a locked controller is reported as a failure");
    CHECK(fm_stat(FM_STAT_CMD_WHILE_LOCKED) == 1u,
          "the model saw the command arrive locked");
    CHECK(all_bytes(0x4000u, 512u, 0x00), "and nothing was erased");
    CHECK(!fm_unlocked(), "still locked afterwards");

    /* --- the mandatory & 0xFF status mask --- */
    fm_reset();
    fm_inject(FM_FAULT_STATUS_NOISE, 1u, 0xFCu);   /* junk in bits 15..10 */
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_OK,
          "high status bits are masked off, not misread as a failure");
    CHECK((fm_status() & 0xFF00u) != 0u,
          "the model really did present junk in the high half (0x%04X)",
          fm_status());

    /* --- bl_flash_program stops at the first failing word --- */
    fm_reset();
    src = sram_load(buf, sizeof buf);
    fm_inject(FM_FAULT_PROG_STATUS, 3u, 0x02u);
    CHECK(bl_flash_program(0x00004000u, src, sizeof buf)
          == BL_FLASH_ERR_STATUS, "the failure propagates out of the loop");
    CHECK(memcmp(fm_mem() + 0x4000u, buf, 8u) == 0,
          "the two words before the fault are programmed");
    CHECK(all_bytes(0x00004008u, 8u, 0xFF),
          "the failing word and everything after it are untouched");
    CHECK(fm_stat(FM_STAT_PROGRAMS) == 2u,
          "only two program commands completed, saw %u",
          fm_stat(FM_STAT_PROGRAMS));
    CHECK(!fm_unlocked(), "locked after a mid-buffer controller failure");
    CHECK(fm_stat(FM_STAT_UNLOCKS) == 3u && fm_stat(FM_STAT_LOCKS) == 4u,
          "the ERROR path's extra bl_flash_lock() ran too "
          "(%u unlocks, %u locks; want 3 and 4)",
          fm_stat(FM_STAT_UNLOCKS), fm_stat(FM_STAT_LOCKS));
    CHECK(fm_bootloader_intact(), "bootloader intact through the fault sweep");
}

/* ------------------------------------------------------------------ */
/* H. Lock and interrupt invariants                                    */
/* ------------------------------------------------------------------ */

static void test_lock_and_irq(void)
{
    uint8_t buf[16];
    void *src;
    uint32_t i;

    g_group = "lock-irq";
    fm_reset();

    for (i = 0u; i < sizeof buf; i++) buf[i] = (uint8_t)i;
    src = sram_load(buf, sizeof buf);

    /* Every exit path leaves the controller locked — success, every error
     * class, and the explicit lock call. */
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_OK, "ok");
    CHECK(!fm_unlocked(), "locked after a successful erase");
    CHECK(bl_flash_erase_sector(0x00000000u) == BL_FLASH_ERR_RANGE, "range");
    CHECK(!fm_unlocked(), "locked after a range rejection");
    CHECK(bl_flash_erase_sector(0x00004001u) == BL_FLASH_ERR_ALIGN, "align");
    CHECK(!fm_unlocked(), "locked after an alignment rejection");
    CHECK(bl_flash_program(0x00004000u, 0, 4u) == BL_FLASH_ERR_PARAM, "param");
    CHECK(!fm_unlocked(), "locked after a param rejection");
    CHECK(bl_flash_program(0x00004000u, fm_outside_sram(), 4u)
          == BL_FLASH_ERR_SRC, "src");
    CHECK(!fm_unlocked(), "locked after a source rejection");
    CHECK(bl_flash_program(0x00004000u, src, sizeof buf) == BL_FLASH_OK, "ok");
    CHECK(!fm_unlocked(), "locked after a successful program");
    CHECK(bl_flash_program_word(0x00004000u, 0xFFFFFFFFu)
          == BL_FLASH_ERR_VERIFY, "verify");
    CHECK(!fm_unlocked(), "locked after a verify failure");
    fm_w8(FM_PROTECT, 0x88u);
    CHECK(fm_unlocked(), "deliberately unlocked");
    bl_flash_lock();
    CHECK(!fm_unlocked(), "bl_flash_lock() locks");
    CHECK(fm_protect_readback() == 0x00u, "and writes the lock value");

    /* PRIMASK is saved and restored, not blindly re-enabled. */
    fm_reset();
    CHECK(!fm_irq_masked(), "interrupts start enabled");
    CHECK(bl_flash_erase_sector(0x00004000u) == BL_FLASH_OK, "erase");
    CHECK(!fm_irq_masked(), "interrupts re-enabled after the unlocked window");

    fm_cpsid();
    CHECK(bl_flash_erase_sector(0x00004200u) == BL_FLASH_OK, "erase");
    CHECK(fm_irq_masked(),
          "a caller that had interrupts masked still has them masked");
    fm_cpsie();

    CHECK(fm_stat(FM_STAT_CMD_IRQ_UNMASKED) == 0u,
          "every command ran inside the interrupt-masked window");
    CHECK(fm_stat(FM_STAT_DATAFLASH_WE) == 0u,
          "RB_ROM_DATA_WE (0x8C) was never written — InfoFlash stays shut");
    CHECK(fm_stat(FM_STAT_BAD_PROTECT) == 0u,
          "only 0x80 and 0x88 were ever written to R8_FLASH_PROTECT");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_model();
    test_erase_guards();
    test_program_word_guards();
    test_program_guards();
    test_blank_check();
    test_semantics();
    test_faults();
    test_lock_and_irq();

    g_group = "run-wide";
    fm_reset();     /* fold the last group's counters into the totals */

    CHECK(acc_total(FM_STAT_CMD_BELOW_FLOOR) == 0u,
          "no command EVER reached the controller below 0x3E00 (%u)",
          acc_total(FM_STAT_CMD_BELOW_FLOOR));
    CHECK(acc_total(FM_STAT_CMD_MISALIGNED) == 0u,
          "no misaligned command ever reached the controller (%u)",
          acc_total(FM_STAT_CMD_MISALIGNED));
    CHECK(acc_total(FM_STAT_DATAFLASH_WE) == 0u,
          "RB_ROM_DATA_WE never set (%u)", acc_total(FM_STAT_DATAFLASH_WE));
    CHECK(acc_total(FM_STAT_BAD_PROTECT) == 0u,
          "no unexpected R8_FLASH_PROTECT value (%u)",
          acc_total(FM_STAT_BAD_PROTECT));
    CHECK(acc_total(FM_STAT_CMD_IRQ_UNMASKED) == 0u,
          "no command ran with interrupts enabled (%u)",
          acc_total(FM_STAT_CMD_IRQ_UNMASKED));
    CHECK(acc_total(FM_STAT_READ_OOR) == 0u,
          "no read outside the 250 KB array (%u)", acc_total(FM_STAT_READ_OOR));
    CHECK(acc_total(FM_STAT_READ_BELOW_FLOOR) == 0u,
          "no read below the write floor (%u)",
          acc_total(FM_STAT_READ_BELOW_FLOOR));
    CHECK(acc_total(FM_STAT_READ_MISALIGNED) == 0u,
          "no unaligned word read (%u)", acc_total(FM_STAT_READ_MISALIGNED));
    CHECK(acc_total(FM_STAT_BAD_CMD) == 0u,
          "no unrecognised opcode issued (%u)", acc_total(FM_STAT_BAD_CMD));
    CHECK(acc_total(FM_STAT_CMD_OOR) == 0u,
          "no command aimed past the end of CodeFlash (%u)",
          acc_total(FM_STAT_CMD_OOR));
    CHECK(fm_bootloader_intact(), "bootloader region intact at exit");
    CHECK(g_guard_refused == g_guard_attempts,
          "flash.c refused all %u guard violations, refused %u",
          g_guard_attempts, g_guard_refused);

    printf("test_flash: %d assertions / %d executions, %d failures\n",
           g_nsites, g_pass + g_fail, g_fail);
    printf("test_flash: %u guard violations attempted, %u correctly refused\n",
           g_guard_attempts, g_guard_refused);
    printf("test_flash: %u erases, %u word programs carried out by the model\n",
           acc_total(FM_STAT_ERASES), acc_total(FM_STAT_PROGRAMS));

    if (g_fail != 0) {
        printf("test_flash: FAILED\n");
        return 1;
    }
    printf("test_flash: OK\n");
    return 0;
}
