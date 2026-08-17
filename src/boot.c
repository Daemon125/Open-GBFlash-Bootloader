/* boot.c — boot decision, application validation, handoff, and update mode.
 *
 * Owner: comp:boot.  Interface in include/boot.h.
 *
 * Called from Reset_Handler (src/start.S) AFTER minimal clock init.  This file
 * never returns to its caller: it either branches into the application or
 * enters update mode.
 *
 * Design authority: docs/DESIGN.md §1 (vector handoff), §3 (flash), §4 (the
 * boot-info record) and §5 (the boot decision and the handoff).  The GPIO map
 * and the button were read out of the shipping application.
 *
 * -------------------------------------------------------------------------
 * NOTE 1 — two deliberate deviations from the brief's sample app_valid().
 *
 *   (a) The sample gates on `marker != 0xFFFF && marker != 0x5555`; this file
 *       does not validate the marker at all.  The stored header CRC covers
 *       bytes 0x00..0x0B *including* the marker, so a whitelist of two literal
 *       values adds no security and can only reject an otherwise-valid future
 *       image.  Nothing here ever writes the marker (stamping 0x5555 would
 *       invalidate the stored CRC and requires setting flash bits, which
 *       programming cannot do).  The validator's and the writer's gate sets are
 *       declared as BL_VALIDATOR_GATES (boot.h) and BL_WRITER_GATES (proto.h)
 *       and asserted equal by every TU including both headers, so the whitelist
 *       can only ever be switched on in both at once.
 *
 *   (b) The sample caps length at 0x7600, only 0xE0 above the shipping L15 image
 *       (0x7520) — that would reject the very next firmware revision.  The
 *       brief's rule is used instead: nonzero, at least large enough for a
 *       36-entry vector table, and 0x4000 + length must stay inside CodeFlash.
 *       Build with -DBL_APP_MAX_LEN=0x7600 to restore the tighter policy.
 *
 * -------------------------------------------------------------------------
 * NOTE 2 — PRIMASK state at application entry.
 *
 *   On ARMv6-M PRIMASK is 0 (interrupts globally ENABLED) out of reset; only
 *   the NVIC enables are clear.  The application's startup is Keil's
 *   (Reset_Handler 0x40A4 -> SystemInit 0x46F0 -> __main 0x4090 ->
 *   __scatterload 0x4270 -> main 0xA654) and executes no
 *   CPSIE I anywhere on that path.  So handing over with PRIMASK set would
 *   leave the application's USB (IRQ6), UART1 (IRQ11) and SysTick interrupts
 *   unable to fire: the device would enumerate nothing and look bricked while
 *   being perfectly healthy.
 *
 *   Interrupts are therefore masked (CPSID I) across the whole quiesce
 *   sequence AND the MSP switch, and unmasked in the same instruction stream
 *   immediately before BX — no exception can be taken while MSP is in flux, and
 *   the application starts in cold-reset PRIMASK state.  The emitted order must
 *   be `msr msp, rN` -> `cpsie i` -> `bx rM`.  Nothing checks that
 *   automatically; if you touch the asm block, re-read `make disasm` and
 *   confirm the three instructions in that order.
 *
 *   Remaining differences from a true cold reset, all benign because the
 *   bootloader runs fully polled and enables no interrupt source (§5.1.2):
 *     - The vector table at 0x0000 is the bootloader's, not the application's.
 *       That is the whole point of the design; exceptions are trampolined into
 *       the application's own table at 0x4000 by the shared IPSR dispatcher
 *       (src/vectors.S), which is transparent to the handlers.
 *     - Clock configuration has already been applied by start.S (internal RC,
 *       Fsys 32 MHz), and the application's SystemInit re-applies it
 *       identically.
 *     - PB23 has been left configured as input-with-pull-up.  bsp_gpio_init
 *       configures it identically, so this is a no-op difference.
 *     - R8_RESET_STATUS reset-cause bits are not disturbed here.
 *     - The bootloader has used some stack below 0x20008000; it is dead by the
 *       time MSP is reloaded, and the application re-establishes SP itself.
 *
 * -------------------------------------------------------------------------
 * NOTE 3 — the one place this file departs from the brief.
 *
 *   The brief says to boot freshly written firmware with bl_app_valid() +
 *   bl_jump_to_app().  The default build validates exactly as asked and then
 *   issues SYSRESETREQ instead of branching, because a direct branch out of
 *   update mode leaves the USB D+ pull-up attached and the host addressing the
 *   BOOTLOADER's device — the application re-inits the SIE to address 0 without
 *   the host ever seeing a disconnect, and the board looks dead after a
 *   SUCCESSFUL update until it is physically unplugged.  The reset re-runs this
 *   file from the top and reaches bl_jump_to_app() through the proven path.
 *   See BL_BOOT_VIA_RESET in boot.h; -DBL_BOOT_VIA_RESET=0 is the whole change
 *   if the direct branch is ever measured to re-enumerate cleanly.
 *
 *   The boot decision, bl_app_valid(), the reset-vector guard, the magic clear
 *   and the handoff sequence are installed and verified on the device.  Treat
 *   them as load-bearing.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include "boot.h"
#ifndef BL_HOST_TEST
/* Include order matters only in that boot.h comes FIRST: proto.h respells
 * BL_APP_BASE and BL_FLASH_END with identical replacement lists (benign), and
 * guards BL_APP_MAX_LEN with #ifndef so boot.h's definition — the one
 * bl_app_valid() enforces — wins.  host/check_headers.c exists to fail the
 * build if those three ever stop agreeing. */
#include "proto.h"
#include "flash.h"
#include "usb.h"
#include "led.h"
#include "timebase.h"
#endif

/* --------------------------------------------------------------------- */
/* Small MMIO helpers                                                     */
/* --------------------------------------------------------------------- */

#define REG32(a)  (*(volatile uint32_t *)(uintptr_t)(a))

static inline uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void wr32(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

/* Cortex-M0 core registers used at handoff. */
#define BL_SYST_CSR   0xE000E010u   /* SysTick control/status                */
#define BL_SYST_RVR   0xE000E014u   /* SysTick reload value                  */
#define BL_SYST_CVR   0xE000E018u   /* SysTick current value                 */
#define BL_NVIC_ICER  0xE000E180u   /* interrupt clear-enable (32 lines)     */
#define BL_NVIC_ICPR  0xE000E280u   /* interrupt clear-pending               */
#define BL_SCB_ICSR   0xE000ED04u   /* PENDSVCLR (bit27), PENDSTCLR (bit25)  */
#define BL_SCB_AIRCR  0xE000ED0Cu   /* VECTKEY | SYSRESETREQ                 */
#define BL_AIRCR_SYSRESETREQ 0x05FA0004u

/* --------------------------------------------------------------------- */
/* State                                                                  */
/* --------------------------------------------------------------------- */

volatile uint8_t bl_boot_reason = (uint8_t)BL_REASON_NONE;

/* --------------------------------------------------------------------- */
/* Crude busy-wait.  Fsys = 32 MHz (internal RC, SYS_MOD = 2).            */
/*                                                                        */
/* Deliberately approximate and biased LONG: used only for a GPIO pull-up */
/* settle and for button debounce, where over-waiting is harmless and     */
/* under-waiting is not.                                                  */
/*                                                                        */
/* Counted off the emitted code, not estimated.  Both functions inline    */
/* into bl_button_held(), where the decrement is a seven-instruction,     */
/* 12-cycle loop (ldr 2 + cmp 1 + beq-not-taken 1 + ldr 2 + subs 1        */
/* + str 2 + b-taken 3).  At 4 iterations per nominal microsecond that is */
/* 48 cycles where 32 MHz makes a microsecond 32, so every wait here runs */
/* 1.5x nominal: the 2 ms pull-up settle is really ~3.0 ms and            */
/* bl_button_held() samples ~1.5 ms apart.  Both err LONG, the safe       */
/* direction for a settle and a debounce alike.                           */
/*                                                                        */
/* THE OVERRUN CANNOT COST A TIMEBASE LAP.  Both callers run on the boot  */
/* path, before bl_time_init(); with tb_running == 0 bl_time_ms() returns */
/* the frozen accumulator and never reads SysTick (timebase.h), so there  */
/* is no counter in flight to wrap.  No timer peripheral is used and      */
/* SysTick deliberately is not either: the boot path must reach           */
/* bl_jump_to_app() having disturbed nothing (§5.1.2).                    */
/* --------------------------------------------------------------------- */
#if BL_BUTTON_ENABLE
static void bl_delay_us(uint32_t us)
{
    volatile uint32_t n = us * 4u;
    while (n != 0u) {
        n--;
    }
}

static void bl_delay_ms(uint32_t ms)
{
    while (ms != 0u) {
        bl_delay_us(1000u);
        ms--;
    }
}
#endif

/* --------------------------------------------------------------------- */
/* U22 button — PB23, input pull-up, ACTIVE LOW  [CONFIRMED]              */
/*                                                                        */
/* Equivalent to the vendor's GPIOB_ModeCfg(1<<23, GPIO_ModeIN_PU):       */
/*     R32_PB_PD_DRV &= ~pin;  R32_PB_PU |= pin;  R32_PB_DIR &= ~pin;     */
/*                                                                        */
/* Only bit 23 is touched.  In particular PB22 (the application's PCB-    */
/* revision strap and boot key, and believed to be the H1/ISP boot pad)   */
/* and PB12 (the activity LED) are never written here.                    */
/* --------------------------------------------------------------------- */
#if BL_BUTTON_ENABLE
static void bl_button_init(void)
{
    wr32(BL_R32_PB_PD_DRV, rd32(BL_R32_PB_PD_DRV) & ~BL_BTN_MASK);
    wr32(BL_R32_PB_PU,     rd32(BL_R32_PB_PU)     |  BL_BTN_MASK);
    wr32(BL_R32_PB_DIR,    rd32(BL_R32_PB_DIR)    & ~BL_BTN_MASK);

    /* bsp_gpio_init settles 1 ms after a mode change before sampling a strap;
     * give the pull-up twice that (really ~3.0 ms — see bl_delay_us). */
    bl_delay_ms(2u);
}

int bl_button_held(void)
{
    /* 16 consecutive LOW samples, really ~1.5 ms apart (see bl_delay_us), so
     * the button must be held ~24 ms.  A single glitch rejects. */
    uint32_t i;

    for (i = 0u; i < 16u; i++) {
        if ((rd32(BL_R32_PB_PIN) & BL_BTN_MASK) != 0u) {
            return 0;               /* high == released (pull-up) */
        }
        bl_delay_ms(1u);
    }

    /* One final sample after the last delay. */
    return ((rd32(BL_R32_PB_PIN) & BL_BTN_MASK) == 0u) ? 1 : 0;
}
#else  /* !BL_BUTTON_ENABLE */
int bl_button_held(void)
{
    return 0;
}
#endif /* BL_BUTTON_ENABLE */

/* --------------------------------------------------------------------- */
/* Application validation                                                 */
/* --------------------------------------------------------------------- */

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int bl_app_valid(void)
{
    const uint8_t *h = (const uint8_t *)(uintptr_t)BL_BOOTINFO_BASE;
    uint32_t len;
    uint32_t sp;
    uint32_t pc;

#if (BL_VALIDATOR_GATES & BL_GATE_MARKER)
    /* 0. The marker at 0x00.  NOT COMPILED TODAY — the bit is clear in both
     *    BL_VALIDATOR_GATES and proto.h's BL_WRITER_GATES, and the headers
     *    refuse to build if only one of them sets it.  See NOTE 1a. */
    {
        uint16_t marker = rd16le(&h[BL_HDR_OFF_MARKER]);
        if (marker != 0xFFFFu && marker != 0x5555u) {
            return 0;
        }
    }
#endif

    /* 1. Magic tag "LFBG" at 0x02..0x05. */
    if (h[BL_HDR_OFF_TAG + 0u] != (uint8_t)'L' ||
        h[BL_HDR_OFF_TAG + 1u] != (uint8_t)'F' ||
        h[BL_HDR_OFF_TAG + 2u] != (uint8_t)'B' ||
        h[BL_HDR_OFF_TAG + 3u] != (uint8_t)'G') {
        return 0;
    }

    /* 2. Record CRC16 over bytes 0x00..0x0B, stored little-endian at 0x0C.
     *    This authenticates the marker at 0x00 as a side effect, which is why
     *    the marker itself is not separately whitelisted (NOTE 1a). */
    if (bl_crc16(h, BL_HDR_CRC_COVERAGE) != rd16le(&h[BL_HDR_OFF_HDRCRC])) {
        return 0;
    }

    /* 3. Length sanity: nonzero, big enough for a vector table, and the image
     *    must fit inside CodeFlash above the application base (NOTE 1b).
     *    Checked BEFORE the payload CRC so a corrupt length can never make the
     *    CRC loop walk off the end of the address map. */
    len = rd32le(&h[BL_HDR_OFF_APPLEN]);
    if (len < (uint32_t)BL_APP_MIN_LEN) {
        return 0;                       /* also catches len == 0 */
    }
    if (len > (uint32_t)BL_APP_MAX_LEN) {
        return 0;
    }
    if ((BL_APP_BASE + len) > BL_CODEFLASH_END) {
        return 0;                       /* redundant, kept explicit on purpose */
    }

    /* 4. The application's initial SP must look like an SRAM pointer.  This is
     *    the documented check and it is deliberately performed on vector entry
     *    0 rather than the reset vector: the bootloader loads MSP from here
     *    itself, because the hardware vector fetch at 0x0000 lands in the
     *    bootloader's own table (§5.1.3). */
    sp = REG32(BL_APP_BASE);
    if ((sp & BL_SP_MASK) != BL_SP_VALUE) {
        return 0;
    }

    /* 4b. The application's RESET VECTOR must be a Thumb pointer that lands
     *     inside the application image.  NOT OPTIONAL.
     *
     *     bl_jump_to_app() BXes to this word directly; it is the one vector the
     *     bootloader dispatches by hand rather than through
     *     bl_vector_forward(), which range-checks everything else.  Without
     *     this gate an application accidentally linked for base 0x0000 instead
     *     of 0x4000 — the layout a bootloader-less GBFlash ships with — passes
     *     the SP mask and both CRC gates while carrying a reset vector like
     *     0x00A5, and the bootloader branches into its OWN code at an arbitrary
     *     offset.  That can land past the argument guards of
     *     bl_flash_erase_sector() / bl_flash_program_word(), unlocking
     *     CodeFlash and erasing below 0x3E00 — recoverable only with the H1
     *     jumper.  flash.c's entry-point guards are correct; handing out an
     *     unvalidated PC is what defeats them, so the fix belongs here. */
    pc = REG32(BL_APP_BASE + 4u);
    if ((pc & 1u) == 0u) {
        return 0;                       /* Thumb bit clear -> instant HardFault */
    }
    if ((pc & ~1u) < (uint32_t)BL_APP_BASE ||
        (pc & ~1u) >= ((uint32_t)BL_APP_BASE + len)) {
        return 0;                       /* outside the image it belongs to */
    }

    /* 5. Payload CRC16 over `len` bytes at 0x4000, stored little-endian at
     *    0x06.  26 cycles/byte, so ~24.8 ms at 32 MHz for the stock
     *    30,496-byte image (see the certification block below).  This is what
     *    makes an interrupted update fail safe. */
    if (bl_crc16((const uint8_t *)(uintptr_t)BL_APP_BASE, len)
            != rd16le(&h[BL_HDR_OFF_APPCRC])) {
        return 0;
    }

    return 1;
}

/* --------------------------------------------------------------------- */
/* Handoff                                                                */
/* --------------------------------------------------------------------- */

#ifdef BL_HOST_TEST
/* Supplied by the native test harness; stands in for the branch. */
extern void bl_host_jump_to_app(uint32_t sp, uint32_t pc);
#endif

void bl_jump_to_app(void)
{
    uint32_t sp = REG32(BL_APP_BASE);           /* vector 0: initial MSP  */
    uint32_t pc = REG32(BL_APP_BASE + 4u);      /* vector 1: reset, Thumb */

#ifdef BL_HOST_TEST
    bl_host_jump_to_app(sp, pc);
    for (;;) {
    }
#else
    /* Mask exceptions for the whole quiesce + switch sequence. */
    __asm volatile ("cpsid i" ::: "memory");

    /* Return the core to as close to cold-reset state as software can.
     *
     * THE THREE SysTick STORES MUST NOT BE REMOVED.  Update mode runs SysTick
     * as a free-running counter with its interrupt disabled
     * (include/timebase.h), so on the BL_BOOT_VIA_RESET=0 path SysTick is live
     * when this function is entered.  §5.1.2 requires SysTick to be in its
     * reset state AT HANDOFF, not that the bootloader never touches it, and
     * these three discharge that: ENABLE/TICKINT/CLKSOURCE cleared, the reload
     * value returned to zero, and COUNTFLAG cleared as a side effect of writing
     * CVR.  (On the default BL_BOOT_VIA_RESET=1 path SYSRESETREQ has already
     * done it in hardware — that redundancy is not an argument for dropping
     * them.)
     *
     * RVR is written because bl_time_init() loads it with 0x00FFFFFF, and zero
     * rather than any "architectural reset value" because ARMv6-M leaves
     * SYST_RVR's reset value UNKNOWN (B3.3.3).  Zero is the one safe value:
     * with ENABLE clear it means nothing, and if a future edit ever sets ENABLE
     * without first loading RVR the counter reloads to 0 and never wraps,
     * instead of free-running at a period nobody chose. */
    wr32(BL_SYST_CSR, 0u);                      /* SysTick off             */
    wr32(BL_SYST_RVR, 0u);                      /* no reload value left    */
    wr32(BL_SYST_CVR, 0u);                      /* and its COUNTFLAG clear */
    wr32(BL_NVIC_ICER, 0xFFFFFFFFu);            /* all 20 IRQs disabled    */
    wr32(BL_NVIC_ICPR, 0xFFFFFFFFu);            /* and none pending        */
    wr32(BL_SCB_ICSR, (1u << 27) | (1u << 25)); /* PENDSVCLR | PENDSTCLR   */

    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");

    /* Switch MSP, then unmask, then branch.  The reset vector already carries
     * the Thumb bit (§5.1), so no ORR #1 is applied: forcing the bit would mask
     * a genuinely malformed table instead of faulting visibly.
     *
     * ORDER IS DELIBERATE — see NOTE 2.  MSR MSP first, so PRIMASK is still set
     * while MSP is in flux; CPSIE I last before BX, leaving the application in
     * cold-reset PRIMASK state.  Nothing may touch the stack between the MSR
     * and the BX; a single asm block is what guarantees that. */
    __asm volatile (
        "msr   msp, %0      \n\t"
#if BL_APP_ENTRY_IRQ_ENABLED
        "cpsie i            \n\t"
#endif
        "bx    %1           \n\t"
        :
        : "r" (sp), "r" (pc)
        : "memory");

    /* Unreachable. */
    for (;;) {
    }
#endif /* BL_HOST_TEST */
}

/* ====================================================================== */
/* Update mode.                                                           */
/*                                                                        */
/* Bring the polled CH340-emulation USB device up, run the wire protocol  */
/* over it against the real flash driver, and hand the device to the      */
/* firmware it just installed.  This function never returns.              */
/*                                                                        */
/* Shape of one iteration:                                                */
/*                                                                        */
/*   bl_usb_poll()   services at most ONE hardware event and contains no  */
/*                   wait loop, so the loop has no unbounded path.  With  */
/*                   no host it spins on R8_USB_INT_FG — a bootloader     */
/*                   that hangs on a missing host is indistinguishable    */
/*                   from a brick.                                        */
/*   bl_led_poll()   advances the blink pattern; non-blocking by contract */
/*                   (led.h rule 3).                                      */
/*   session check   a tty close/reopen does not reset the bus, so usb.c  */
/*                   throws its staging away and bumps a counter.  The    */
/*                   framer must be told, or it parses the tail of the    */
/*                   previous session's frame as the head of this one.    */
/*   rx -> feed -> tx                                                     */
/*   idle timeout    bl_proto_idle()                                      */
/*   boot check      the hands-off handover                               */
/*                                                                        */
/* THE ~10 ms RULE (usb.h).  No path may run longer than about 10 ms      */
/* without calling bl_usb_poll(), because a USB bus reset must be         */
/* answered inside the host's reset-recovery window.  Two places in this  */
/* loop spend real time inside bl_proto_feed():                           */
/*                                                                        */
/*   - a 0x24 data packet erases one 512-byte sector (~1.4 ms) and        */
/*     programs 128 words (~3.5 ms) plus a read-back: ~6 ms, in budget.   */
/*   - a 0x23 finalize CRCs the whole application back out of flash and   */
/*     commits the boot-info record — ~10-15 ms for a 30 KB image, which  */
/*     does exceed the budget.  Unavoidable (the verification cannot be   */
/*     interleaved without re-entering proto.c mid-handler) and harmless: */
/*     the transfer flag LATCHES, so late service is degraded not lost,   */
/*     and RB_UC_INT_BUSY makes the SIE auto-NAK meanwhile.  Once per     */
/*     update, at the very end.                                           */
/*                                                                        */
/* NOTHING ON THIS PATH CAN FAULT.  Memory touched: the USB register      */
/* block, the flash controller block, PB12's GPIO registers, the four     */
/* SysTick words at 0xE000E010 (timebase.h — counter enabled, TICKINT     */
/* clear, so it is a counter and not an interrupt source), .bss, and      */
/* CodeFlash at or above 0x3E00 (flash.c refuses anything else at every   */
/* entry point).  No vector is taken: IRQ6 is masked at the NVIC by       */
/* bl_usb_init() and NVIC_ISER is written nowhere in the image, so        */
/* vector 22 can never trampoline into the application table that this    */
/* very loop is in the middle of erasing (§5.1.2, usb.h header comment).  */
/* ====================================================================== */

#ifdef BL_HOST_TEST
/* The native harness links neither usb.c, proto.c, flash.c nor any MMIO; keep
 * the stage-0 placeholder so boot.c still compiles standalone off-target. */
__attribute__((weak)) void bl_update_mode(void)
{
    for (;;) {
        __asm volatile ("" ::: "memory");
    }
}
#else

/* --------------------------------------------------------------------- */
/* The real flash driver, injected into proto.c                           */
/*                                                                        */
/* proto.c names no flash address below BL_IMAGE_BASE and range-checks    */
/* every op before it calls through this table; flash.c then range-checks */
/* again at its own entry points and refuses anything below 0x3E00 or     */
/* past 0x3E7FF.  Two independent guards, neither trusting the other —    */
/* that is the whole reason the ops are injected rather than called       */
/* directly.  Do not "simplify" either layer's checks away.               */
/* --------------------------------------------------------------------- */

/* Read-back for proto.c's verification passes.  flash.h has no read entry
 * point because CodeFlash is plainly memory-mapped at its own address, so this
 * is a byte copy — but it carries the SAME window guard as the write paths on
 * purpose: proto.c hands this pointer arithmetic it computed itself, and a
 * verification that silently read the wrong place would report success over a
 * bad image.  A byte loop (not memcpy) because the image links -nostdlib;
 * -fno-builtin and -fno-tree-loop-distribute-patterns keep GCC from
 * synthesising the call this file could not resolve. */
static int bl_update_flash_read(uint32_t addr, void *buf, uint32_t len)
{
    const uint8_t *src;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t i;

    /* THIS IS WHERE THE FINALIZE VERIFY IS CUT UP FOR THE TIME BASE — see the
     * certification block below bl_update_reply.  h_final()'s flash_verify()
     * re-reads and re-CRCs the WHOLE application through this op at 9 + 26
     * cycles/byte, which over a maximum-size image is ~262 ms in ONE stretch,
     * half of timebase.h's 524 ms budget.  proto.c calls this once per
     * <=512-byte page, so one clock read here turns that image-proportional gap
     * into ~0.6 ms per page and removes the image size from the contract.  Do
     * not "optimise" it away, and do not move it below the guards: the guards
     * can return early, and a gap must be bounded on the failing paths too. */
    (void)bl_time_ms();

    if (buf == 0 || len == 0u)                     return -1;
    if (addr < (uint32_t)BL_BOOTINFO_BASE)         return -1;  /* our own code */

    /* The window's upper edge, in TWO tests, in this order — matching flash.c's
     * range_writable().  A single `len > BL_CODEFLASH_END - addr` is not
     * enough: for any addr past BL_CODEFLASH_END the unsigned subtraction wraps
     * to nearly 4 GB and every length passes.
     *
     * This is the READ path and it has no third layer behind it — proto.c
     * range-checks, this function range-checks, flash.c is not involved.  A
     * read of unmapped space is a BusFault -> HardFault -> a trampoline into
     * the application vector table this update has already ERASED -> .Lhang
     * with CodeFlash unlocked, the one fault here that needs the H1 jumper to
     * recover from. */
    if (addr >= (uint32_t)BL_CODEFLASH_END)        return -1;  /* past the map */
    if (len > (uint32_t)BL_CODEFLASH_END - addr)   return -1;  /* runs off it  */

    src = (const uint8_t *)(uintptr_t)addr;
    for (i = 0u; i < len; i++) {
        dst[i] = src[i];
    }
    return 0;
}

/* BL_DRY_RUN is a proto.c-level option and short-circuits inside fl_erase() /
 * fl_program() / fl_readable(), so this table is bound unconditionally and the
 * dry-run image simply never calls through it.  Build one with
 *     make EXTRA_CFLAGS=-DBL_DRY_RUN
 * which must reach proto.c as well as this file — hence a build flag rather
 * than a #define here. */
static const bl_flash_ops bl_update_flash_ops = {
    bl_flash_erase_sector,
    bl_flash_program,
    bl_update_flash_read
};

/* ~2.2 KB of .bss (two 512-byte page buffers, a 520-byte payload buffer and
 * the 531-byte rescan window).  Static, not automatic: the linker script
 * reserves only 4 KB of stack and ASSERTs .bss against it, so this belongs
 * where the link can see it. */
static bl_proto bl_update_proto;

/* Transmit whatever the framer just built.  bl_usb_tx() pumps the poll loop
 * itself and gives up after BL_USB_TX_TIMEOUT_MS rather than blocking forever,
 * so a host that stops reading cannot wedge the bootloader.  A short return
 * means the response was truncated; every host treats that as a timeout and
 * retries, which the protocol permits (a repeated seq_no and a repeated packet
 * index are never errors). */
static void bl_update_reply(int r)
{
    if (r > 0) {
        (void)bl_usb_tx(bl_update_proto.tx, bl_update_proto.tx_len);

        /* KEEP THE TIME BASE INSIDE ITS ONE CONTRACT.  timebase.h recovers
         * every elapsed cycle across a gap, but only up to one lap,
         * BL_TIME_MAX_GAP_MS (524 ms at 32 MHz).  One 64-byte receive chunk can
         * hold four 16-byte frames, so without this the whole batch would sit
         * between two clock readings.  One read per transmitted frame bounds
         * the gap by ONE frame's work, which is what makes the certification
         * below a per-operation argument rather than a per-chunk one. */
        (void)bl_time_ms();
    }
}

/* ---------------------------------------------------------------------
 * THE TIMEBASE CONTRACT FOR THIS LOOP, CERTIFIED AGAINST THE EMITTED CODE.
 *
 * timebase.h's one obligation on its callers: no more than BL_TIME_MAX_GAP_MS
 * (524 ms at 32 MHz — one lap of the 24-bit counter) may pass between two
 * bl_time_ms() calls.  Past that a lap is lost, the accumulator UNDER-counts,
 * and every window spanning the gap fires up to half a second LATE:
 * bl_proto_idle() at 574 ms instead of 50 — precisely the "device appears
 * dead" failure it exists to prevent — and the handover at ~774 ms, at the
 * edge of the unlocker's 0.8 s sleep.
 *
 * usb.c clocks its own wait and certifies BL_USB_TX_MAX_GAP_MS (2 ms, usb.h)
 * as the gap it can contribute; the figures here are for what boot.c itself
 * does between two clock reads.
 *
 * Counted off build/bootloader.lst rather than assumed.  `make disasm`
 * regenerates it; the addresses are for the current image and will move, the
 * cycle counts should not.
 *
 *     bl_crc16_update inner loop   26 cycles/byte
 *         (function at 0x7C0, loop body 0x7C8..0x7EE: cmp 1 + bne-taken 3
 *          + ldrb 2 + 6 ALU + ldrh 2 + 4 ALU + ldrh 2 + 2 ALU + b-taken 3)
 *     bl_update_flash_read copy     9 cycles/byte
 *         (function at 0x160, loop body 0x18A..0x192: ldrb 2 + strb 2
 *          + adds 1 + cmp 1 + bne-taken 3)
 *     sector erase ~1.4 ms, word program ~27 us    (flash.h, silicon)
 *
 * The four things on this path that take real time, and what bounds each:
 *
 *   (1) h_data(): one erase + 128 word programs + a 512-byte read-back
 *       ~= 1.4 + 3.5 + 0.6 = 5.5 ms.  Bounded by the SECTOR, not the image.
 *   (2) commit_header(): 131 word programs plus two read-backs, ~4.2 ms.
 *       Also sector-bounded.
 *   (3) h_final()'s flash_verify(): it reads each page through
 *       bl_update_flash_read() and then CRCs it, so 9 + 26 = 35 cycles/byte
 *       over the whole application, i.e. ~262 ms for a maximum-size image in
 *       one stretch (~33 ms for the stock 30,496-byte one).  That single gap
 *       would be half the budget on its own, so it is cut up:
 *       bl_update_flash_read() reads the clock on every call and proto.c calls
 *       it once per <=512-byte page, giving ~0.6 ms per page.
 *   (4) bl_app_valid()'s payload CRC, read straight out of memory-mapped
 *       flash with no read op in the middle: 26 cycles/byte, so ~24.8 ms for
 *       the stock image.  This one IS proportional to the image and is NOT
 *       cut up — it is installed and proven on the device, and bl_main() calls
 *       it before the time base exists at all.  It is therefore the largest gap
 *       this loop can produce, and what the assertion is written against.
 *
 * BL_UPDATE_FLASH_MAX_GAP_MS covers (1)-(3) — the longest run of flash work
 * between two clock reads, 5.5 ms measured, 16 ms asserted — and the sum below
 * adds it to (4) and to usb.c's own certified figure even though the three are
 * not concurrent.  The result is 194 + 16 + 2 = 212 ms against a 524 ms
 * budget, so the conclusion survives the 2x pessimism for flash wait states
 * asserted below (424 < 524), which is the one factor none of these cycle
 * counts includes.
 * --------------------------------------------------------------------- */
#define BL_UPDATE_CRC_CYCLES_PER_BYTE   26u   /* bl_crc16_update, counted above */
#define BL_UPDATE_FLASH_MAX_GAP_MS      16u   /* longest erase/program stretch  */

/* 194 ms at BL_APP_MAX_LEN = 0x3A800 and Fsys 32 MHz.  Derived, not written
 * down, so that raising the accepted image size cannot silently break the
 * contract: it moves this figure and the assertion re-checks it. */
#define BL_UPDATE_APP_CRC_MAX_GAP_MS \
    (((uint32_t)BL_APP_MAX_LEN * BL_UPDATE_CRC_CYCLES_PER_BYTE) \
     / ((uint32_t)BL_TIME_CYCLES_PER_US * 1000u))

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(BL_UPDATE_APP_CRC_MAX_GAP_MS
                   + (uint32_t)BL_UPDATE_FLASH_MAX_GAP_MS
                   + (uint32_t)BL_USB_TX_MAX_GAP_MS
                   < (uint32_t)BL_TIME_MAX_GAP_MS,
               "the longest run of update-mode work between two bl_time_ms() "
               "calls must fit inside one lap of the timebase counter "
               "(BL_TIME_MAX_GAP_MS)");
/* And the margin, not just the inequality: a bound that only just fits is a
 * bound that has not accounted for flash wait states. */
_Static_assert(2u * (BL_UPDATE_APP_CRC_MAX_GAP_MS
                     + (uint32_t)BL_UPDATE_FLASH_MAX_GAP_MS
                     + (uint32_t)BL_USB_TX_MAX_GAP_MS)
                   < (uint32_t)BL_TIME_MAX_GAP_MS,
               "the worst-case gap must still fit with 2x pessimism for flash "
               "wait states");
#endif

/* Hand over to the freshly written application.  See BL_BOOT_VIA_RESET in
 * boot.h for why a reset is the default and a direct branch is not. */
static void bl_update_handover(void)
{
#if BL_BOOT_VIA_RESET
    /* Belt and braces: bl_main() already cleared this word on the way in, but
     * the reset we are about to issue runs bl_main() again and a stale magic
     * would drop straight back into update mode. */
    REG32(BL_UPDATE_MAGIC_ADDR) = 0u;

    __asm volatile ("cpsid i" ::: "memory");
    __asm volatile ("dsb" ::: "memory");
    REG32(BL_SCB_AIRCR) = BL_AIRCR_SYSRESETREQ;
    __asm volatile ("dsb" ::: "memory");
    for (;;) {
    }
#else
    bl_jump_to_app();                   /* never returns */
#endif
}

void bl_update_mode(void)
{
    uint8_t  rx[BL_UPDATE_RX_CHUNK];
    uint32_t session;
    uint32_t last_rx;           /* ms stamp of the last inbound byte         */
    uint32_t settle_at;         /* ms stamp the settle window (re)started at */
    uint8_t  armed   = 0u;      /* an update finished; settle window running */
    uint8_t  refused = 0u;      /* ...and the image failed bl_app_valid()    */

    /* Start the 1 ms time base BEFORE anything that measures a timeout.
     * SysTick, interrupt DISABLED — see the timing section of boot.h, and
     * bl_jump_to_app() for the clear that hands SysTick back in reset state. */
    bl_time_init();
    last_rx   = bl_time_ms();
    settle_at = last_rx;

    bl_usb_init();
    session = bl_usb_session();

    /* led.h rule 1: bl_led_init() is called HERE and nowhere else.  On the
     * handoff path PB12 stays an input, which is what preserves the stage-2
     * observable "LED lit ~500 ms after power-on == the application booted". */
    bl_led_init();
    bl_led_set_pattern(bl_boot_reason);

    bl_proto_reset(&bl_update_proto);
    bl_proto_bind(&bl_update_proto, &bl_update_flash_ops);

    for (;;) {
        uint32_t n;
        uint32_t now_session;
        uint32_t now_ms;

        bl_usb_poll();
        bl_led_poll();

        /* One clock reading per iteration on the idle path (the receive path
         * re-reads it after the handlers, which is the whole point of taking
         * it there).  Every window below is a comparison of unsigned
         * DIFFERENCES, so the 32-bit millisecond counter wrapping every
         * ~49.7 days changes nothing. */
        now_ms = bl_time_ms();

        /* A new host session (bus reset, or a tty close/reopen seen as the
         * ch34x line-reconfiguration request) means usb.c has thrown the bulk
         * staging away.  Resynchronise the framer rather than parsing across
         * the seam.  bl_proto_reset() preserves the bound flash ops.
         *
         * IF AN UPDATE COMPLETED IN THE SESSION THAT JUST ENDED, HAND OVER
         * HERE, before the reset clears `finalized`.  Carrying it forward as a
         * latched flag instead would leave it set part-way through the NEXT
         * session, and FlashGBX sleeps a whole second before retrying a NAKed
         * packet — long enough for a settle window armed by the PREVIOUS update
         * to expire in the middle of the new one.  Nothing is lost by not
         * waiting: the session ended, so there is no host left to drain the ack
         * to, and the completed update is real either way. */
        now_session = bl_usb_session();
        if (now_session != session) {
            session = now_session;

            if (!refused && bl_proto_finalized(&bl_update_proto)) {
                if (bl_app_valid()) {
                    bl_update_handover();       /* never returns */
                }
                bl_boot_reason = (uint8_t)BL_REASON_APP_INVALID;
                bl_led_set_pattern((uint8_t)BL_REASON_APP_INVALID);
            }

            bl_proto_reset(&bl_update_proto);
            /* `finalized` is false again, so the guard it justified is void. */
            last_rx   = now_ms;
            settle_at = now_ms;
            armed     = 0u;
            refused   = 0u;
        }

        n = bl_usb_rx(rx, (uint32_t)sizeof rx);

        if (n != 0u) {
            uint32_t i;

            for (i = 0u; i < n; i++) {
                bl_update_reply(bl_proto_feed(&bl_update_proto, rx[i]));
            }

            /* Stamp AFTER the handlers, not before.  A 0x24 spends ~6 ms
             * erasing and programming and a 0x23 ~33 ms re-CRCing the image out
             * of flash, and a stalled host can hold bl_usb_tx() for
             * BL_USB_TX_TIMEOUT_MS on top; none of that is receive silence.
             *
             * Restarting the settle window here is the second half of the rule:
             * the host is still talking, and this may BE a finalize
             * retransmission — h_final() answers those idempotently — so the
             * device must never hand over in the middle of one.  settle_at is
             * only read while `armed`, so setting it unconditionally is free. */
            now_ms    = bl_time_ms();
            last_rx   = now_ms;
            settle_at = now_ms;
        } else {
            /* Without this, a host killed part-way through a 0x24 frame leaves
             * a legal header with up to 522 bytes still outstanding (518
             * payload + the 4-byte outro, for a full 512-byte data frame).
             * The framer is then CORRECT to keep waiting, and it eats whatever
             * arrives next to satisfy the count — including the user's next
             * connection attempt, whose two 16-byte init frames total 32 bytes.
             * Both hosts then report "No device found." and the board looks
             * dead until it is unplugged.  bl_proto_feed()'s rescan cannot see
             * this: a frame that is still waiting has not failed.  Only time
             * distinguishes it.
             *
             * Re-stamping rather than latching means it also fires periodically
             * while merely idle, where it costs one subtract and one compare
             * and clears the intro shift register so a stale partial marker
             * cannot combine with the next burst. */
            if ((uint32_t)(now_ms - last_rx) >= (uint32_t)BL_PROTO_IDLE_MS) {
                last_rx = now_ms;
                bl_update_reply(bl_proto_idle(&bl_update_proto));
            }
        }

        /* ---- protocol-level session restart ----------------------------- */
        /* A 0x21 init frame calls session_reset() inside proto.c, which clears
         * `finalized`.  That is a session boundary every bit as real as the USB
         * one above, and `armed` / `refused` must not survive it.  Both
         * survivals are reachable when a host sends a second 0x21 on the SAME
         * tty, without closing it, after an update completed — FlashGBX does
         * exactly that:
         *
         *   (a) `armed` survives.  More than BL_BOOT_SETTLE_MS of silence
         *       between that 0x21 and the first data packet fires the settle
         *       window while the PREVIOUS image is still intact, so
         *       bl_app_valid() passes and the handover issues SYSRESETREQ,
         *       aborting the second update mid-flight.
         *   (b) `refused` survives.  Once packet 1 has erased sector 31 there
         *       is no valid boot-info, bl_app_valid() fails and latches
         *       `refused` (FlashGBX sleeps ~1 s after a NAK, four times the
         *       settle window).  `refused` then suppresses the handover for the
         *       rest of the USB session: the second update answers 0x01 SUCCESS
         *       and the device sits in update mode until it is replugged.
         *
         * Neither is destructive — the ordering invariant means no partially
         * written image is ever branched into — but both are bad behaviour on a
         * path a real host takes.
         *
         * DETECTED THROUGH proto.c's PUBLIC API, not its internals.  `armed`
         * and `refused` are only ever set while bl_proto_finalized() is true,
         * and the only two things that pull it false again are session_reset()
         * (a 0x21) and bl_proto_reset() — whose only call site is the USB seam
         * above, which clears both flags itself.  So "armed or refused, yet the
         * framer reports not finalized" means precisely "a 0x21 restarted the
         * session under us".  If proto.c ever grows a bl_proto_session()
         * counter mirroring bl_usb_session(), switch to comparing that.
         *
         * The LED pattern is deliberately NOT restored, for the same reason the
         * USB seam does not restore it: BL_REASON_APP_INVALID is true while it
         * is displayed, and the next handover or failure sets it again. */
        if ((armed || refused) && !bl_proto_finalized(&bl_update_proto)) {
            armed     = 0u;
            refused   = 0u;
            settle_at = last_rx;
        }

        /* ---- hands-off handover ---------------------------------------- */

        /* proto.h's ORDERING CONTRACT: this flag went true INSIDE the feed
         * call above, before the ack reached the wire.  Arm a settle window
         * (BL_BOOT_SETTLE_MS of receive silence — see boot.h for exactly what
         * that does and does not prove) instead of rebooting on the flag, or
         * every host reports failure on a successful update and the user's
         * next move is to run the updater again. */
        if (!armed && !refused && bl_proto_finalized(&bl_update_proto)) {
            armed     = 1u;
            settle_at = now_ms;
        }

        if (armed &&
            (uint32_t)(now_ms - settle_at) >= (uint32_t)BL_BOOT_SETTLE_MS) {
            /* Validate what was actually written before branching into it.
             * proto.c has already read the image back and matched its CRC, so
             * this should never fail — but bl_app_valid() applies two gates
             * proto.c does not (the stored header CRC, and the reset vector's
             * Thumb bit and range), and "should never fail" is not a reason to
             * jump into an unvalidated PC on a device that needs a case opened
             * to recover. */
            if (bl_app_valid()) {
                bl_update_handover();       /* never returns */
            }

            /* Stay in update mode and say so.  `refused` stops the ~25 ms CRC
             * being re-run every iteration while `finalized` remains true; the
             * next session clears it, and h_data() NAKs every data packet
             * until then anyway, so nothing is lost by refusing to retry. */
            armed          = 0u;
            refused        = 1u;
            bl_boot_reason = (uint8_t)BL_REASON_APP_INVALID;
            bl_led_set_pattern((uint8_t)BL_REASON_APP_INVALID);
        }
    }
}
#endif /* BL_HOST_TEST */

/* --------------------------------------------------------------------- */
/* Entry point                                                            */
/* --------------------------------------------------------------------- */

void bl_main(void)
{
    uint32_t magic;
    bl_reason_t reason = BL_REASON_NONE;

    /* ---- Step 1: consume the update request. -------------------------
     * FIRST, before anything else can fault, and cleared unconditionally.
     * SRAM survives SYSRESETREQ (measured) and the application never writes 0
     * here, so a bootloader that fails to clear this word leaves the device
     * stuck in update mode forever — which is exactly the state the real
     * device was found in.  The clear is unconditional rather than guarded by
     * the comparison so that a partially-written or garbage value can never
     * persist either. */
    magic = REG32(BL_UPDATE_MAGIC_ADDR);
    REG32(BL_UPDATE_MAGIC_ADDR) = 0u;

    if (magic == BL_UPDATE_MAGIC) {
        reason = BL_REASON_MAGIC;
    }

    /* ---- Step 2: U22 held at power-on. -------------------------------
     * PB23, input pull-up, active low.  Sampled only when step 1 did not
     * already decide, so a magic-word boot never pays the debounce time. */
#if BL_BUTTON_ENABLE
    if (reason == BL_REASON_NONE) {
        bl_button_init();
        if (bl_button_held()) {
            reason = BL_REASON_BUTTON;
        }
    }
#endif

    /* ---- Step 3: validate the application. --------------------------- */
    if (reason == BL_REASON_NONE) {
        if (bl_app_valid()) {
            bl_boot_reason = (uint8_t)BL_REASON_NONE;
            bl_jump_to_app();               /* never returns */
        }
        reason = BL_REASON_APP_INVALID;
    }

    bl_boot_reason = (uint8_t)reason;
    bl_update_mode();                       /* never returns */
}
