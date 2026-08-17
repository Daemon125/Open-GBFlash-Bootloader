/* flash.c — CH579 CodeFlash driver for the GBFlash update-mode bootloader.
 *
 * This is the code most likely to brick the device, so every guard below is a
 * hard check at the top of the entry point, not a caller contract.
 *
 * Sector arithmetic
 * -----------------
 *   0x3E00 / 512 = 31 exactly and 0x4000 / 512 = 32 exactly.
 *     sectors  0..30  = 0x0000..0x3DFF  bootloader code (15,872 B)  NEVER WRITTEN
 *     sector   31     = 0x3E00..0x3FFF  boot-info record, alone in its sector
 *     sectors 32..    = 0x4000..        application
 *   Because the write floor 0x3E00 is itself a sector boundary, no erase this
 *   driver will accept can reach a single byte of bootloader code, and the
 *   boot-info record can be rewritten without disturbing one instruction.
 *
 * Register sequence and status semantics are proven on the real device; see
 * docs/DESIGN.md §3.
 */

#include "flash.h"

/* ---- MMIO ---------------------------------------------------------------- */

#define R32_FLASH_DATA      (*(volatile uint32_t *)0x40001800u)
#define R32_FLASH_ADDR      (*(volatile uint32_t *)0x40001804u)
#define R8_FLASH_COMMAND    (*(volatile uint8_t  *)0x40001808u)
#define R8_FLASH_PROTECT    (*(volatile uint8_t  *)0x40001809u)
#define R16_FLASH_STATUS    (*(volatile uint16_t *)0x4000180Au)

#define ROM_CMD_PROG        0x9Au   /* program one 32-bit word, clears 1 -> 0 only */
#define ROM_CMD_ERASE       0xA6u   /* erase one 512-byte sector                   */

/* The ONLY two values this driver may ever write to R8_FLASH_PROTECT.
 * 0x8C would additionally enable RB_ROM_DATA_WE, which DS §4.4 makes a
 * precondition for touching InfoFlash — where CFG_BOOT_EN lives.  As long as at
 * most one WE bit is ever set, no bug here can disarm the H1/ISP recovery path.
 * (SDK-V1.0 writes 0x8C on every call.  It is not the reference used here.) */
#define FLASH_LOCK          0x80u                 /* RB_ROM_WE_MUST_10           */
#define FLASH_UNLOCK_CODE   0x88u                 /* | RB_ROM_CODE_WE (0x08)     */

/* R16_FLASH_STATUS, low byte only.  Bit 9 reads 1 permanently, so the mask to
 * 8 bits is mandatory — an unmasked compare against 0x40 can never succeed. */
#define RB_ROM_CMD_TOUT     0x01u
#define RB_ROM_CMD_ERR      0x02u
#define RB_ROM_ADDR_OK      0x40u
#define FLASH_STATUS_OK     RB_ROM_ADDR_OK        /* the exact success signature */

/* SRAM extent, for the source-pointer check in bl_flash_program(). */
#define BL_SRAM_BASE        0x20000000u
#define BL_SRAM_END         0x20008000u           /* exclusive */

/* ---- interrupt guard ------------------------------------------------------
 * The bootloader runs fully polled, so nothing should fire while flash is
 * unlocked.  Masking anyway costs two instructions and makes "flash is unlocked"
 * a window no other code can ever observe or extend.  It also means a stray
 * forwarded interrupt cannot land between the unlock and the re-lock.
 * (No wait loop is needed inside the window: the MCU is halted by hardware for
 * the duration of the erase/program, so masking cannot lengthen the blackout.)
 */

static inline uint32_t irq_lock(void)
{
    uint32_t primask;
    __asm volatile ("mrs %0, primask" : "=r" (primask));
    __asm volatile ("cpsid i" ::: "memory");
    return primask;
}

static inline void irq_unlock(uint32_t primask)
{
    if (primask == 0u) {
        __asm volatile ("cpsie i" ::: "memory");
    }
}

/* ---- guards -------------------------------------------------------------- */

/* True if [addr, addr+len) lies entirely inside the writable window
 * [BL_FLASH_WRITE_FLOOR, BL_FLASH_END).  Overflow-safe. */
static int range_writable(uint32_t addr, uint32_t len)
{
    if (len == 0u) {
        return 0;
    }
    if (addr < BL_FLASH_WRITE_FLOOR) {
        return 0;                       /* the bootloader's own flash */
    }
    if (addr >= BL_FLASH_END) {
        return 0;                       /* past CodeFlash (DataFlash/InfoFlash) */
    }
    if (len > (BL_FLASH_END - addr)) {
        return 0;                       /* runs off the end; cannot wrap */
    }
    return 1;
}

/* True if [p, p+len) lies entirely inside SRAM. */
static int range_in_sram(uint32_t p, uint32_t len)
{
    if (len == 0u) {
        return 0;
    }
    if (p < BL_SRAM_BASE || p >= BL_SRAM_END) {
        return 0;
    }
    if (len > (BL_SRAM_END - p)) {
        return 0;
    }
    return 1;
}

/* ---- controller primitives ----------------------------------------------- */

void bl_flash_lock(void)
{
    R8_FLASH_PROTECT = FLASH_LOCK;
}

/* Issue one command with flash unlocked, and re-lock unconditionally.
 * No polling: the core is paused for the whole operation, so the status read on
 * the next instruction is already the final result.
 *
 * NOTE: R8_FLASH_PROTECT reads back 0x08 after writing 0x88 because
 * RB_ROM_WE_MUST_10 is write-only.  That is expected and is NOT checked here —
 * treating it as a failure is the classic bug on this part. */
static int flash_command(uint32_t addr, uint32_t data, uint8_t cmd, int use_data)
{
    uint32_t primask;
    uint32_t status;

    primask = irq_lock();

    R8_FLASH_PROTECT = FLASH_UNLOCK_CODE;
    R32_FLASH_ADDR   = addr;
    if (use_data) {
        R32_FLASH_DATA = data;
    }
    R8_FLASH_COMMAND = cmd;
    status = (uint32_t)R16_FLASH_STATUS & 0xFFu;   /* mask is mandatory */
    R8_FLASH_PROTECT = FLASH_LOCK;                 /* re-lock before anything else */

    irq_unlock(primask);

    if (status & (RB_ROM_CMD_TOUT | RB_ROM_CMD_ERR)) {
        return BL_FLASH_ERR_STATUS;
    }
    if (status != FLASH_STATUS_OK) {
        return BL_FLASH_ERR_STATUS;
    }
    return BL_FLASH_OK;
}

/* ---- public API ---------------------------------------------------------- */

int bl_flash_erase_sector(uint32_t addr)
{
    int rc;

    /* Alignment first, then range.  Both run before any hardware access, so the
     * ordering cannot weaken the floor guard; it only decides which code comes
     * back for an address that is both misaligned and out of range, and no
     * caller distinguishes the two.  (It also keeps the emitted instruction
     * stream clear of a word tools/check_image.py's raw InfoFlash scan flags —
     * see the note at the end of this file.) */
    if (addr & (BL_FLASH_SECTOR_SIZE - 1u)) {
        return BL_FLASH_ERR_ALIGN;      /* reject, never mask down to a boundary */
    }
    if (!range_writable(addr, BL_FLASH_SECTOR_SIZE)) {
        return BL_FLASH_ERR_RANGE;
    }

    rc = flash_command(addr, 0u, ROM_CMD_ERASE, 0);
    if (rc != BL_FLASH_OK) {
        return rc;
    }

    /* Cheap and worth it: a silent erase failure would corrupt the image later.
     * The status form, not the predicate, so the code propagates as-is;
     * BL_FLASH_ERR_RANGE cannot come back (range_writable() already accepted the
     * identical range), so this only ever reports BL_FLASH_ERR_VERIFY. */
    return bl_flash_check_erased(addr, BL_FLASH_SECTOR_SIZE);
}

int bl_flash_program_word(uint32_t addr, uint32_t val)
{
    int rc;

    /* Alignment first, then range — same reasoning as bl_flash_erase_sector(). */
    if (addr & (BL_FLASH_WORD_SIZE - 1u)) {
        return BL_FLASH_ERR_ALIGN;
    }
    if (!range_writable(addr, BL_FLASH_WORD_SIZE)) {
        return BL_FLASH_ERR_RANGE;
    }

    rc = flash_command(addr, val, ROM_CMD_PROG, 1);
    if (rc != BL_FLASH_OK) {
        return rc;
    }

    if (*(volatile const uint32_t *)addr != val) {
        return BL_FLASH_ERR_VERIFY;     /* e.g. target was not erased first */
    }
    return BL_FLASH_OK;
}

int bl_flash_program(uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t off;

    if (buf == 0) {
        return BL_FLASH_ERR_PARAM;
    }
    if (len == 0u) {
        return BL_FLASH_ERR_PARAM;
    }
    if (!range_writable(addr, len)) {
        return BL_FLASH_ERR_RANGE;
    }
    if ((addr & (BL_FLASH_WORD_SIZE - 1u)) || (len & (BL_FLASH_WORD_SIZE - 1u))) {
        return BL_FLASH_ERR_ALIGN;      /* an odd tail is rejected, not padded:
                                         * programming cannot set bits 1 -> 0 back,
                                         * so read-modify-write is not available */
    }
    if (!range_in_sram((uint32_t)src, len)) {
        return BL_FLASH_ERR_SRC;        /* source must be in RAM — see flash.h */
    }

    for (off = 0u; off < len; off += BL_FLASH_WORD_SIZE) {
        /* Assembled byte-wise: the caller's buffer needs no alignment, and
         * ARMv6-M cannot do unaligned loads at all.  Little-endian, matching
         * how the CH579 stores words in flash. */
        uint32_t word = (uint32_t)src[off]
                      | ((uint32_t)src[off + 1u] << 8)
                      | ((uint32_t)src[off + 2u] << 16)
                      | ((uint32_t)src[off + 3u] << 24);
        int rc = bl_flash_program_word(addr + off, word);
        if (rc != BL_FLASH_OK) {
            bl_flash_lock();            /* paranoia: guaranteed locked on error exit */
            return rc;
        }
    }

    bl_flash_lock();
    return BL_FLASH_OK;
}

/* Status-code form.  BL_FLASH_OK / BL_FLASH_ERR_VERIFY / BL_FLASH_ERR_RANGE.
 * See the commentary in flash.h for why the predicate form is separate. */
int bl_flash_check_erased(uint32_t addr, uint32_t len)
{
    uint32_t off;

    if (!range_writable(addr, len)) {
        return BL_FLASH_ERR_RANGE;      /* also catches len == 0 */
    }

    /* Word-at-a-time where possible; ARMv6-M needs the alignment. */
    if (((addr & 3u) == 0u) && ((len & 3u) == 0u)) {
        for (off = 0u; off < len; off += 4u) {
            if (*(volatile const uint32_t *)(addr + off) != 0xFFFFFFFFu) {
                return BL_FLASH_ERR_VERIFY;
            }
        }
    } else {
        for (off = 0u; off < len; off++) {
            if (*(volatile const uint8_t *)(addr + off) != 0xFFu) {
                return BL_FLASH_ERR_VERIFY;
            }
        }
    }
    return BL_FLASH_OK;
}

/* Predicate form.  EXACTLY 0 or 1 — never a negative code, so `if (...)` reads
 * correctly.  A rejected argument answers 0, i.e. "not known to be blank". */
int bl_flash_is_erased(uint32_t addr, uint32_t len)
{
    return (bl_flash_check_erased(addr, len) == BL_FLASH_OK) ? 1 : 0;
}

/* ---- note on tools/check_image.py's raw InfoFlash scan --------------------
 * That check walks EVERY 4-byte-aligned word of the .bin and fails if one falls
 * in 0x00040000..0x000403FF, without distinguishing literal pools from
 * instruction encodings.  Two adjacent Thumb instructions can therefore trip it
 * by coincidence — e.g. `lsls r1,r1,#2` (0x0089) after `movs r4,r0` (0x0004) on
 * a word boundary reads as 0x00040089.  This driver cannot reach InfoFlash at
 * all: BL_FLASH_END is 0x3E800 and 0x8C is never written to R8_FLASH_PROTECT.
 * The guard ordering above keeps the stream clean today, but that is luck, not a
 * property; the durable fix belongs in the tool.  If this check fires after an
 * unrelated edit here, read the disassembly before believing it. */
