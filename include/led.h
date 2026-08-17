/* led.h — update-mode activity LED on PB12.
 *
 * Owner: comp:led.  Implemented by src/led.c.
 *
 * Design authority: docs/DESIGN.md §7.  The pin, drive strength and active
 * level below were read out of the shipping application's own disassembly.
 *
 * ==========================================================================
 * THE THREE RULES.  Each one is a real defect if broken.
 * ==========================================================================
 *
 * 1. THE NORMAL BOOT PATH MUST NEVER TOUCH PB12.
 *    bl_led_init() may be called from bl_update_mode() and from nowhere else.
 *    Do NOT call it from bl_main(), from Reset_Handler, or from the path that
 *    validates the application and hands off.  With PB12 left as an input the
 *    observable still holds: "LED lights ~500 ms after power-on" means the
 *    APPLICATION booted, which is how the device owner tells a good boot from a
 *    bad one.
 *
 *    bl_led_poll() and bl_led_set_pattern() are inert until bl_led_init() has
 *    run — they touch no GPIO register — so a stray call on the handoff path
 *    cannot light the pin.  That guard is belt-and-braces, not a licence to
 *    call them there.
 *
 * 2. THE LATCH IS PRESET DARK BEFORE THE PIN BECOMES AN OUTPUT.
 *    PB12 is ACTIVE LOW: PB_CLR lights it, PB_OUT darkens it.  bl_led_init()
 *    writes the dark state into R32_PB_OUT while the pin is still an input and
 *    only then sets R32_PB_DIR.  The other order drives whatever the latch
 *    happened to hold and produces a visible flash on entry to update mode.
 *
 * 3. bl_led_poll() IS NON-BLOCKING, ON EVERY PATH.
 *    It never delays and never busy-waits.  boot.c's bl_delay_us()/
 *    bl_delay_ms() must not appear in led.c: the update loop services USB by
 *    polling, and a blocking delay there stalls USB servicing long enough to
 *    drop host transactions, fail enumeration, or stall a firmware transfer
 *    mid-flash.  usb.h states the budget: no path may run longer than ~10 ms
 *    without calling bl_usb_poll().
 *
 * ==========================================================================
 * TIMEBASE — real milliseconds
 * ==========================================================================
 *
 * Slots are timed with bl_time_ms() (include/timebase.h): a free-running
 * SysTick counter with its INTERRUPT DISABLED, sampled by polling.  No vector,
 * no NVIC enable, nothing to disturb the polled design or the vector-22
 * trampoline hazard, and SysTick is still handed to the application in its
 * reset state because bl_jump_to_app() clears it at handoff.
 *
 * NOT a count of bl_led_poll() calls scaled by a polls-per-millisecond
 * constant, and do not reintroduce one: that constant is unmeasurable in
 * practice, and getting it wrong by the factor the disassembly actually shows
 * (~107 iterations/ms, not the plausible-looking 400) stretches the 1.6 s
 * pattern to about six seconds.  At that speed the double-blink stops reading
 * as the idiom FlashGBX's CLI tells the user to watch for ("the blue LED
 * labeled ACT should now keep blinking twice").
 *
 * The period is BL_LED_SLOTS * BL_LED_SLOT_MS = 1600 ms of real time and needs
 * no hardware correction procedure.
 *
 * WHAT A BUSY LOOP DOES TO THE PATTERN.  A slot ends when at least
 * BL_LED_SLOT_MS have elapsed since the previous one ended, so a stretch where
 * bl_led_poll() is not reached — a sector erase pauses the core for up to
 * 2.4 ms, a 512-byte sector for ~4.6 ms — pushes the next transition out by
 * that much and no further.  The pattern still visibly slows during a firmware
 * transfer, which reads as "busy" and is useful; it just no longer slows by a
 * factor of four when the device is idle.  There is deliberately no catch-up:
 * bl_led_poll() advances AT MOST ONE SLOT per call, so a long stall can never
 * turn into a burst of fast transitions.
 */

#ifndef BL_LED_H
#define BL_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* ------------------------------------------------------------------------- */

/* One pattern slot in milliseconds, and the number of slots in a pattern.
 * 16 x 100 ms = a 1.6 s repeat, which is about right for "blinking twice".
 * Real milliseconds off bl_time_ms(), not poll counts. */
#ifndef BL_LED_SLOT_MS
#define BL_LED_SLOT_MS          100u
#endif
#define BL_LED_SLOTS            16u

/* The whole pattern, for anyone reasoning about how long the idiom takes. */
#define BL_LED_PERIOD_MS        ((BL_LED_SLOTS) * (BL_LED_SLOT_MS))

/* ------------------------------------------------------------------------- */
/* Patterns                                                                   */
/* ------------------------------------------------------------------------- */

/* A pattern is a 16-bit word, one bit per slot, bit 0 played first.
 * 1 = lit (PB12 driven LOW), 0 = dark (PB12 driven HIGH).  It repeats.
 *
 * Written LSB-first below, so the diagram reads left-to-right in time order.
 *
 *                                 slot 0123456789ABCDEF          */
#define BL_LED_PAT_DOUBLE   0x0005u  /* # #             = blink,blink,pause  */
#define BL_LED_PAT_TRIPLE   0x0015u  /* # # #           = blink x3, pause    */
#define BL_LED_PAT_FAST     0x5555u  /* # # # # # # # # = 5 Hz, no pause     */
#define BL_LED_PAT_SLOW     0x00FFu  /* ########        = 0.8 s on/off       */

/* Which reason gets which:
 *
 *   BL_REASON_MAGIC (1)       DOUBLE  two 100 ms blinks then 1.3 s dark.
 *                                     The firmware family's established idiom
 *                                     and what FlashGBX's CLI tells the user
 *                                     to look for: "the blue LED labeled ACT
 *                                     should now keep blinking twice".  This
 *                                     is the ordinary host-requested update.
 *   BL_REASON_BUTTON (2)      TRIPLE  three blinks then 1.1 s dark.  Same
 *                                     idiom, one extra blink: "you did this
 *                                     with the button, not the host."
 *   BL_REASON_APP_INVALID (3) FAST    continuous 5 Hz, never pausing.  Reads
 *                                     as an alarm rather than a heartbeat,
 *                                     because it is one: there is no valid
 *                                     application to go back to.
 *   anything else             SLOW    0.8 s on, 0.8 s off.  Update mode with
 *                                     no reason recorded — should not happen;
 *                                     if it is ever seen, bl_boot_reason was
 *                                     not set before bl_update_mode().
 *
 * All four are distinguishable at a glance: count the blinks, or note that
 * FAST never pauses and SLOW is a lazy on/off.  None can be confused with the
 * application's steady-on, or with a dark (dead / no bootloader) board. */

/* ------------------------------------------------------------------------- */
/* Interface                                                                  */
/* ------------------------------------------------------------------------- */

/* Configure PB12 as output push-pull 5 mA, latched DARK before the direction
 * change (rule 2), and arm the pattern state machine.
 *
 * Also calls bl_time_init(), which is idempotent and does not restart an
 * already-running clock: this module cannot blink without a timebase, and
 * making it start its own means the LED works regardless of whether the update
 * loop got there first.  bl_time_init() touches SysTick only — no GPIO, no
 * interrupt — so it breaks none of the three rules above.
 *
 * CALL FROM bl_update_mode() ONLY (rule 1).  Idempotent.  Costs no delay. */
void bl_led_init(void);

/* Select the pattern for a bl_boot_reason value (a bl_reason_t, passed as the
 * uint8_t that boot.h publishes).  Restarts the pattern from slot 0 so the
 * first blink is immediate.  Safe to call before bl_led_init(), and safe to
 * call repeatedly.  Never blocks. */
void bl_led_set_pattern(uint8_t reason);

/* Advance the pattern.  Call once per iteration of the update loop, next to
 * bl_usb_poll().  NON-BLOCKING: a bl_time_ms() read and one unsigned compare on
 * every call, plus one GPIO store on the call that crosses a slot boundary.
 * Never waits.  Missing calls, or calling at an irregular rate, only delays
 * transitions — at most one slot advances per call, so a stall cannot produce a
 * burst.  Calling it more often than once per slot costs the compare and
 * nothing else. */
void bl_led_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* BL_LED_H */
