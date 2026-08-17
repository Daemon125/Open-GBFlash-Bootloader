/* boot.h — boot decision, application validation and handoff.
 *
 * Owner: comp:boot.  Implemented by src/boot.c.
 *
 * References (design authority): docs/DESIGN.md, §1 (vector handoff),
 * §3 (flash), §4 (boot-info record), §5 (boot decision and handoff).
 */

#ifndef BL_BOOT_H
#define BL_BOOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Flash / SRAM map — docs/DESIGN.md §3, §4                                   */
/* ------------------------------------------------------------------------- */

/* Bootloader code occupies 0x0000..0x3DFF = sectors 0..30 (31 x 512 B). */
#define BL_BOOT_LIMIT           0x00003E00u   /* exclusive; NEVER write below this */

/* Boot-info record: sector 31, alone. 14 bytes used, rest 0xFF. */
#ifndef BL_BOOTINFO_BASE
#define BL_BOOTINFO_BASE        0x00003E00u
#endif
#define BL_BOOTINFO_LEN         14u

/* Application: 0x4000 upward, exactly sector-aligned (0x4000 / 0x200 == 32). */
#ifndef BL_APP_BASE
#define BL_APP_BASE             0x00004000u
#endif

/* CH579 CodeFlash is 250 KB: 0x00000000..0x0003E7FF.  DataFlash starts 0x3E800. */
#ifndef BL_CODEFLASH_END
#define BL_CODEFLASH_END        0x0003E800u
#endif

/* The three addresses above, plus BL_UPDATE_MAGIC_ADDR below, are overridable
 * so that boot.c can be compiled natively (-DBL_HOST_TEST) with them pointed at
 * an ordinary buffer.  Nothing on the target ever overrides them. */

/* Largest application length the validator will accept.  The brief's rule is
 * "0x4000 + length must stay within CodeFlash"; the brief's sample code used a
 * much tighter 0x7600, which is barely above the shipping L15 image (0x7520)
 * and would reject any future firmware that grows. See NOTE 1 in boot.c.
 * Override at build time if you want the tighter policy. */
#ifndef BL_APP_MAX_LEN
#define BL_APP_MAX_LEN          (BL_CODEFLASH_END - BL_APP_BASE)   /* 0x3A800 */
#endif

/* An application must at minimum carry a full 36-entry vector table. */
#define BL_APP_MIN_LEN          0x90u

/* Flash sector granularity (erase unit), verified on silicon. */
#define BL_FLASH_SECTOR         0x200u

/* SRAM. Initial MSP for both bootloader and application is 0x20008000. */
#define BL_SRAM_BASE            0x20000000u
#define BL_SRAM_END             0x20008000u

/* The "initial SP looks like SRAM" gate: (sp & BL_SP_MASK) == BL_SP_VALUE.
 *
 * ONE DEFINITION, TWO HEADERS, DELIBERATELY.  proto.h defines this pair with an
 * identical replacement list, because it is portable and must not include this
 * header; boot.c includes boot.h unconditionally and proto.h only on the target,
 * so the copy that is always in scope has to live here.  An identical
 * redefinition is legal and silent, and the #ifndef makes the order harmless. */
#ifndef BL_SP_MASK
#define BL_SP_MASK              0x2FFE0000u
#endif
#ifndef BL_SP_VALUE
#define BL_SP_VALUE             0x20000000u
#endif

/* ------------------------------------------------------------------------- */
/* Update-request handoff word — docs/DESIGN.md §5                            */
/* ------------------------------------------------------------------------- */

/* The application writes this magic then issues SYSRESETREQ.  SRAM survives
 * SYSRESETREQ (measured), and nothing in the application's startup zeroes this
 * word (.data begins at 0x200000A0), so the bootloader MUST clear it after
 * consuming it or the device re-enters update mode on every reset forever. */
#ifndef BL_UPDATE_MAGIC_ADDR
#define BL_UPDATE_MAGIC_ADDR    0x20000090u
#endif
#define BL_UPDATE_MAGIC         0xAA55BB01u

/* ------------------------------------------------------------------------- */
/* Boot-info record layout at 0x3E00 (little-endian fields)                    */
/* ------------------------------------------------------------------------- */
/*   0x00  u16  marker      0xFFFF in every distributed image. NOT validated.
 *   0x02  4    "LFBG"
 *   0x06  u16  CRC16 of `length` bytes at 0x4000
 *   0x08  u32  application length
 *   0x0C  u16  CRC16 of record bytes 0x00..0x0B
 */
#define BL_HDR_OFF_MARKER       0x00u
#define BL_HDR_OFF_TAG          0x02u
#define BL_HDR_OFF_APPCRC       0x06u
#define BL_HDR_OFF_APPLEN       0x08u
#define BL_HDR_OFF_HDRCRC       0x0Cu
#define BL_HDR_CRC_COVERAGE     0x0Cu   /* CRC16 covers bytes 0x00..0x0B */

/* ------------------------------------------------------------------------- */
/* THE GATE SET — which fields of that record are inspected, and by WHOM.      */
/*                                                                             */
/* Two independent pieces of code look at the same 14 bytes:                   */
/*                                                                             */
/*   bl_app_valid()             (src/boot.c)  — the VALIDATOR. Decides whether */
/*                                              the device boots what is       */
/*                                              already in flash.              */
/*   hdr_check() + app_gates()  (src/proto.c) — the WRITER. Decides whether     */
/*                                              finalize answers 0x01 SUCCESS. */
/*                                                                             */
/* THEY MUST APPLY THE SAME GATES. Every divergence found so far has been a    */
/* real defect of one shape — a writer STRICTER than the validator NAKs a      */
/* legitimate image at finalize, after packet 1 has already erased sector 31;  */
/* a writer LOOSER than the validator answers SUCCESS for an image the boot    */
/* decision then refuses forever. Both leave the device in update mode.        */
/*                                                                             */
/* This bitmask makes the whole set comparable: each side declares what it     */
/* gates — BL_VALIDATOR_GATES here, BL_WRITER_GATES in proto.h — and the two   */
/* must be equal. The assertion sits at the foot of whichever header is        */
/* included second, so it fires in src/boot.c AND in host/check_headers.c.     */
/*                                                                             */
/* The declaration is not decorative. The one OPTIONAL gate — the marker — is  */
/* compiled from its bit at BOTH sites, so switching it on in one place fails  */
/* the build instead of passing silently. The other bits are documentation,    */
/* and a diff that removes a gate without clearing its bit is a diff that      */
/* lied.                                                                       */
/*                                                                             */
/* Spelled IDENTICALLY in proto.h (same tokens, no #ifndef): C permits the     */
/* redefinition silently only while the replacement lists match, so divergent  */
/* bit VALUES draw -Wmacro-redefined in every TU including both. That case is  */
/* the one the _Static_asserts CANNOT catch — both masks expand at the point   */
/* of use and would see whichever definition came last — which is why          */
/* host/check_headers.c also compares them at RUN time, snapshotting           */
/* BL_WRITER_GATES into a variable before boot.h can redefine anything.        */
/* ------------------------------------------------------------------------- */
#define BL_GATE_MARKER          0x01u   /* marker at 0x00 whitelisted         */
#define BL_GATE_TAG             0x02u   /* "LFBG" at 0x02                     */
#define BL_GATE_HDR_CRC         0x04u   /* record CRC at 0x0C over 0x00..0x0B */
#define BL_GATE_APPLEN          0x08u   /* BL_APP_MIN_LEN..BL_APP_MAX_LEN     */
#define BL_GATE_APP_CRC         0x10u   /* payload CRC at 0x06                */
#define BL_GATE_SP              0x20u   /* initial SP looks like SRAM         */
#define BL_GATE_RESETVEC        0x40u   /* reset vector Thumb + inside image  */

/* What bl_app_valid() gates.  BL_GATE_MARKER is deliberately ABSENT. */
#define BL_VALIDATOR_GATES      (BL_GATE_TAG | BL_GATE_HDR_CRC | \
                                 BL_GATE_APPLEN | BL_GATE_APP_CRC | \
                                 BL_GATE_SP | BL_GATE_RESETVEC)

/* Fires when boot.h is the SECOND of the pair to be included — host/
 * check_headers.c.  proto.h carries the mirror image for the other order,
 * which is what src/boot.c sees. */
#ifdef BL_WRITER_GATES
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(BL_WRITER_GATES == BL_VALIDATOR_GATES,
               "the updater and the boot decision must gate the same fields "
               "of the boot-info record (proto.h BL_WRITER_GATES vs boot.h "
               "BL_VALIDATOR_GATES)");
#elif defined(__GNUC__)
__extension__ _Static_assert(BL_WRITER_GATES == BL_VALIDATOR_GATES,
               "the updater and the boot decision must gate the same fields "
               "of the boot-info record");
#endif
#endif

/* ------------------------------------------------------------------------- */
/* GPIO — U22 user button, PB23, input pull-up, ACTIVE LOW.                    */
/* [CONFIRMED] from the application's own bsp_gpio_init (GPIOB_ModeCfg(1<<23,
 * IN_PU)) and bsp_button_scan (reads R32_PB_PIN bit 23, 1 == released).  PB22
 * is a DIFFERENT pin — the application's PCB-revision strap, and believed to be
 * the H1/ISP boot pad; never touched here.  */
/* ------------------------------------------------------------------------- */
#define BL_R32_PB_DIR           0x400010C0u
#define BL_R32_PB_PIN           0x400010C4u
#define BL_R32_PB_OUT           0x400010C8u
#define BL_R32_PB_CLR           0x400010CCu
#define BL_R32_PB_PU            0x400010D0u
#define BL_R32_PB_PD_DRV        0x400010D4u

#define BL_BTN_PIN_BIT          23u
#define BL_BTN_MASK             (1u << BL_BTN_PIN_BIT)

/* The button path is enabled: the pin, mode and active level are all confirmed.
 * Define BL_BUTTON_ENABLE=0 to compile it out entirely. */
#ifndef BL_BUTTON_ENABLE
#define BL_BUTTON_ENABLE        1
#endif

/* Whether PRIMASK is left CLEAR (interrupts globally enabled) when branching to
 * the application.  Default 1 = replicate cold-reset state. See NOTE 2 in
 * boot.c — setting this to 0 will leave the application unable to ever service
 * an interrupt, because its Keil __main startup never executes CPSIE I. */
#ifndef BL_APP_ENTRY_IRQ_ENABLED
#define BL_APP_ENTRY_IRQ_ENABLED 1
#endif

/* ------------------------------------------------------------------------- */
/* Update-mode loop timing                                                     */
/*                                                                             */
/* THESE ARE REAL MILLISECONDS, not iteration counts.  include/timebase.h runs  */
/* SysTick as a free-running 1 ms counter with its INTERRUPT DISABLED, so       */
/* nothing about the polled design changes: no vector is taken, NVIC_ISER is    */
/* written nowhere in the image, and vector 22 still cannot trampoline into an  */
/* application table that is mid-erase.  §5.1.2 requires SysTick to be in its   */
/* RESET STATE AT HANDOFF, which bl_jump_to_app()'s quiesce block discharges —  */
/* that clear must stay.                                                       */
/*                                                                             */
/* Both windows are measured from the LAST BYTE RECEIVED and are restarted by   */
/* any further inbound traffic, so time spent erasing, programming or spinning  */
/* inside bl_usb_tx() is never mistaken for receive silence.                    */
/* ------------------------------------------------------------------------- */

/* How much receive silence abandons a half-received frame (bl_proto_idle()).
 * The bounds it sits between: the shortest legitimate mid-frame gap is a host
 * stalling inside one 534-byte write (2.6 ms of wire time at 2 Mbaud), and the
 * upper bound is proto.h's 100 ms inter-frame spacing — exceed that and a host
 * that truncated a frame and reconnected inside the window has its init frames
 * swallowed, the "device appears dead" failure bl_proto_idle() exists to
 * prevent. */
#ifndef BL_PROTO_IDLE_MS
#define BL_PROTO_IDLE_MS        50u
#endif

/* How much receive silence must follow a successful 0x23 before the bootloader
 * hands the device to the freshly written application.
 *
 * proto.h's ORDERING CONTRACT: bl_proto_finalized() goes true DURING the
 * bl_proto_feed() call that parsed the 0x23, i.e. BEFORE the 16-byte ack has
 * left the endpoint.  Rebooting on the flag alone makes every host report
 * failure on a SUCCESSFUL update — FlashGBX "No response from device", the
 * unlocker UpdateError, getserial a TypeError on None — after which the user's
 * natural move is to run the updater again.
 *
 * What this window actually measures, stated precisely: 250 ms counted from
 * the LAST BYTE RECEIVED, restarted by any further inbound traffic.  It is a
 * fixed delay, not a proof of delivery — the ack is queued while the host is
 * already silent and waiting to read, so silence alone cannot confirm it was
 * collected.  It does not need to.  The ack is a single 16-byte packet armed
 * on EP2 IN and the host's ch34x driver has read URBs outstanding, so it is
 * taken within microseconds; 250 ms is four orders of magnitude of margin.
 * Restarting on inbound traffic is what makes the window useful beyond the
 * delay: it absorbs a finalize retransmission (h_final() is idempotent for the
 * same CRC) instead of rebooting into the middle of one.
 *
 * Upper bound: the unlocker closes the port right after the ack and sleeps
 * 0.8 s; FlashGBX sleeps 3 s.  250 ms sits well inside both, so the device is
 * back as the application before either host looks for it. */
#ifndef BL_BOOT_SETTLE_MS
#define BL_BOOT_SETTLE_MS       250u
#endif

/* Bytes drained from the USB layer per loop iteration.  Two bulk packets. */
#ifndef BL_UPDATE_RX_CHUNK
#define BL_UPDATE_RX_CHUNK      64u
#endif

/* HOW UPDATE MODE HANDS OVER TO A FRESHLY INSTALLED APPLICATION.
 *
 *   1 (default) — issue SYSRESETREQ and let the bootloader run again from a
 *                 real reset.  It re-validates and jumps through the very same
 *                 bl_app_valid() / bl_jump_to_app() path stage 2 proved.
 *   0           — call bl_jump_to_app() directly from the update loop.
 *
 * WHY THE RESET IS THE DEFAULT, and it is a USB argument, not a CPU one.
 *
 * A direct jump leaves the USB SIE exactly as update mode left it: enumerated,
 * D+ pull-up attached (RB_UD_PORT_EN set), and holding the address the host
 * assigned to the BOOTLOADER's CH340 device.  The application's own
 * USB_DeviceInit then resets the SIE and clears R8_USB_DEV_AD back to 0 — but
 * it never drops the pull-up, so the host sees no disconnect, keeps addressing
 * the device at the old address, and the device answers nothing.  The result is
 * a board that looks dead after a SUCCESSFUL update until it is physically
 * unplugged, which is precisely the report this design has to avoid.
 *
 * SYSRESETREQ resets the USB block, R8_UDEV_CTRL returns to 0, the pull-up is
 * removed for the ~25 ms the bootloader spends re-validating the image, and the
 * host sees a clean disconnect followed by the application enumerating.  That
 * this works on THIS silicon is not an inference: it is the exact mechanism the
 * application already uses to enter update mode, and the measured result is a
 * fresh CH340 enumeration and a new tty node.
 *
 * The reset costs one extra ~25 ms application CRC and nothing else.  The one
 * behavioural difference: if U22 is still physically held down at that instant,
 * the post-reset bootloader samples it and re-enters update mode (triple
 * blink).  That is correct behaviour, and 250 ms after a finalize nobody is
 * still holding the button.
 *
 * Set to 0 to compare the two on hardware; bl_jump_to_app() is wired either
 * way and is what the reset path ends up calling regardless. */
#ifndef BL_BOOT_VIA_RESET
#define BL_BOOT_VIA_RESET       1
#endif

/* ------------------------------------------------------------------------- */
/* Why the bootloader stayed in update mode                                    */
/* ------------------------------------------------------------------------- */
typedef enum {
    BL_REASON_NONE        = 0,  /* not in update mode (jumped to the app)     */
    BL_REASON_MAGIC       = 1,  /* application requested it via 0x20000090    */
    BL_REASON_BUTTON      = 2,  /* U22 held at power-on                       */
    BL_REASON_APP_INVALID = 3   /* no valid application present               */
} bl_reason_t;

/* Set exactly once by bl_main() before update mode is entered.  Read-only to
 * everyone else; useful for choosing an LED blink pattern. */
extern volatile uint8_t bl_boot_reason;

/* ------------------------------------------------------------------------- */
/* Interfaces                                                                  */
/* ------------------------------------------------------------------------- */

/* Provided by src/proto.c (portable, no MMIO).  CRC-16/MODBUS: reflected poly
 * 0xA001, init 0xFFFF, no final XOR, no output reflection. */
uint16_t bl_crc16(const uint8_t *p, uint32_t n);

/* Entry point, called from Reset_Handler in src/start.S after clock init.
 * Never returns. */
void bl_main(void);

/* Full application validity test.  Returns 1 if the boot-info record and the
 * application image both check out and the app's initial SP looks like SRAM. */
int bl_app_valid(void);

/* Load MSP and PC from the application vector table at 0x4000 and branch.
 * Never returns.  Does NOT validate — call bl_app_valid() first. */
void bl_jump_to_app(void) __attribute__((noreturn));

/* Sample the U22 button (PB23).  1 = pressed (pin LOW), 0 = released.
 * Debounced; costs ~18 ms.  Always returns 0 when BL_BUTTON_ENABLE == 0. */
int bl_button_held(void);

/* Update mode.  Brings up polled USB and the activity LED, runs the wire
 * protocol against the real flash driver, and hands the device to the firmware
 * it installs (see BL_BOOT_VIA_RESET).  Never returns.
 *
 * Under -DBL_HOST_TEST src/boot.c carries a weak fallback that simply spins, so
 * boot.c still links natively without usb.c / proto.c / flash.c / led.c. */
void bl_update_mode(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* BL_BOOT_H */
