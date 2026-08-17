/* flash_model.h — register-level model of the CH579 CodeFlash controller, so
 * that src/flash.c can be compiled and executed natively.
 *
 * WHY A SECOND FLASH MODEL?  host/flash_stub.c models the flash *service* that
 * proto.c consumes (erase/program/read behind bl_flash_ops).  It stands in for
 * src/flash.c and therefore cannot test it.  This file models the *silicon* one
 * level lower — R32_FLASH_ADDR, R8_FLASH_COMMAND, R8_FLASH_PROTECT,
 * R16_FLASH_STATUS and a 250 KB array — so that src/flash.c itself is the code
 * under test, guards and all.
 *
 * HOW src/flash.c IS REDIRECTED (least-invasive choice, and it is not zero)
 * ------------------------------------------------------------------------
 * src/flash.c is NOT edited and NOT conditionally compiled.  The host build
 * generates build/flash_host.c from it with a fixed sed transform (see the
 * $(FLASHGEN) rule in host/Makefile) that touches exactly 15 lines and nothing
 * else.  A -D on a register base was considered and rejected: it cannot solve
 * the real blockers.
 *
 *   1. MMIO.  flash.c hard-codes `(*(volatile uint8_t *)0x40001808u)`.  On
 *      arm64 macOS the whole low 4 GB is __PAGEZERO, so nothing can be mapped
 *      at 0x40001800 — nor at 0x3E00, which flash.c dereferences directly when
 *      it verifies a program or blank-checks a sector.  Even if it could be
 *      mapped, a plain memory store to the command register cannot *trigger*
 *      anything, and the command register is where the erase happens.  The
 *      transform rewrites the seven register statements into fm_w8/fm_w32/
 *      fm_r16 calls and the three raw-address reads into fm_rd32/fm_rd8.
 *   2. Inline assembly.  `mrs %0, primask` / `cpsid i` do not assemble for
 *      arm64 or x86-64 at all, so *some* rewrite is unavoidable.  The three
 *      statements become fm_primask()/fm_cpsid()/fm_cpsie(), which the model
 *      tracks so the tests can assert the unlock window really is interrupt-
 *      masked and that PRIMASK is restored, not blindly re-enabled.
 *   3. The SRAM window.  bl_flash_program() rejects a source outside
 *      [0x20000000, 0x20008000); a host heap pointer would always be rejected
 *      and the whole programming path would be untestable.  The two constants
 *      are rewritten to fm_sram_lo()/fm_sram_hi(), which describe a real
 *      32 KB host buffer.  flash.c's comparison logic — including its
 *      overflow-safe `len > END - p` — is left exactly as written.
 *
 * The generated file is checked twice at build time: the diff against
 * src/flash.c must be exactly the expected number of lines, and the
 * PREPROCESSED output must contain no `volatile`, no `__asm` and no 0x400018
 * literal, i.e. no path back to real MMIO can survive the transform.
 * Everything the tests care about — every guard, every ordering, every status
 * check — is byte-identical to the shipping source.
 *
 * MODELLED SEMANTICS (all hardware-measured; see docs/DESIGN.md §3)
 *   - ROM_CMD_PROG 0x9A programs one 32-bit word and ANDs into the existing
 *     contents.  Bits only go 1 -> 0.  Re-programming an already-programmed
 *     word succeeds; programming 0xFFFFFFFF over 0x0F0F0F0F leaves
 *     0x0F0F0F0F, so flash.c's read-back verify fails, exactly as on silicon.
 *   - ROM_CMD_ERASE 0xA6 erases exactly 512 bytes and not one byte more.
 *   - Unlock is writing 0x88 to R8_FLASH_PROTECT.  It READS BACK 0x08 because
 *     RB_ROM_WE_MUST_10 (0x80) is write-only.  Treating that as a failure is
 *     the classic bug on this part, so the model reproduces it.
 *   - R16_FLASH_STATUS bit 9 reads 1 permanently.  The model always sets it,
 *     so a driver that dropped flash.c's mandatory `& 0xFF` would fail every
 *     command here.
 *   - Success is low byte == 0x40 (RB_ROM_ADDR_OK set, TOUT and CMD_ERR clear).
 *   - There is no busy bit: the core is halted for the duration, so the status
 *     read on the next instruction is already final.  The model executes the
 *     command inside the write to R8_FLASH_COMMAND, matching that.
 *   - A command issued while LOCKED does nothing and reports CMD_ERR.  The
 *     model counts those (FM_STAT_CMD_WHILE_LOCKED).
 *   - The hardware has NO write floor.  The model happily erases sector 0 if
 *     told to; the only thing standing between a bug and a destroyed
 *     bootloader is flash.c's own guard, which is the point of the exercise.
 *     Commands reaching the controller below 0x3E00 are counted
 *     (FM_STAT_CMD_BELOW_FLOOR) and independently caught by the tripwire
 *     pattern fm_init() writes there.
 */

#ifndef FLASH_MODEL_H
#define FLASH_MODEL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FM_FLASH_SIZE     0x0003E800u   /* 250 KB CodeFlash                  */
#define FM_WRITE_FLOOR    0x00003E00u   /* what flash.c must never go below  */
#define FM_SECTOR         512u
#define FM_SRAM_SIZE      0x8000u       /* 32 KB, same as the real CH579     */

/* ---- register file ------------------------------------------------------- */

enum {
    FM_DATA = 0,        /* R32_FLASH_DATA    0x40001800 */
    FM_ADDR,            /* R32_FLASH_ADDR    0x40001804 */
    FM_CMD,             /* R8_FLASH_COMMAND  0x40001808 */
    FM_PROTECT,         /* R8_FLASH_PROTECT  0x40001809 */
    FM_STATUS,          /* R16_FLASH_STATUS  0x4000180A */
    FM_NREGS
};

/* Opcodes and status bits, restated here so the tests can assert against the
 * same numbers the driver uses without including src/flash.c's private ones. */
#define FM_CMD_PROG       0x9Au
#define FM_CMD_ERASE      0xA6u

#define FM_ST_TOUT        0x0001u
#define FM_ST_CMD_ERR     0x0002u
#define FM_ST_ADDR_OK     0x0040u
#define FM_ST_ALWAYS_SET  0x0200u       /* bit 9 reads 1 permanently         */

#define FM_PROT_LOCK      0x80u
#define FM_PROT_CODE_WE   0x08u
#define FM_PROT_DATA_WE   0x04u         /* 0x8C — the InfoFlash door. Never.  */

/* The transformed src/flash.c calls these.  Nothing else should. */
void     fm_w32(int reg, uint32_t v);
void     fm_w8 (int reg, uint8_t  v);
uint16_t fm_r16(int reg);
uint32_t fm_r32(int reg);
uint32_t fm_rd32(uint32_t addr);        /* a word read of modelled flash     */
uint8_t  fm_rd8 (uint32_t addr);        /* a byte read of modelled flash     */
uint32_t fm_primask(void);
void     fm_cpsid(void);
void     fm_cpsie(void);
uint32_t fm_sram_lo(void);
uint32_t fm_sram_hi(void);

/* ---- lifecycle ----------------------------------------------------------- */

/* Reset registers, counters and fault injection.  [0, 0x3E00) is filled with a
 * deterministic tripwire pattern containing no 0xFF byte — so an erase down
 * there is always detectable, which an all-0xFF fill could not be — and
 * [0x3E00, 250 KB) is set to 0xFF, i.e. erased. */
void fm_init(void);

/* Same, but the entire 250 KB array is 0xFF, for tests that want a uniformly
 * blank part.  fm_bootloader_intact() is meaningless after this. */
void fm_init_blank(void);

/* 1 while every byte below FM_WRITE_FLOOR still holds fm_init()'s pattern. */
int fm_bootloader_intact(void);

/* Write straight into the array, bypassing the controller entirely.  For
 * arranging non-erased flash without laundering it through the driver. */
void fm_poke(uint32_t addr, const void *buf, uint32_t len);
void fm_fill(uint32_t addr, uint8_t val, uint32_t len);

const uint8_t *fm_mem(void);            /* not counted as a device read */

/* ---- observation --------------------------------------------------------- */

enum {
    FM_STAT_ERASES = 0,        /* erase commands the controller carried out   */
    FM_STAT_PROGRAMS,          /* program commands carried out                */
    FM_STAT_CMD_WHILE_LOCKED,  /* command issued with code WE clear           */
    FM_STAT_BAD_CMD,           /* unrecognised opcode                         */
    FM_STAT_CMD_BELOW_FLOOR,   /* MUST STAY 0: command aimed under 0x3E00     */
    FM_STAT_CMD_OOR,           /* command aimed past the end of CodeFlash     */
    FM_STAT_CMD_MISALIGNED,    /* MUST STAY 0: erase !512-aligned, prog !4    */
    FM_STAT_CMD_IRQ_UNMASKED,  /* MUST STAY 0: command run with IRQs enabled  */
    FM_STAT_UNLOCKS,           /* protect writes that enabled code WE         */
    FM_STAT_LOCKS,             /* protect writes that cleared it              */
    FM_STAT_DATAFLASH_WE,      /* MUST STAY 0: 0x04 set, i.e. InfoFlash armed */
    FM_STAT_BAD_PROTECT,       /* MUST STAY 0: a value other than 0x80 / 0x88 */
    FM_STAT_READS32,
    FM_STAT_READS8,
    FM_STAT_READ_OOR,          /* MUST STAY 0: read outside the 250 KB array  */
    FM_STAT_READ_BELOW_FLOOR,  /* MUST STAY 0: driver read its own sectors    */
    FM_STAT_READ_MISALIGNED,   /* MUST STAY 0: ARMv6-M cannot do these        */
    FM_STAT_REG_WRITES,        /* every fm_w8/fm_w32, i.e. controller traffic */
    FM_STAT_INJECTED,          /* faults actually delivered                   */
    FM_STAT_NCOUNTERS
};

uint32_t fm_stat(int which);
void     fm_stat_reset(void);

int      fm_unlocked(void);             /* code WE currently set?            */
uint8_t  fm_protect_readback(void);     /* what a read of 0x40001809 gives   */
int      fm_irq_masked(void);           /* model PRIMASK                     */
uint16_t fm_status(void);               /* full 16-bit status register       */

/* ---- the modelled SRAM window ------------------------------------------- */

/* A real, readable 32 KB host buffer that flash.c's range_in_sram() accepts.
 * fm_outside_sram() returns an equally real, readable buffer of the same size
 * that is guaranteed to sit outside the accepted window — so the
 * BL_FLASH_ERR_SRC guard can be attacked with a pointer that would not crash
 * the harness if the guard let it through. */
void *fm_sram(void);
void *fm_outside_sram(void);

/* ---- fault injection ----------------------------------------------------- */

typedef enum {
    FM_FAULT_NONE = 0,
    FM_FAULT_ERASE_STATUS,   /* Nth erase: nothing erased, status reports err */
    FM_FAULT_PROG_STATUS,    /* Nth program: nothing written, status err      */
    FM_FAULT_ERASE_SILENT,   /* Nth erase: status OK, last byte stays 0x00    */
    FM_FAULT_PROG_SILENT,    /* Nth program: status OK, word does not take    */
    FM_FAULT_UNLOCK_IGNORED, /* Nth unlock does not take; the command is then
                              * issued against a LOCKED controller            */
    FM_FAULT_STATUS_NOISE    /* Nth command: OR status_low<<8 into the status
                              * register, on top of the normal result — proves
                              * flash.c's `& 0xFFu` mask is doing its job      */
} fm_fault_t;

/* Arm a one-shot fault on the nth event of its kind, counting from 1.
 * nth == 0 disarms.  status_low is the low status byte the *_STATUS faults
 * report (0 selects the default, FM_ST_CMD_ERR) and the byte STATUS_NOISE
 * shifts into the high half.  Cleared by fm_init(). */
void fm_inject(fm_fault_t what, uint32_t nth, uint8_t status_low);

#ifdef __cplusplus
}
#endif

#endif /* FLASH_MODEL_H */
