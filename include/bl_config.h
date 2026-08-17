/* bl_config.h — GBFlash CH579 update-mode bootloader: addresses and constants.
 *
 * OWNED BY: comp:vectors+ld
 *
 * Where a value was measured on the real device it is marked [MEASURED]; where
 * it comes from the CH579 datasheet it is marked [DS]; inferences are marked
 * [INFERRED].  docs/DESIGN.md is the written-up reasoning.
 *
 * This header is written to be includable from BOTH C and assembly (.S)
 * sources — src/start.S and src/vectors.S spell their constants out locally
 * today, but the guard is kept so either could include it.  Therefore:
 *   - every macro outside the __ASSEMBLER__ guard is a plain integer literal,
 *     with no casts and no C syntax;
 *   - pointer-typed conveniences live at the bottom, behind #ifndef __ASSEMBLER__.
 */

#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/* ------------------------------------------------------------------------- */
/* 1. Flash memory map                                                        */
/* ------------------------------------------------------------------------- */

/* CodeFlash is 250 KB at 0x00000000..0x0003E7FF.  512-byte erase sectors.
 * 0x3E00 = 31 * 512 exactly, and 0x4000 = 32 * 512 exactly, so the three
 * regions below are each a whole number of sectors and never share one.
 * [DS + MEASURED] */

#define BL_CODEFLASH_BASE       0x00000000
#define BL_CODEFLASH_SIZE       0x0003E800      /* 250 KB                     */
#define BL_CODEFLASH_END        0x0003E800      /* one past the last byte     */

/* The bootloader's own image.  Sectors 0..30.  15,872 bytes. */
#define BL_SELF_BASE            0x00000000
#define BL_SELF_SIZE            15872
#define BL_SELF_END             0x00003E00

/* The boot-info record.  Sector 31, entirely to itself. */
#define BL_BOOTINFO_BASE        0x00003E00
#define BL_BOOTINFO_SECTOR_SIZE 0x00000200
#define BL_BOOTINFO_LEN         14              /* bytes actually used        */

/* The application.  Sector 32 upward. */
#define BL_APP_BASE             0x00004000
#define BL_APP_MAX_SIZE         (BL_CODEFLASH_END - BL_APP_BASE)   /* 0x3A800 */

/* THE runtime safety floor.  At runtime the bootloader must never erase or
 * program any address below this.  Enforce it in flash.c on every call.
 * [hard constraint 2] */
#define BL_WRITE_FLOOR          BL_BOOTINFO_BASE                   /* 0x3E00  */

/* ------------------------------------------------------------------------- */
/* 2. SRAM map                                                                */
/* ------------------------------------------------------------------------- */

#define BL_SRAM_BASE            0x20000000
#define BL_SRAM_SIZE            0x00008000      /* 32 KB                      */
#define BL_STACK_TOP            0x20008000      /* vector[0]                  */

/* The application reserves 0x20000000..0x2000009F as an UNINIT / no-init hole
 * that its scatter table never touches.  We reproduce that hole exactly so the
 * handshake word below survives a SYSRESETREQ in BOTH directions.
 * [CONFIRMED] */
#define BL_NOINIT_BASE          0x20000000
#define BL_NOINIT_SIZE          0x000000A0
#define BL_RW_BASE              0x200000A0      /* first byte of .data/.bss   */

/* ------------------------------------------------------------------------- */
/* 3. The update-request handshake                                            */
/* ------------------------------------------------------------------------- */

/* The application writes BL_BOOT_MAGIC_VALUE here and issues SYSRESETREQ.
 * SRAM contents across SYSRESETREQ verified on the real device. [MEASURED]
 *
 * THE BOOTLOADER MUST CLEAR THIS WORD once it has consumed the request, on
 * EVERY path.  If it does not, the device re-enters update mode on every
 * subsequent reset, forever, and looks bricked while being perfectly healthy.
 * [see docs/DESIGN.md §5] */
#define BL_BOOT_MAGIC_ADDR      0x20000090
#define BL_BOOT_MAGIC_VALUE     0xAA55BB01

/* SCB AIRCR, for issuing SYSRESETREQ ourselves if we ever need to. */
#define BL_SCB_AIRCR            0xE000ED0C
#define BL_SYSRESETREQ_KEY      0x05FA0004

/* ------------------------------------------------------------------------- */
/* 4. Vector table and the trampoline dispatcher                              */
/* ------------------------------------------------------------------------- */

/* 16 core exceptions + 20 IRQs = 36 entries = 0x90 bytes.  Confirmed against
 * the vendor datasheet Table 3-1 and against the app's own live table.
 * [CONFIRMED] */
#define BL_VECTOR_COUNT         36
#define BL_VECTOR_TABLE_SIZE    0x90

/* The dispatcher accepts an application handler address only inside
 * [BL_VEC_ACCEPT_LO, BL_VEC_ACCEPT_HI).  This rejects BOTH a zeroed table entry
 * (0x00000000, reserved slots and today's provisioning) AND an erased one
 * (0xFFFFFFFF, what a real bootloader leaves behind mid-update).
 *
 * Rejecting 0xFFFFFFFF is MANDATORY, not cosmetic: `bx r0` executes in Handler
 * mode, where ARMv6-M reads a PC of 0xFFFFFFFx as EXC_RETURN.  0xFFFFFFFF is a
 * reserved EXC_RETURN, so it faults, and a fault from a fault handler on M0
 * escalates to LOCKUP rather than the intended spin.  [see docs/DESIGN.md §1]
 *
 * The two bounds are powers of two so the check is two `lsrs` and two branches:
 *   lsrs r1, r0, #14   -> 0 iff r0 <  0x4000
 *   lsrs r1, r0, #18   -> 0 iff r0 <  0x40000 */
#define BL_VEC_ACCEPT_LO        0x00004000      /* == BL_APP_BASE, 1 << 14    */
#define BL_VEC_ACCEPT_HI        0x00040000      /* 1 << 18                    */
#define BL_VEC_ACCEPT_LO_SHIFT  14
#define BL_VEC_ACCEPT_HI_SHIFT  18

/* ------------------------------------------------------------------------- */
/* 5. Clock — internal RC only.  NEVER select the external crystal.           */
/* ------------------------------------------------------------------------- */

/* R16_CLK_SYS_CFG is a safe-access (SAM) register.  Writing it requires
 * 0x57 then 0xA8 written to R8_SAFE_ACCESS_SIG within a few CPU cycles of the
 * target write, with no intervening instructions. */
#define BL_R8_SAFE_ACCESS_SIG   0x40001040
#define BL_SAFE_ACCESS_SIG1     0x57
#define BL_SAFE_ACCESS_SIG2     0xA8
#define BL_SAFE_ACCESS_LOCK     0x00

#define BL_R16_CLK_SYS_CFG      0x40001008
#define BL_R8_HFCK_PWR_CTRL     0x4000100A      /* do not disturb: 0x1C live  */

/* PLL_DIV = 8, SYS_MOD = 2 ("directly from 32 MHz"), RB_CLK_OSC32M_XT = 0
 * (INTERNAL RC).  This is what the shipping application's SystemInit writes,
 * and the internal RC is proven to drive USB CDC at 2 Mbaud on this hardware.
 * The live device reads back 0x8088; bit 15 is not written by SystemInit and is
 * believed to be a hardware status/ready bit.  We write 0x0088 and never touch
 * bit 8 (RB_CLK_OSC32M_XT) — some boards have no crystal fitted.
 * [MEASURED] */
#define BL_CLK_SYS_CFG_VALUE    0x0088
#define BL_FSYS_HZ              32000000

/* ------------------------------------------------------------------------- */
/* 6. GPIO — port B block                                                     */
/* ------------------------------------------------------------------------- */

#define BL_R32_PB_DIR           0x400010C0
#define BL_R32_PB_PIN           0x400010C4
#define BL_R32_PB_OUT           0x400010C8
#define BL_R32_PB_CLR           0x400010CC
#define BL_R32_PB_PU            0x400010D0
#define BL_R32_PB_PD_DRV        0x400010D4

/* Activity LED: PB12, output push-pull, firmware convention LOW = lit.
 * [CONFIRMED] */
#define BL_LED_PIN              12
#define BL_LED_MASK             (1u << 12)

/* U22 user button: PB23, input with pull-up, ACTIVE LOW.  Holding it at
 * power-on forces update mode.  Allow >= 1 ms after enabling the pull-up
 * before sampling. [CONFIRMED] */
#define BL_BTN_PIN              23
#define BL_BTN_MASK             (1u << 23)
#define BL_BTN_SETTLE_MS        1

/* PB22 is the application's PCB-revision strap, and is believed to be the
 * H1 / ISP boot pad as well.  A DIFFERENT pin from U22.  Never configure it,
 * never drive it — it may be the floor under every recovery path. [INFERRED] */
#define BL_H1_PIN               22

/* ------------------------------------------------------------------------- */
/* 7. Flash controller                                                        */
/* ------------------------------------------------------------------------- */

#define BL_R32_FLASH_DATA       0x40001800
#define BL_R32_FLASH_ADDR       0x40001804
#define BL_R8_FLASH_COMMAND     0x40001808
#define BL_R8_FLASH_PROTECT     0x40001809
#define BL_R16_FLASH_STATUS     0x4000180A

#define BL_ROM_CMD_PROG         0x9A            /* program one 32-bit word    */
#define BL_ROM_CMD_ERASE        0xA6            /* erase one 512-byte sector  */

/* R8_FLASH_PROTECT.  RB_ROM_WE_MUST_10 (0x80) is WRITE-ONLY: after writing
 * 0x88 the register READS BACK 0x08.  That is not a failure.
 *
 * 0x8C (InfoFlash unlocked) MUST NEVER BE WRITTEN.  InfoFlash holds
 * CFG_BOOT_EN, which is what makes H1/ROM-ISP recovery work.  The datasheet
 * requires BOTH the DATA and CODE write-enable bits for any InfoFlash access,
 * and every path in this design sets exactly one. */
#define BL_FLASH_LOCK           0x80
#define BL_FLASH_UNLOCK_CODE    0x88            /* CodeFlash write-enable     */
/* deliberately NOT defined: DataFlash (0x84) and InfoFlash (0x8C) unlocks. */

/* R16_FLASH_STATUS.  There is NO busy bit — the datasheet states the MCU is
 * automatically paused for the duration of the operation, so no polling loop
 * is needed and code may run from flash while programming other flash.
 * Success signature measured on the real device: 0x40. */
#define BL_FLASH_STAT_CMD_TOUT  0x0001
#define BL_FLASH_STAT_CMD_ERR   0x0002
#define BL_FLASH_STAT_ADDR_OK   0x0040
#define BL_FLASH_STAT_OK_MASK   (BL_FLASH_STAT_ADDR_OK | BL_FLASH_STAT_CMD_TOUT | BL_FLASH_STAT_CMD_ERR)
#define BL_FLASH_STAT_OK_VALUE  BL_FLASH_STAT_ADDR_OK       /* 0x40           */

#define BL_FLASH_SECTOR_SIZE    512
#define BL_FLASH_SECTOR_MASK    (BL_FLASH_SECTOR_SIZE - 1)

/* ------------------------------------------------------------------------- */
/* 8. Boot-info record at 0x3E00                                              */
/* ------------------------------------------------------------------------- */

/*   0x00 u16  marker      — 0xFFFF in every distributed image.  DO NOT STAMP
 *                           0x5555: the stored record CRC covers bytes
 *                           0x00..0x0B including the marker, and changing
 *                           0xFFFF -> 0x5555 requires SETTING bits, which
 *                           flash programming cannot do without an erase.
 *   0x02 4    "LFBG"
 *   0x06 u16  CRC16 of the application bytes at 0x4000, `length` of them
 *   0x08 u32  application length (0x7520 for L15)
 *   0x0C u16  CRC16 of record bytes 0x00..0x0B
 * All little-endian.  Both CRCs verified to recompute. [MEASURED] */
#define BL_BI_OFF_MARKER        0x00
#define BL_BI_OFF_NAME          0x02
#define BL_BI_OFF_APP_CRC       0x06
#define BL_BI_OFF_APP_LEN       0x08
#define BL_BI_OFF_REC_CRC       0x0C

#define BL_BI_MARKER_ERASED     0xFFFF
#define BL_BI_MARKER_STAMPED    0x5555          /* accepted, never written    */
#define BL_BI_NAME_0            'L'
#define BL_BI_NAME_1            'F'
#define BL_BI_NAME_2            'B'
#define BL_BI_NAME_3            'G'

/* Upper bound on the application length accepted by app_valid() AND by the
 * updater's 0x24 handler.  The two MUST be the same number.
 *
 * A ceiling tighter than the validator's is an update-breaking defect: h_data()
 * erases the boot-info sector on packet 1, BEFORE any total-size limit applies,
 * so an image above it erases the header, writes part of the application, then
 * NAKs every remaining packet forever — no valid application, stuck in update
 * mode, unable to accept the firmware being installed.
 *
 * boot.h and proto.h both define this as BL_APP_MAX_LEN = 0x3A800.  The
 * translation units that do include bl_config.h (src/led.c, src/usb.c,
 * src/timebase.c) use only its GPIO, USB and clock constants, so a stale value
 * HERE would sit undetected; host/check_headers compiles all three headers and
 * FAILS if they diverge. */
#define BL_BI_MAX_APP_LEN       BL_APP_MAX_SIZE         /* 0x3A800 */

/* Application initial-SP sanity test:  (word_at_0x4000 & MASK) == VALUE. */
#define BL_APP_SP_MASK          0x2FFE0000
#define BL_APP_SP_VALUE         0x20000000

/* CRC16/MODBUS: poly 0x8005 reflected (0xA001), init 0xFFFF, no final xor. */
#define BL_CRC16_INIT           0xFFFF
#define BL_CRC16_POLY_REFL      0xA001

/* ------------------------------------------------------------------------- */
/* 9. C-only conveniences                                                     */
/* ------------------------------------------------------------------------- */

#ifndef __ASSEMBLER__

#include <stdint.h>

#define BL_REG8(a)   (*(volatile uint8_t  *)(uintptr_t)(a))
#define BL_REG16(a)  (*(volatile uint16_t *)(uintptr_t)(a))
#define BL_REG32(a)  (*(volatile uint32_t *)(uintptr_t)(a))

/* Symbols supplied by ld/bootloader.ld. */
extern uint32_t __stack_top;
extern uint32_t __data_load, __data_start, __data_end;
extern uint32_t __bss_start,  __bss_end;
extern uint32_t __flash_image_end;

/* Supplied by src/vectors.S — the shared trampoline. */
void bl_vector_forward(void);

/* Supplied by src/start.S — the reset entry point. */
void Reset_Handler(void);

/* Provided by src/boot.c.  Must never return. */
void bl_main(void);

#endif /* __ASSEMBLER__ */

#endif /* BL_CONFIG_H */
