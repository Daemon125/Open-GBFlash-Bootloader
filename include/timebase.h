/* timebase.h — a real millisecond clock for the polled update loop.
 *
 * Owner: comp:timebase.  Implemented by src/timebase.c.
 *
 * Design authority: docs/DESIGN.md §2 (SysTick free-running, interrupt off,
 * and cleared at handoff).  The 32 MHz / 1 ms numbers come from the shipping
 * application's own SysTick setup; the flash-write core stall this module has
 * to survive is measured on this silicon.
 *
 * ==========================================================================
 * WHY THIS EXISTS
 * ==========================================================================
 *
 * The alternative is timing everything by counting iterations of the update
 * loop, scaled by a polls-per-millisecond constant.  That constant is a guess
 * whatever value it takes: it depends on compiler output, on flash wait states,
 * and on which branch of bl_usb_poll() the loop happens to take.  Counting the
 * idle iteration out of build/bootloader.lst gives ~298 cycles, i.e. ~107
 * iterations/ms — so a plausible-looking 400 makes every timeout 3.7x too long,
 * turning a 50 ms framer window into ~186 ms and a 1.6 s LED period into ~6 s.
 * This module removes the guess.
 *
 * ==========================================================================
 * WHY SysTick IS SAFE HERE — the three constraints it does NOT break
 * ==========================================================================
 *
 * 1. NO INTERRUPT IS ENABLED.  TICKINT (SYST_CSR bit 1) stays 0 and the value
 *    written to SYST_CSR is ENABLE|CLKSOURCE only.  SysTick is exception 15,
 *    not an NVIC line, so nothing here goes anywhere near NVIC_ISER — the
 *    check_image.py assertion that NVIC_ISER appears in no literal pool is
 *    untouched, and so is the reason for it (vector 22 trampolines into the
 *    application's table, which is ERASED mid-update).
 *
 * 2. THE DESIGN STAYS POLLED.  bl_time_ms() is a counter read.  It never
 *    waits, never spins and never blocks, so the "no path may run longer than
 *    ~10 ms without calling bl_usb_poll()" budget in usb.h is unaffected.
 *
 * 3. THE APPLICATION STILL GETS SysTick IN ITS RESET STATE.  §5.1.2's
 *    requirement is about what is true AT HANDOFF, not about never touching
 *    the peripheral.  bl_jump_to_app() (boot.c) already writes ALL THREE of
 *    SYST_CSR = 0, SYST_RVR = 0 and SYST_CVR = 0 inside its interrupts-masked
 *    quiesce block, before the MSR MSP / BX; that is what makes this module
 *    invisible to the application, and all three must stay.  bl_time_deinit()
 *    below does exactly the same thing and is therefore redundant with boot.c
 *    on the handoff path — it exists for callers that want the peripheral
 *    released without a handoff, and it is gc-sectioned out of the shipping
 *    image, so boot.c's three stores are the only ones that actually run.
 *    See "SYST_RVR at handoff" in src/timebase.c for why the third store was
 *    added and why it is not optional.
 *
 * ==========================================================================
 * THE ONE CONTRACT ON THE CALLER
 * ==========================================================================
 *
 * bl_time_ms() must be called at least once every BL_TIME_MAX_GAP_MS (524 ms).
 * It is a lazy accumulator: the 24-bit hardware counter is the only thing that
 * runs on its own, and 24 bits at 32 MHz is 524.288 ms before the counter has
 * wrapped all the way round and a delta becomes ambiguous.
 *
 * HOW MUCH SLACK THERE ACTUALLY IS.  The margin is set by the LONGEST run
 * between two calls, not by the cost of an idle iteration.  The longest runs
 * are:
 *
 *     ~33 ms   bl_proto's finalize re-reads and re-CRCs the whole application
 *              out of flash (9 cycles/byte for the read copy + 26 for
 *              bl_crc16_update = 35, over 30,496 B — both loops counted off
 *              build/bootloader.lst in src/boot.c's certification block).
 *              This one is CUT UP, one clock read per <=512-byte page, so it
 *              is not actually a single gap; it is listed at full size
 *              because that is what it would be if the cut were removed.
 *     ~4.6 ms  one 512-byte sector erase + program, during which the core is
 *              PAUSED (measured on this silicon)
 *     ~2 ms    at most TWO bl_usb_poll() calls inside bl_usb_tx()'s stall wait
 *              (BL_USB_TX_MAX_GAP_MS, certified in usb.h, which derives why
 *              the count is two and not one)
 *     ~8.75 ms the USB PLL settle busy-wait in bl_usb_init(), once per entry
 *              into update mode (BL_USB_INIT_MAX_GAP_MS, also in usb.h)
 *
 * Worst realistic sum is ~48 ms against 524 ms: a factor of about 11.
 * Comfortable, but not vast, and easy to violate — a busy-wait timeout of a few
 * hundred nominal milliseconds is enough on its own.  A future caller that waits
 * on anything for longer than a few tens of milliseconds must call bl_time_ms()
 * while it waits, and should certify its own bound the way usb.h does.
 *
 * WHAT GOING OVER COSTS.  A lost lap makes the accumulator UNDER-count by up to
 * 524 ms, so windows that span the gap fire LATE, not early — bl_proto_idle at
 * 574 ms instead of 50 ms, which reads on the wire as a device that has died.
 *
 * Within that bound NO TIME IS EVER LOST, which is the whole point and is why
 * this module does not simply reload every 1 ms and count COUNTFLAGs.  On this
 * silicon the core is PAUSED for up to 2.4 ms by a sector erase and ~36 us per
 * programmed word — a 512-byte sector is ~4.6 ms of wall clock in which the
 * loop calls nothing at all.  COUNTFLAG is one
 * sticky bit cleared by reading it, so any such gap would collapse several
 * milliseconds into one tick, and 60 packets of a 30 KB firmware would drift by
 * a quarter of a second.  Sampling a free-running counter and accumulating the
 * DIFFERENCE has no such failure: whatever the gap, up to the 524 ms bound, the
 * elapsed cycles are recovered exactly.
 */

#ifndef BL_TIMEBASE_H
#define BL_TIMEBASE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Clock                                                                      */
/* ------------------------------------------------------------------------- */

/* Fsys.  start.S leaves the clock at internal RC / SYS_MOD = 2 = 32 MHz and
 * nothing in the bootloader changes it (hard constraint).  This is deliberately
 * a private copy rather than an include of bl_config.h: boot.c includes boot.h
 * first, and boot.h respells several bl_config.h macros, so a header included
 * from boot.c must not drag bl_config.h in behind it.  src/timebase.c includes
 * BOTH and _Static_asserts that they agree, which is the same belt-and-braces
 * src/led.c uses for the port-B addresses. */
#ifndef BL_TIME_FSYS_HZ
#define BL_TIME_FSYS_HZ         32000000u
#endif

/* 32 core cycles per microsecond.  A power of two, which is what lets the
 * cycles -> microseconds step be a shift and a mask instead of a division
 * (there is no divide instruction on Cortex-M0 and no libgcc under -nostdlib).
 * It also keeps 32000 — which would land inside check_image.py's "no literal in
 * application space" window, 0x3E00..0xB51F — out of the image entirely. */
#define BL_TIME_CYCLES_PER_US   ((BL_TIME_FSYS_HZ) / 1000000u)
#define BL_TIME_US_SHIFT        5u      /* log2(BL_TIME_CYCLES_PER_US)        */
#define BL_TIME_US_PER_MS       1000u

/* ------------------------------------------------------------------------- */
/* SysTick (ARMv6-M B3.3).  Exception 15, NOT an NVIC line.                   */
/*                                                                            */
/* Spelled BL_TIME_SYST_* rather than BL_SYST_*: src/boot.c already defines    */
/* BL_SYST_CSR and BL_SYST_CVR for its handoff teardown, and boot.c includes   */
/* this header.  Distinct names mean the two cannot collide, benignly or       */
/* otherwise, and boot.c's copies stay the authority on the handoff path.      */
/* ------------------------------------------------------------------------- */

#define BL_TIME_SYST_CSR        0xE000E010u     /* control and status         */
#define BL_TIME_SYST_RVR        0xE000E014u     /* reload value, 24 bits      */
#define BL_TIME_SYST_CVR        0xE000E018u     /* current value, 24 bits     */

#define BL_TIME_SYST_MAX        0x00FFFFFFu     /* the counter is 24 bits     */

#define BL_TIME_CSR_ENABLE      (1u << 0)
#define BL_TIME_CSR_TICKINT     (1u << 1)       /* NEVER SET — see note 1     */
#define BL_TIME_CSR_CLKSOURCE   (1u << 2)       /* 1 = processor clock        */

/* What bl_time_init() writes.  TICKINT is absent by construction, not by
 * accident: with it clear SysTick raises no exception, needs no vector and
 * needs no NVIC enable, so the polled design and the vector-22 trampoline
 * hazard are both untouched.  The application's own systick_init (0xAB3C)
 * writes 7 here — ENABLE|TICKINT|CLKSOURCE — which is the difference. */
#define BL_TIME_CSR_RUN         (BL_TIME_CSR_ENABLE | BL_TIME_CSR_CLKSOURCE)

/* The longest a caller may go between bl_time_ms() calls, in milliseconds:
 * one full trip of the 24-bit counter.  524 at 32 MHz. */
#define BL_TIME_MAX_GAP_MS      (((BL_TIME_SYST_MAX + 1u) \
                                  / BL_TIME_CYCLES_PER_US) / BL_TIME_US_PER_MS)

/* ------------------------------------------------------------------------- */
/* Interface                                                                  */
/* ------------------------------------------------------------------------- */

/* Start the clock: SYST_RVR = 0x00FFFFFF (free-running, 524.288 ms per lap),
 * SYST_CVR = 0, SYST_CSR = ENABLE|CLKSOURCE with TICKINT CLEAR.  Zeroes the
 * millisecond accumulator, so bl_time_ms() returns 0 immediately afterwards.
 *
 * IDEMPOTENT: a second call while the clock is already running does nothing at
 * all — it does NOT restart the epoch.  That matters because more than one
 * module calls it (bl_update_mode() for its timeouts, bl_led_init() so the LED
 * works even if the loop has not), and a restart under a caller that is holding
 * a timestamp would make its elapsed-time arithmetic jump.  bl_time_deinit()
 * is what allows a subsequent bl_time_init() to start a fresh epoch.
 *
 * Costs three MMIO stores and one load.  Never blocks. */
void bl_time_init(void);

/* Milliseconds since the bl_time_init() that started the current epoch.
 *
 * Monotonic and non-decreasing; wraps at 2^32 ms (49.7 days), so callers must
 * compare differences — (uint32_t)(now - then) >= limit — and never absolute
 * values.  Cheap enough to call every loop iteration: one 24-bit counter read,
 * a subtract, a shift, a mask and, on average once per thousand calls, one
 * subtract of 1000.
 *
 * SAFE BEFORE bl_time_init(), AND BY CONSTRUCTION.  If the clock is not running
 * — before the first bl_time_init(), or after bl_time_deinit() — this returns
 * the frozen accumulator (0 before any epoch has started) and touches SysTick
 * not at all.  That is an explicit `tb_running` test, NOT a consequence of a
 * stopped counter reading CVR as 0: ARMv6-M B3.3.3 leaves SYST_CVR's reset
 * value UNKNOWN and says nothing about what a disabled SysTick reads.
 * src/led.c's bl_led_set_pattern() depends on this and says so.
 *
 * A caller that needs to know whether time is actually passing must therefore
 * not infer it from the return value: a frozen clock and an idle clock look
 * identical.  bl_usb_tx() is the worked example — its deadline is paired with a
 * poll-count backstop precisely because a frozen clock can never reach it.
 *
 * Never blocks. */
uint32_t bl_time_ms(void);

/* Stop the clock and return SysTick to a clean state: SYST_CSR = 0 (which also
 * disables it before anything else is touched), SYST_CVR = 0 (which clears
 * COUNTFLAG), SYST_RVR = 0.
 *
 * REDUNDANT WITH bl_jump_to_app(), WHICH ALREADY CLEARS ALL THREE OF SYST_CSR,
 * SYST_RVR AND SYST_CVR — do not remove those.  bl_time_deinit() is
 * gc-sectioned out of the shipping image, so the handoff path's three stores
 * are the only guarantee the application gets SysTick in its reset state, and
 * tools/check_image.py section 10(d) asserts all three against the linked
 * image.  This function is for releasing the peripheral without handing off.
 * After it, bl_time_ms() freezes and the next bl_time_init() starts a fresh
 * epoch at 0.  Never blocks. */
void bl_time_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* BL_TIMEBASE_H */
