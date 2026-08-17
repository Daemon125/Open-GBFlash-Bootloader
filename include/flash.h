/* flash.h — CH579 CodeFlash driver for the GBFlash update-mode bootloader.
 *
 * The API is deliberately small and hard to misuse.  Every entry point
 * validates its own arguments; there is no "caller promises" contract.  A bug
 * anywhere else in the bootloader cannot make this driver touch sector 0.
 *
 * Hardware protocol (proven on the real device; docs/DESIGN.md §3 is the
 * written-up version):
 *   unlock  R8_FLASH_PROTECT = 0x88   (reads back 0x08 — WE_MUST_10 is write-only)
 *   erase   R32_FLASH_ADDR = a; R8_FLASH_COMMAND = 0xA6    (512-byte sector)
 *   program R32_FLASH_ADDR = a; R32_FLASH_DATA = w; R8_FLASH_COMMAND = 0x9A
 *   status  (R16_FLASH_STATUS & 0xFF) == 0x40 is success.  The mask is MANDATORY:
 *           bit 9 reads 1 permanently, so an unmasked 16-bit compare never matches.
 *   relock  R8_FLASH_PROTECT = 0x80
 * There is no busy bit and no polling loop: the MCU is halted by hardware for
 * the duration of the operation.  Typical ~1.4 ms erase, ~27 us word program;
 * WORST CASE up to 2.4 ms and ~36 us, which is the pair every timing argument
 * in timebase.h is derived from.  Both pairs appear in the docs on purpose.
 *
 * The header itself is portable — no MMIO, no target-only types — so the native
 * host test harness can link its own stub implementation of these functions.
 */

#ifndef BL_FLASH_H
#define BL_FLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- geometry ------------------------------------------------------------ */

/* CodeFlash is 250 KB: 0x00000000..0x0003E7FF.  DataFlash (0x3E800) and
 * InfoFlash (0x00040000, which holds CFG_BOOT_EN) are NOT reachable through
 * this driver by design. */
#define BL_FLASH_END             0x0003E800u   /* exclusive */

#define BL_FLASH_SECTOR_SIZE     512u          /* erase granularity, verified */
#define BL_FLASH_WORD_SIZE       4u            /* program granularity */

/* The write floor.  Everything below this is the bootloader's own code.
 * 0x3E00 = 31 * 512, so the floor is exactly a sector boundary and the
 * bootloader occupies sectors 0..30 whole.  The boot-info record owns sector 31
 * alone; the application starts at 0x4000 = sector 32. */
#define BL_FLASH_WRITE_FLOOR     0x00003E00u

/* ---- return codes -------------------------------------------------------- */

#define BL_FLASH_OK               0
#define BL_FLASH_ERR_RANGE      (-1)   /* below the write floor, or past CodeFlash */
#define BL_FLASH_ERR_ALIGN      (-2)   /* bad address or length alignment */
#define BL_FLASH_ERR_PARAM      (-3)   /* null pointer, zero length, overflow */
#define BL_FLASH_ERR_STATUS     (-4)   /* controller reported TOUT/ERR/!ADDR_OK */
#define BL_FLASH_ERR_VERIFY     (-5)   /* readback did not match */
#define BL_FLASH_ERR_SRC        (-6)   /* source buffer is not in RAM (see below) */

/* ---- API ----------------------------------------------------------------- */

/* Erase one 512-byte sector.  addr must be 512-byte aligned and >= 0x3E00.
 * Verifies the whole sector reads 0xFF afterwards. */
int bl_flash_erase_sector(uint32_t addr);

/* Program one 32-bit word.  addr must be 4-byte aligned and >= 0x3E00.
 * Verifies the readback.  Flash programming can only clear bits 1 -> 0, so the
 * target must have been erased (or the new value must be a bit-subset of the
 * old one); otherwise this returns BL_FLASH_ERR_VERIFY. */
int bl_flash_program_word(uint32_t addr, uint32_t val);

/* Program len bytes from buf, little-endian word order.  addr and len must both
 * be 4-byte aligned; an unaligned length is REJECTED, never rounded or
 * read-modify-written (programming cannot set bits back to 1).
 *
 * buf is read a byte at a time and assembled into a register before the command
 * is issued, so it needs no alignment of its own.  It must, however, live in
 * SRAM: WCH's flash application note states the source of a flash write is only
 * supported in RAM, and this driver enforces that (BL_FLASH_ERR_SRC) rather
 * than trusting callers.  Update chunks and the boot-info record are both
 * assembled in RAM already, so nothing legitimate is refused.
 * Stops at the first failing word; earlier words remain programmed. */
int bl_flash_program(uint32_t addr, const void *buf, uint32_t len);

/* Force the flash controller back to the locked state (R8_FLASH_PROTECT = 0x80).
 * Every function above already re-locks on every exit path, including error
 * paths; this exists for belt-and-braces use at the end of a session. */
void bl_flash_lock(void);

/* Blank-check [addr, addr+len).  Both entry points apply the same address guard
 * as the write paths, deliberately: nothing in the bootloader has any business
 * asking about its own sectors through this driver.
 *
 * TWO forms on purpose, because the obvious single form is a trap.  This
 * function used to return 1 / 0 / negative-error, and -1 is TRUE in C, so the
 * natural-looking `if (bl_flash_is_erased(a, n)) { ...treat as blank... }` did
 * the wrong thing for a rejected argument.  The split removes the trap instead
 * of documenting it:
 *
 *   bl_flash_is_erased()    a real predicate.  EXACTLY 0 or 1, never negative,
 *                           so it is always safe in an `if`.  Bad arguments
 *                           yield 0 — "not known to be blank" — which is the
 *                           fail-safe answer for every caller: a blank check
 *                           that could not be performed must never read as a
 *                           successful one.
 *
 *   bl_flash_check_erased() the status-code form, for callers that must
 *                           distinguish and propagate.  Returns BL_FLASH_OK if
 *                           every byte reads 0xFF, BL_FLASH_ERR_VERIFY if any
 *                           byte does not, BL_FLASH_ERR_RANGE for a range or
 *                           zero-length argument.  Compare against
 *                           BL_FLASH_OK, never for truth. */
int bl_flash_is_erased(uint32_t addr, uint32_t len);
int bl_flash_check_erased(uint32_t addr, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* BL_FLASH_H */
