/* timebase.c — free-running SysTick as a millisecond clock, interrupt DISABLED.
 *
 * Owner: comp:timebase.  Interface, rationale and the caller's one contract:
 * include/timebase.h — read it first.
 *
 * ---------------------------------------------------------------------------
 * The hardware
 * ---------------------------------------------------------------------------
 * Standard ARMv6-M SysTick, CONFIRMED present and usable on this part by the
 * shipping application itself: its
 * systick_init at 0xAB3C is a hand-inlined SysTick_Config(32000) —
 *
 *     SYST_RVR = 0x00007CFF (31999)   -> 32000 core cycles
 *     SYST_CVR = 0
 *     SYST_CSR = 7  (ENABLE | TICKINT | CLKSOURCE = processor clock)
 *
 * — and its handler at 0x46B8 increments a 1 ms tick.  That is the third
 * independent confirmation of Fsys = 32 MHz, and it settles the two things this
 * module needs: CLKSOURCE = 1 selects a real 32 MHz processor clock, and the
 * counter behaves as the architecture specifies.
 *
 * We use the same peripheral with two differences: TICKINT stays 0 (no
 * exception, no vector, no NVIC line — timebase.h note 1), and the reload is
 * the full 24-bit range instead of 1 ms.
 *
 * ---------------------------------------------------------------------------
 * Why the reload is 0x00FFFFFF and not 32000-1
 * ---------------------------------------------------------------------------
 * With a 1 ms reload the only way to count elapsed milliseconds without an
 * interrupt is COUNTFLAG (SYST_CSR bit 16), and COUNTFLAG is ONE STICKY BIT
 * THAT CLEARS ON READ.  It answers "has the counter wrapped since you last
 * looked", not "how many times".  Any gap longer than 2 ms between polls
 * therefore loses time — and on this silicon such gaps are routine, not
 * hypothetical: the core is PAUSED (not merely stalled) for up to 2.4 ms by a
 * sector erase and ~36 us per programmed word, so writing one 512-byte sector
 * is ~4.6 ms during which nothing in the loop runs at all
 * (measured on this silicon — the measurement says in terms not to time the
 * session from a SysTick-driven millisecond counter).
 *
 * Sampling a free-running counter and accumulating the DIFFERENCE has no such
 * hole.  The counter counts DOWN and reloads at 0, so the elapsed count between
 * two samples is exactly
 *
 *     (previous - current) mod (RELOAD + 1)
 *
 * which is correct across any number of reloads as long as fewer than one full
 * lap has passed.  RELOAD = 0x00FFFFFF makes one lap 16,777,216 cycles =
 * 524.288 ms, so the caller's obligation is "call bl_time_ms() at least twice a
 * second" and every erase and program blackout is recovered exactly.  (Whether
 * SysTick itself keeps counting while the core is paused is not documented
 * either way; if it stops, this module simply under-counts that stall, which is
 * the safe direction — every timeout built on it gets longer, never shorter.)
 *
 * ---------------------------------------------------------------------------
 * Why the accumulation goes through microseconds
 * ---------------------------------------------------------------------------
 * Cortex-M0 has no divide instruction, and -nostdlib means no libgcc, so
 * cycles/32000 cannot be a division.  32 cycles is exactly 1 us at 32 MHz and
 * 32 is a power of two, so the cycles -> microseconds step is a shift and a
 * mask, and only the microseconds -> milliseconds step needs a subtract loop
 * against 1000 — which runs zero or one times on a normal call.
 *
 * The staging has a second, unobvious benefit.  tools/check_image.py forbids any
 * literal in 0x3E00..0xB51F (application space) that is not individually
 * whitelisted, because a stray pointer into the region being erased is the one
 * unrecoverable class of bug in this design.  32000 (0x7D00) and 31999 (0x7CFF)
 * both sit inside that window; 1000 (0x3E8) and 31 do not.  So this arithmetic
 * adds NO new whitelist entry.
 *
 * ---------------------------------------------------------------------------
 * SYST_RVR at handoff
 * ---------------------------------------------------------------------------
 * bl_jump_to_app() (boot.c) clears SYST_CSR, SYST_RVR and SYST_CVR in its
 * quiesce block — all THREE, which is what the handoff contract requires, what
 * this module relies on, and what tools/check_image.py section 10(d) asserts
 * against the linked image.  Zero rather than a nominal "reset value" because
 * ARMv6-M B3.3.3 leaves SYST_RVR's reset value UNKNOWN: there is nothing else to
 * restore.
 *
 * bl_time_deinit() clears them too, for callers that release the peripheral
 * without handing off.  boot.c's three stores are not this file's to change and
 * must not be removed: the handoff path has to stand on its own, and
 * bl_time_deinit() is gc-sectioned out of the shipping image.
 */

#include "bl_config.h"
#include "timebase.h"

/* ------------------------------------------------------------------------- */
/* Build-time checks.  These cost nothing in the image.                       */
/* ------------------------------------------------------------------------- */

/* timebase.h keeps a private copy of Fsys so it can be included from boot.c
 * without dragging bl_config.h in behind boot.h.  This is the proof the two
 * never drift — the same pattern src/led.c uses for the port-B addresses. */
_Static_assert((uint32_t)BL_FSYS_HZ == (uint32_t)BL_TIME_FSYS_HZ,
               "BL_TIME_FSYS_HZ disagrees with bl_config.h's BL_FSYS_HZ");

/* The shift-and-mask conversion is only exact if a microsecond is a whole,
 * power-of-two number of core cycles.  At 32 MHz it is 32. */
_Static_assert(BL_TIME_CYCLES_PER_US * 1000000u == BL_TIME_FSYS_HZ,
               "Fsys is not a whole number of cycles per microsecond");
_Static_assert((BL_TIME_CYCLES_PER_US & (BL_TIME_CYCLES_PER_US - 1u)) == 0u,
               "cycles-per-microsecond must be a power of two");
_Static_assert((1u << BL_TIME_US_SHIFT) == BL_TIME_CYCLES_PER_US,
               "BL_TIME_US_SHIFT disagrees with BL_TIME_CYCLES_PER_US");

/* The counter is 24 bits, and the reload we program is the whole range. */
_Static_assert(BL_TIME_SYST_MAX == 0x00FFFFFFu, "SysTick is a 24-bit counter");

/* TICKINT must not be in the value we write to SYST_CSR.  This is the one
 * assertion in this file that is a safety property rather than arithmetic: with
 * TICKINT set, SysTick would take exception 15, vectors.S would trampoline it
 * into the application's vector table — which is ERASED during an update — and
 * the core would spin in .Lhang with CodeFlash unlocked. */
_Static_assert((BL_TIME_CSR_RUN & BL_TIME_CSR_TICKINT) == 0u,
               "SysTick must never raise an exception in the bootloader");

/* One lap of the counter must be a useful amount of time.  If Fsys were ever
 * raised far enough that a lap no longer covered the longest gap a caller can
 * have (a 2.4 ms erase, with orders of magnitude to spare), the difference
 * arithmetic below would become ambiguous rather than merely tight. */
_Static_assert(BL_TIME_MAX_GAP_MS >= 100u,
               "one lap of SysTick is too short to bridge a flash blackout");

/* ------------------------------------------------------------------------- */
/* State — zero-initialised in .bss by start.S.  20 bytes.                    */
/* ------------------------------------------------------------------------- */

static uint32_t tb_ms;      /* whole milliseconds since the epoch             */
static uint32_t tb_us;      /* 0..999, microseconds not yet a millisecond     */
static uint32_t tb_cyc;     /* 0..31,  core cycles not yet a microsecond      */
static uint32_t tb_last;    /* the previous SYST_CVR sample, 24 bits          */
static uint32_t tb_running; /* 0 = clock stopped; makes bl_time_init() idempotent */

/* ------------------------------------------------------------------------- */
/* Interface                                                                  */
/* ------------------------------------------------------------------------- */

void bl_time_init(void)
{
    /* IDEMPOTENT.  bl_update_mode() and bl_led_init() both call this and
     * neither knows about the other; restarting the epoch under a caller that
     * is already holding a timestamp would make its elapsed-time arithmetic
     * jump backwards by however long the clock had been running. */
    if (tb_running != 0u) {
        return;
    }

    /* Disable before reconfiguring, so the counter cannot reload from a
     * half-written RVR.  Writing CVR clears both the counter and COUNTFLAG. */
    BL_REG32(BL_TIME_SYST_CSR) = 0u;
    BL_REG32(BL_TIME_SYST_RVR) = BL_TIME_SYST_MAX;
    BL_REG32(BL_TIME_SYST_CVR) = 0u;

    tb_ms  = 0u;
    tb_us  = 0u;
    tb_cyc = 0u;

    /* ENABLE | CLKSOURCE.  TICKINT deliberately absent — see the assertion
     * above and note 1 in timebase.h. */
    BL_REG32(BL_TIME_SYST_CSR) = BL_TIME_CSR_RUN;

    /* The epoch is this sample, not the store above: the handful of cycles
     * between them are simply not counted, which is the correct direction and
     * is why the sample is taken after the enable rather than assumed to be 0.
     * (After a write of 0 to CVR the counter reads 0 until it reloads; the
     * modular difference in bl_time_ms() is correct either way.) */
    tb_last    = BL_REG32(BL_TIME_SYST_CVR) & BL_TIME_SYST_MAX;
    tb_running = 1u;
}

uint32_t bl_time_ms(void)
{
    uint32_t now;
    uint32_t elapsed;
    uint32_t us;
    uint32_t ms;

    /* THE CLOCK-STOPPED TEST, WHICH IS NOT OPTIONAL.  It is tempting to argue
     * that a stopped SysTick reads CVR as 0 so the difference below would be 0
     * anyway — but ARMv6-M B3.3.3 makes SYST_CVR's reset value UNKNOWN and a
     * disabled SysTick retains whatever the counter last held.
     *
     * A caller is explicitly INVITED to call this before anything has started:
     * src/led.c's bl_led_set_pattern() timestamps with bl_time_ms() and
     * documents that it is safe before bl_led_init() / bl_time_init().  Before
     * the first init, CVR holds whatever reset or the ISP left there and tb_last
     * is 0 from .bss — up to 16.7 million cycles of difference, i.e. up to 524
     * spurious milliseconds on the very first call.
     *
     * This test makes the documented behaviour — a frozen clock, reading the
     * epoch's last value, 0 before any epoch — true by construction. */
    if (tb_running == 0u) {
        return tb_ms;
    }

    now = BL_REG32(BL_TIME_SYST_CVR) & BL_TIME_SYST_MAX;

    /* SysTick counts DOWN and reloads at 0, so time flows from tb_last towards
     * 0 and then wraps to RELOAD.  With RELOAD == BL_TIME_SYST_MAX the modulus
     * is exactly BL_TIME_SYST_MAX + 1, so masking the difference IS the modulo:
     * 0 -> 0x00FFFFFF is one count, not 16 million.  Correct across any number
     * of reloads up to one full lap — the caller's BL_TIME_MAX_GAP_MS
     * contract. */
    elapsed = (tb_last - now) & BL_TIME_SYST_MAX;
    tb_last = now;

    /* Cycles -> microseconds, carrying the sub-microsecond remainder so no
     * cycle is ever dropped.  Shift and mask, not a division: Cortex-M0 has no
     * divide instruction and -nostdlib means no __aeabi_uidiv to call. */
    elapsed += tb_cyc;
    tb_cyc   = elapsed & (BL_TIME_CYCLES_PER_US - 1u);
    us       = (elapsed >> BL_TIME_US_SHIFT) + tb_us;

    /* Microseconds -> milliseconds.  NON-BLOCKING and bounded: on a normal call
     * `us` has grown by tens, so this runs zero or one times.  Its worst case
     * is one full lap of the counter arriving in a single sample — 524
     * iterations of a subtract and a compare, a few microseconds — and that is
     * only reachable by a caller that broke the BL_TIME_MAX_GAP_MS contract, in
     * which case bounded catch-up is exactly the behaviour wanted. */
    ms = tb_ms;
    while (us >= BL_TIME_US_PER_MS) {
        us -= BL_TIME_US_PER_MS;
        ms++;
    }
    tb_ms = ms;         /* stored unconditionally: writing back through a local
                         * is two instructions cheaper than the flag GCC keeps
                         * when the counter is incremented in place */
    tb_us = us;

    return ms;
}

void bl_time_deinit(void)
{
    /* Disable FIRST, then clear.  CSR = 0 also clears COUNTFLAG's only
     * consumer; the CVR write clears COUNTFLAG itself.  All three registers
     * are cleared here, which is exactly what bl_jump_to_app() also does —
     * see "SYST_RVR at handoff" above.  Nothing in this function may be taken
     * as licence to remove boot.c's three stores: this one is gc-sectioned
     * out of the shipping image, so the handoff path must stand on its own. */
    BL_REG32(BL_TIME_SYST_CSR) = 0u;
    BL_REG32(BL_TIME_SYST_CVR) = 0u;
    BL_REG32(BL_TIME_SYST_RVR) = 0u;

    /* tb_ms is deliberately NOT reset: bl_time_ms() must never step backwards
     * within an epoch, and a caller holding a timestamp across the stop should
     * see the clock freeze, not rewind.  The next bl_time_init() zeroes it.
     *
     * tb_running = 0 is what makes bl_time_ms() freeze, and it is the ONLY
     * thing that does.  Zeroing tb_last below is tidiness; bl_time_init()
     * re-samples CVR into it before setting tb_running again. */
    tb_last    = 0u;
    tb_cyc     = 0u;
    tb_us      = 0u;
    tb_running = 0u;
}
