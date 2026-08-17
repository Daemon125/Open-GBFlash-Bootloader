/* flash_model.c — register-level CH579 CodeFlash controller model.
 * See flash_model.h for what is modelled, why, and how src/flash.c is
 * redirected onto it.
 *
 * The model is deliberately NOT protective.  It has no write floor, no
 * alignment requirement it refuses on, and no idea what a bootloader is.  It
 * does what the silicon does and counts what it was asked to do, so that every
 * safety property under test belongs to src/flash.c and not to the harness.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_model.h"

/* ---- state --------------------------------------------------------------- */

static uint8_t  fm_mem_arr[FM_FLASH_SIZE];

static uint32_t fm_reg_data;
static uint32_t fm_reg_addr;
static uint8_t  fm_reg_cmd;
static uint8_t  fm_reg_prot_written;
static uint8_t  fm_reg_prot_readback;
static uint16_t fm_reg_status;

static int      fm_code_we;          /* code-flash write enable latched      */
static int      fm_irq_mask;         /* model PRIMASK                        */

static uint32_t fm_counters[FM_STAT_NCOUNTERS];

/* per-kind ordinals, for "the Nth erase" style injection */
static uint32_t fm_n_erase_cmd;
static uint32_t fm_n_prog_cmd;
static uint32_t fm_n_unlock;
static uint32_t fm_n_cmd;

static fm_fault_t fm_fault;
static uint32_t   fm_fault_nth;
static uint8_t    fm_fault_status;

/* ---- tripwire pattern ---------------------------------------------------- */

/* Never 0xFF, so an erase below the floor is always visible: an all-0xFF fill
 * would make the single most dangerous bug in this driver undetectable. */
static uint8_t fm_pattern(uint32_t i)
{
    uint8_t b = (uint8_t)(0x5Au ^ (i * 31u) ^ (i >> 8));
    return (b == 0xFFu) ? 0x5Au : b;
}

void fm_init(void)
{
    uint32_t i;

    memset(fm_mem_arr, 0xFF, sizeof fm_mem_arr);
    for (i = 0u; i < FM_WRITE_FLOOR; i++) {
        fm_mem_arr[i] = fm_pattern(i);
    }

    fm_reg_data = 0u;
    fm_reg_addr = 0u;
    fm_reg_cmd = 0u;
    fm_reg_prot_written = FM_PROT_LOCK;
    fm_reg_prot_readback = 0u;
    fm_reg_status = FM_ST_ALWAYS_SET;
    fm_code_we = 0;
    fm_irq_mask = 0;

    memset(fm_counters, 0, sizeof fm_counters);
    fm_n_erase_cmd = fm_n_prog_cmd = fm_n_unlock = fm_n_cmd = 0u;
    fm_fault = FM_FAULT_NONE;
    fm_fault_nth = 0u;
    fm_fault_status = 0u;
}

void fm_init_blank(void)
{
    fm_init();
    memset(fm_mem_arr, 0xFF, sizeof fm_mem_arr);
}

int fm_bootloader_intact(void)
{
    uint32_t i;
    for (i = 0u; i < FM_WRITE_FLOOR; i++) {
        if (fm_mem_arr[i] != fm_pattern(i)) {
            return 0;
        }
    }
    return 1;
}

void fm_poke(uint32_t addr, const void *buf, uint32_t len)
{
    if (len == 0u || addr >= FM_FLASH_SIZE || len > FM_FLASH_SIZE - addr) {
        fprintf(stderr, "fm_poke: bad range %08x+%u\n", addr, len);
        abort();
    }
    memcpy(fm_mem_arr + addr, buf, len);
}

void fm_fill(uint32_t addr, uint8_t val, uint32_t len)
{
    if (len == 0u || addr >= FM_FLASH_SIZE || len > FM_FLASH_SIZE - addr) {
        fprintf(stderr, "fm_fill: bad range %08x+%u\n", addr, len);
        abort();
    }
    memset(fm_mem_arr + addr, val, len);
}

const uint8_t *fm_mem(void) { return fm_mem_arr; }

uint32_t fm_stat(int which)
{
    if (which < 0 || which >= FM_STAT_NCOUNTERS) return 0u;
    return fm_counters[which];
}

void fm_stat_reset(void) { memset(fm_counters, 0, sizeof fm_counters); }

int      fm_unlocked(void)         { return fm_code_we; }
uint8_t  fm_protect_readback(void) { return fm_reg_prot_readback; }
int      fm_irq_masked(void)       { return fm_irq_mask; }
uint16_t fm_status(void)           { return fm_reg_status; }

/* ---- fault injection ----------------------------------------------------- */

void fm_inject(fm_fault_t what, uint32_t nth, uint8_t status_low)
{
    fm_fault = (nth == 0u) ? FM_FAULT_NONE : what;
    fm_fault_nth = nth;
    fm_fault_status = status_low;
}

static int fm_fires(fm_fault_t what, uint32_t ordinal)
{
    if (fm_fault != what || fm_fault_nth == 0u || ordinal != fm_fault_nth) {
        return 0;
    }
    fm_counters[FM_STAT_INJECTED]++;
    return 1;
}

static uint8_t fm_fault_low(void)
{
    return (fm_fault_status != 0u) ? fm_fault_status : (uint8_t)FM_ST_CMD_ERR;
}

/* ---- command execution --------------------------------------------------- */

/* Runs inside the write to R8_FLASH_COMMAND: the core is halted by hardware for
 * the whole operation, so by the time the next instruction reads the status
 * register the result is already final.  No busy bit, no polling loop. */
static void fm_execute(uint8_t cmd)
{
    uint32_t addr = fm_reg_addr;
    uint16_t low;

    fm_n_cmd++;

    if (!fm_irq_mask) {
        /* flash.c masks interrupts around the unlocked window; if that ever
         * stops being true the unlock becomes observable by other code. */
        fm_counters[FM_STAT_CMD_IRQ_UNMASKED]++;
    }

    if (!fm_code_we) {
        fm_counters[FM_STAT_CMD_WHILE_LOCKED]++;
        low = FM_ST_CMD_ERR;
        goto done;
    }

    if (cmd != FM_CMD_ERASE && cmd != FM_CMD_PROG) {
        fm_counters[FM_STAT_BAD_CMD]++;
        low = FM_ST_CMD_ERR;
        goto done;
    }

    if (addr >= FM_FLASH_SIZE) {
        /* RB_ROM_ADDR_OK stays clear: no error bit, but not the success
         * signature either, which is flash.c's second status test. */
        fm_counters[FM_STAT_CMD_OOR]++;
        low = 0u;
        goto done;
    }
    if (addr < FM_WRITE_FLOOR) {
        /* The silicon does this happily.  Counting it is the harness's job. */
        fm_counters[FM_STAT_CMD_BELOW_FLOOR]++;
    }

    if (cmd == FM_CMD_ERASE) {
        uint32_t base = addr & ~(FM_SECTOR - 1u);

        fm_n_erase_cmd++;
        if (addr != base) {
            fm_counters[FM_STAT_CMD_MISALIGNED]++;
        }
        if (fm_fires(FM_FAULT_ERASE_STATUS, fm_n_erase_cmd)) {
            low = fm_fault_low();
            goto done;
        }
        if (base <= FM_FLASH_SIZE - FM_SECTOR) {
            memset(fm_mem_arr + base, 0xFF, FM_SECTOR);
            if (fm_fires(FM_FAULT_ERASE_SILENT, fm_n_erase_cmd)) {
                /* Erase reports success but one byte did not come back.  The
                 * exact failure bl_flash_erase_sector()'s post-check exists
                 * for. */
                fm_mem_arr[base + FM_SECTOR - 1u] = 0x00u;
            }
            fm_counters[FM_STAT_ERASES]++;
        }
        low = FM_ST_ADDR_OK;
        goto done;
    }

    /* ROM_CMD_PROG: one 32-bit word, little-endian, ANDed into place. */
    {
        uint32_t base = addr & ~3u;
        uint32_t i;

        fm_n_prog_cmd++;
        if (addr != base) {
            fm_counters[FM_STAT_CMD_MISALIGNED]++;
        }
        if (fm_fires(FM_FAULT_PROG_STATUS, fm_n_prog_cmd)) {
            low = fm_fault_low();
            goto done;
        }
        if (base <= FM_FLASH_SIZE - 4u
            && !fm_fires(FM_FAULT_PROG_SILENT, fm_n_prog_cmd)) {
            for (i = 0u; i < 4u; i++) {
                /* Programming can only clear bits. */
                fm_mem_arr[base + i] &= (uint8_t)(fm_reg_data >> (8u * i));
            }
            fm_counters[FM_STAT_PROGRAMS]++;
        }
        low = FM_ST_ADDR_OK;
    }

done:
    fm_reg_status = (uint16_t)(low | FM_ST_ALWAYS_SET);
    if (fm_fires(FM_FAULT_STATUS_NOISE, fm_n_cmd)) {
        fm_reg_status |= (uint16_t)((uint16_t)fm_fault_status << 8);
    }
}

/* ---- register file ------------------------------------------------------- */

static void fm_write_protect(uint8_t v)
{
    fm_reg_prot_written = v;
    /* RB_ROM_WE_MUST_10 (0x80) is WRITE-ONLY: 0x88 reads back as 0x08. */
    fm_reg_prot_readback = (uint8_t)(v & 0x7Fu);

    if (v & FM_PROT_DATA_WE) {
        /* 0x8C would arm InfoFlash, where CFG_BOOT_EN lives.  Never. */
        fm_counters[FM_STAT_DATAFLASH_WE]++;
    }
    if (v != FM_PROT_LOCK && v != (uint8_t)(FM_PROT_LOCK | FM_PROT_CODE_WE)) {
        fm_counters[FM_STAT_BAD_PROTECT]++;
    }

    if ((v & FM_PROT_LOCK) && (v & FM_PROT_CODE_WE)) {
        fm_n_unlock++;
        if (fm_fires(FM_FAULT_UNLOCK_IGNORED, fm_n_unlock)) {
            fm_code_we = 0;             /* the unlock silently did not take */
        } else {
            fm_code_we = 1;
            fm_counters[FM_STAT_UNLOCKS]++;
        }
    } else {
        fm_code_we = 0;
        fm_counters[FM_STAT_LOCKS]++;
    }
}

void fm_w32(int reg, uint32_t v)
{
    fm_counters[FM_STAT_REG_WRITES]++;
    switch (reg) {
    case FM_DATA: fm_reg_data = v; break;
    case FM_ADDR: fm_reg_addr = v; break;
    default:
        fprintf(stderr, "fm_w32: bad register %d\n", reg);
        abort();
    }
}

void fm_w8(int reg, uint8_t v)
{
    fm_counters[FM_STAT_REG_WRITES]++;
    switch (reg) {
    case FM_PROTECT: fm_write_protect(v); break;
    case FM_CMD:     fm_reg_cmd = v; fm_execute(v); break;
    default:
        fprintf(stderr, "fm_w8: bad register %d\n", reg);
        abort();
    }
}

uint16_t fm_r16(int reg)
{
    if (reg != FM_STATUS) {
        fprintf(stderr, "fm_r16: bad register %d\n", reg);
        abort();
    }
    return fm_reg_status;
}

uint32_t fm_r32(int reg)
{
    switch (reg) {
    case FM_DATA: return fm_reg_data;
    case FM_ADDR: return fm_reg_addr;
    case FM_CMD:  return fm_reg_cmd;
    default:
        fprintf(stderr, "fm_r32: bad register %d\n", reg);
        abort();
    }
}

/* ---- flash reads --------------------------------------------------------- */

static void fm_note_read(uint32_t addr)
{
    if (addr < FM_WRITE_FLOOR) {
        fm_counters[FM_STAT_READ_BELOW_FLOOR]++;
    }
}

uint32_t fm_rd32(uint32_t addr)
{
    fm_counters[FM_STAT_READS32]++;
    fm_note_read(addr);
    if (addr & 3u) {
        /* ARMv6-M has no unaligned word load; on the device this would fault. */
        fm_counters[FM_STAT_READ_MISALIGNED]++;
    }
    if (addr > FM_FLASH_SIZE - 4u || addr >= FM_FLASH_SIZE) {
        fm_counters[FM_STAT_READ_OOR]++;
        return 0xFFFFFFFFu;
    }
    return (uint32_t)fm_mem_arr[addr]
         | ((uint32_t)fm_mem_arr[addr + 1u] << 8)
         | ((uint32_t)fm_mem_arr[addr + 2u] << 16)
         | ((uint32_t)fm_mem_arr[addr + 3u] << 24);
}

uint8_t fm_rd8(uint32_t addr)
{
    fm_counters[FM_STAT_READS8]++;
    fm_note_read(addr);
    if (addr >= FM_FLASH_SIZE) {
        fm_counters[FM_STAT_READ_OOR]++;
        return 0xFFu;
    }
    return fm_mem_arr[addr];
}

/* ---- core state ---------------------------------------------------------- */

uint32_t fm_primask(void) { return fm_irq_mask ? 1u : 0u; }
void     fm_cpsid(void)   { fm_irq_mask = 1; }
void     fm_cpsie(void)   { fm_irq_mask = 0; }

/* ---- the modelled SRAM window -------------------------------------------- */

/* One 96 KB host allocation, used as
 *     [base,            base + 32 KB)  outside-below  (real, readable)
 *     [base + 32 KB,    base + 64 KB)  the modelled SRAM
 *     [base + 64 KB,    base + 96 KB)  outside-above  (real, readable)
 * flash.c compares (uint32_t)pointer against fm_sram_lo()/fm_sram_hi(), so the
 * block must not straddle a 4 GB boundary or the truncated comparison would be
 * meaningless.  That is checked, with retries, rather than assumed. */

#define FM_BLOCK (3u * FM_SRAM_SIZE)

static uint8_t *fm_block;

static void fm_sram_setup(void)
{
    void *tries[64];
    int n = 0;
    int i;

    if (fm_block != 0) return;

    for (n = 0; n < 64; n++) {
        uint8_t *p = (uint8_t *)malloc(FM_BLOCK);
        uintptr_t u;

        if (p == 0) break;
        tries[n] = p;
        u = (uintptr_t)p;
        if ((u & 0xFFFFFFFFu) + (uintptr_t)FM_BLOCK <= 0xFFFFFFFFu) {
            fm_block = p;
            memset(fm_block, 0xA5, FM_BLOCK);
            break;
        }
    }
    for (i = 0; i < n; i++) {
        if (tries[i] != (void *)fm_block) free(tries[i]);
    }
    if (fm_block == 0) {
        fprintf(stderr,
                "flash_model: could not obtain a 96 KB block that does not "
                "straddle a 4 GB boundary\n");
        abort();
    }
}

void *fm_sram(void)
{
    fm_sram_setup();
    return fm_block + FM_SRAM_SIZE;
}

void *fm_outside_sram(void)
{
    fm_sram_setup();
    return fm_block;
}

uint32_t fm_sram_lo(void)
{
    fm_sram_setup();
    return (uint32_t)(uintptr_t)(fm_block + FM_SRAM_SIZE);
}

uint32_t fm_sram_hi(void)
{
    return fm_sram_lo() + FM_SRAM_SIZE;
}
