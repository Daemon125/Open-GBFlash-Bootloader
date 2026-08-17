/* flash_stub.c — see flash_stub.h. */

#include <string.h>
#include "flash_stub.h"

static uint8_t  fs_flash[FS_FLASH_END];
static uint8_t  fs_shadow[FS_WRITE_FLOOR];   /* pristine copy of our own code */
static uint32_t fs_counters[FS_STAT_NCOUNTERS];

/* ---- fault injection state ---- */
static fs_fault_t fs_fault;
static uint32_t   fs_fault_nth;      /* 1-based; 0 = disarmed               */
static uint32_t   fs_n_erase_calls;  /* every call, accepted or not         */
static uint32_t   fs_n_prog_words;   /* words, not calls — matches the real
                                      * driver, which programs a word at a
                                      * time and stops at the first failure  */
static uint32_t   fs_n_read_calls;

void fs_inject(fs_fault_t what, uint32_t nth)
{
    fs_fault     = nth ? what : FS_FAULT_NONE;
    fs_fault_nth = nth;
}

/* Returns 1 when this call is the armed one, and disarms (one-shot). */
static int fs_fires(fs_fault_t what, uint32_t counter)
{
    if (fs_fault != what || fs_fault_nth == 0u) return 0;
    if (counter != fs_fault_nth) return 0;
    fs_fault     = FS_FAULT_NONE;
    fs_fault_nth = 0u;
    fs_counters[FS_STAT_INJECTED]++;
    return 1;
}

/* Deterministic filler standing in for the bootloader's own machine code.
 * 0xFF would be a poor stand-in: an AND of anything into 0xFF leaves a visible
 * change only if the source has zero bits, and a memset-style bug writing 0xFF
 * would be invisible. A pseudorandom pattern makes every stray write show. */
static void fs_fill_pattern(void)
{
    uint32_t s = 0x13579BDFu;
    uint32_t i;
    for (i = 0; i < FS_WRITE_FLOOR; i++) {
        s = s * 1664525u + 1013904223u;
        fs_flash[i] = (uint8_t)(s >> 24);
    }
    memcpy(fs_shadow, fs_flash, FS_WRITE_FLOOR);
}

void fs_init(void)
{
    memset(fs_counters, 0, sizeof fs_counters);
    memset(fs_flash + FS_WRITE_FLOOR, 0xFF, FS_FLASH_END - FS_WRITE_FLOOR);
    fs_fill_pattern();
    fs_fault         = FS_FAULT_NONE;
    fs_fault_nth     = 0u;
    fs_n_erase_calls = 0u;
    fs_n_prog_words  = 0u;
    fs_n_read_calls  = 0u;
}

int fs_bootloader_intact(void)
{
    return memcmp(fs_flash, fs_shadow, FS_WRITE_FLOOR) == 0;
}

uint32_t fs_stat(int which)
{
    if (which < 0 || which >= FS_STAT_NCOUNTERS) return 0;
    return fs_counters[which];
}

const uint8_t *fs_mem(void) { return fs_flash; }

/* Shared refusal path so every rejection is counted the same way. */
static int fs_refuse(int floor_hit)
{
    fs_counters[FS_STAT_VIOLATIONS]++;
    if (floor_hit) fs_counters[FS_STAT_FLOOR_HITS]++;
    return -1;
}

static int fs_erase(uint32_t addr)
{
    if (addr < FS_WRITE_FLOOR)                  return fs_refuse(1);
    if (addr >= FS_FLASH_END)                   return fs_refuse(0);
    if (addr & (FS_SECTOR - 1u))                return fs_refuse(0);
    if (addr + FS_SECTOR > FS_FLASH_END)        return fs_refuse(0);

    fs_n_erase_calls++;
    /* An erase that reports failure must also have changed nothing the caller
     * can rely on. Modelled as "nothing happened at all", which is the benign
     * end of what real silicon does; the point of the test is proto.c's
     * response, not the sector's exact contents. */
    if (fs_fires(FS_FAULT_ERASE, fs_n_erase_calls)) return -1;

    memset(fs_flash + addr, 0xFF, FS_SECTOR);
    fs_counters[FS_STAT_ERASES]++;
    return 0;
}

static int fs_program(uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t off;

    if (buf == 0 || len == 0u)                  return fs_refuse(0);
    if (addr < FS_WRITE_FLOOR)                  return fs_refuse(1);
    if (addr >= FS_FLASH_END)                   return fs_refuse(0);
    if (len > FS_FLASH_END - addr)              return fs_refuse(0);
    if (addr & 3u)                              return fs_refuse(0);
    if (len & 3u)                               return fs_refuse(0);
    /* The SOURCE is deliberately NOT alignment-checked. src/flash.c assembles
     * every word byte-wise precisely so "the caller's buffer needs no
     * alignment" (flash.c:229); a stub that refused an unaligned source would
     * be stricter than the silicon and would flag a caller the real device
     * accepts. What the driver DOES require is that the source be RAM, never
     * flash (BL_FLASH_ERR_SRC), because programming reads the source while the
     * flash controller owns the array. Model that by refusing any source that
     * aliases the modelled CodeFlash. */
    if (src >= fs_flash && src < fs_flash + FS_FLASH_END) return fs_refuse(0);

    /* Word at a time, verifying each word before moving on — exactly the shape
     * of bl_flash_program(), which calls bl_flash_program_word() in a loop and
     * returns at the first failure, leaving the later words untouched. A stub
     * that ANDed the whole range and only then verified would model a partial
     * failure wrongly. */
    for (off = 0u; off < len; off += 4u) {
        uint32_t i;
        fs_n_prog_words++;
        if (fs_fires(FS_FAULT_PROGRAM, fs_n_prog_words)) {
            /* The word refuses to take: contents unchanged, read-back verify
             * inside the driver fails, BL_FLASH_ERR_VERIFY. */
            fs_counters[FS_STAT_VERIFY_FAILS]++;
            return -1;
        }
        /* This is the whole point of the stub: AND, never assign. */
        for (i = 0; i < 4u; i++) fs_flash[addr + off + i] &= src[off + i];
        /* And then verify, exactly as bl_flash_program_word() does.
         * Programming over flash that was not erased first leaves bits that
         * should have been 1 still 0 — the real driver returns
         * BL_FLASH_ERR_VERIFY, so do the same. */
        for (i = 0; i < 4u; i++) {
            if (fs_flash[addr + off + i] != src[off + i]) {
                fs_counters[FS_STAT_VERIFY_FAILS]++;
                return -1;
            }
        }
    }

    fs_counters[FS_STAT_PROGRAMS]++;
    fs_counters[FS_STAT_PROG_BYTES] += len;
    return 0;
}

static int fs_read(uint32_t addr, void *buf, uint32_t len)
{
    if (buf == 0 || len == 0u)          return -1;
    if (addr >= FS_FLASH_END)           return -1;
    if (len > FS_FLASH_END - addr)      return -1;

    fs_n_read_calls++;
    if (fs_fires(FS_FAULT_READ, fs_n_read_calls)) return -1;

    memcpy(buf, fs_flash + addr, len);
    fs_counters[FS_STAT_READS]++;
    /* A read that succeeds but hands back the wrong bytes: the failure mode a
     * read-back check exists to catch. */
    if (fs_fires(FS_FAULT_READ_CORRUPT, fs_n_read_calls))
        ((uint8_t *)buf)[len - 1u] ^= 0x01u;
    return 0;
}

static const bl_flash_ops fs_ops_all = {
    fs_erase, fs_program, fs_read
};

static const bl_flash_ops fs_ops_nr = {
    fs_erase, fs_program, 0
};

const bl_flash_ops *fs_ops(void)        { return &fs_ops_all; }
const bl_flash_ops *fs_ops_noread(void) { return &fs_ops_nr;  }
