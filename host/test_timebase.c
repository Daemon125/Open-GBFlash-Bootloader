/* test_timebase.c — native test suite for src/timebase.c.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * Every timeout in the update loop is a difference of bl_time_ms():
 * bl_proto_idle()'s 50 ms framer-recovery window, the post-finalize handover
 * delay, bl_usb_tx()'s 250 ms transmit deadline, the LED's blink phases.  Until
 * this suite existed the module's arithmetic had NO executable coverage at
 * all — host/check_headers.c was the only file in host/ that even mentioned the
 * timebase, and it only compared two constants.  The accumulator was verified
 * by reading emitted ARM instructions, which is a real check but a
 * write-only one: nothing re-ran it when the code changed, and it had already
 * been wrong once (bl_usb_tx's busy-wait put a ~470 ms gap inside a 524 ms
 * budget, and the _Static_assert that was supposed to catch that was written
 * against a number the code did not produce).
 *
 * src/timebase.c is compiled here UNMODIFIED over a model of SysTick whose
 * clock this file advances explicitly — see host/timebase_model.c.  No sed
 * transform, no host-only #ifdef in the shipping source.
 *
 * WHAT IS ASSERTED, AND AGAINST WHAT
 * ----------------------------------
 * The module carries both remainders (sub-microsecond cycles in tb_cyc,
 * sub-millisecond microseconds in tb_us), so it drops nothing, so its answer
 * is exactly floor(cycles_since_epoch / 32000) for any sampling pattern in
 * which no single gap reaches one lap of the counter.  That closed form is the
 * oracle — tbm_expect_ms() — and it is computed from a uint64 the MODEL keeps,
 * never from the module's own state.
 *
 * Groups:
 *   A  register discipline: what init/deinit actually write, TICKINT clear
 *   B  tick accumulation, and that not one cycle is dropped
 *   C  the 24-bit wrap, including the largest legal gap (one lap minus one)
 *   D  stalls: the 4.6 ms sector blackout, the ~23 ms finalize re-CRC, and
 *      irregular real-shaped sampling
 *   E  a stall past BL_TIME_MAX_GAP_MS — DETECTED here, and shown to be a
 *      silent UNDER-count in the module (see the note in group E)
 *   F  the clock-stopped guard: bl_time_ms() before bl_time_init(), after
 *      bl_time_deinit(), and the idempotence of a second bl_time_init()
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "timebase.h"
#include "timebase_model.h"

/* ------------------------------------------------------------------ */
/* Tiny test framework — same shape as test_flash.c                    */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;
static const char *g_group = "";

static int g_sites[512];
static int g_nsites;

static void note_site(int line)
{
    int i;
    for (i = 0; i < g_nsites; i++) {
        if (g_sites[i] == line) return;
    }
    if (g_nsites < (int)(sizeof g_sites / sizeof g_sites[0])) {
        g_sites[g_nsites++] = line;
    }
}

static void checkf(int line, int cond, const char *fmt, ...)
{
    va_list ap;
    note_site(line);
    if (cond) {
        g_pass++;
        return;
    }
    g_fail++;
    fprintf(stderr, "FAIL test_timebase.c:%d [%s] ", line, g_group);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define CHECK(cond, ...) checkf(__LINE__, (cond), __VA_ARGS__)

/* Loops here run tens of thousands of samples and every one is a real check,
 * so every one is counted — the headline "executions" number is honest rather
 * than one-per-loop.  STOP_ON_FAIL breaks out at the first failure so the
 * output is one diagnostic line and not 96,000 of them; g_fail is snapshotted
 * per loop (f0) rather than tested against zero, so a failure in an earlier
 * group does not make later loops exit immediately. */
#define STOP_ON_FAIL(f0)  do { if (g_fail != (f0)) break; } while (0)

/* The oracle, in one place.  Fails loudly with both numbers, because "off by
 * one millisecond" and "off by 524" are different bugs. */
static void check_oracle(int line, const char *what)
{
    uint32_t got  = bl_time_ms();
    uint64_t want = tbm_expect_ms();
    checkf(line, (uint64_t)got == want,
           "%s: bl_time_ms() = %u, oracle = %llu (cycles since epoch %llu)",
           what, (unsigned)got, (unsigned long long)want,
           (unsigned long long)tbm_cycles_since_epoch());
}

#define ORACLE(what)  check_oracle(__LINE__, (what))

/* Start a fresh epoch on a fresh model. */
static void fresh(void)
{
    tbm_reset();
    bl_time_init();
}

/* ------------------------------------------------------------------ */
/* A. Register discipline                                              */
/* ------------------------------------------------------------------ */

static void test_registers(void)
{
    g_group = "A registers";

    tbm_reset();

    /* Before anything: the model deliberately does NOT pretend the counter
     * reads 0 while disabled (ARMv6-M B3.3.3 leaves it UNKNOWN), so this is
     * the state the shipping code has to be correct in. */
    CHECK(tbm_enabled() == 0, "the model starts with SysTick disabled");
    CHECK(tbm_cvr_accesses() == 0u, "nothing has touched SYST_CVR yet");

    bl_time_init();

    CHECK(tbm_rvr() == BL_TIME_SYST_MAX,
          "bl_time_init writes SYST_RVR = 0x%08X (a full 24-bit lap), got 0x%08X",
          (unsigned)BL_TIME_SYST_MAX, (unsigned)tbm_rvr());
    CHECK(tbm_csr() == BL_TIME_CSR_RUN,
          "bl_time_init writes SYST_CSR = %u (ENABLE|CLKSOURCE), got %u",
          (unsigned)BL_TIME_CSR_RUN, (unsigned)tbm_csr());

    /* THE safety property, restated in an executable form.  tools/check_image.py
     * asserts it against the linked ARM image and src/timebase.c asserts it at
     * compile time; this is the third, behavioural, opinion.  With TICKINT set
     * SysTick would take exception 15, vectors.S would trampoline it into the
     * application's vector table — erased mid-update — and the core would spin
     * in .Lhang with CodeFlash unlocked. */
    CHECK((tbm_csr() & BL_TIME_CSR_TICKINT) == 0u,
          "TICKINT is CLEAR in the value bl_time_init writes to SYST_CSR "
          "(CSR = 0x%08X)", (unsigned)tbm_csr());
    CHECK((tbm_csr() & BL_TIME_CSR_CLKSOURCE) != 0u,
          "CLKSOURCE is set — the counter runs off the 32 MHz processor clock");

    CHECK(tbm_enabled() != 0, "the counter is running after bl_time_init");
    CHECK(bl_time_ms() == 0u, "the epoch starts at 0 ms");

    /* The counter really counts DOWN and reloads at the top. */
    tbm_advance_cycles(1);
    CHECK(tbm_cvr() == BL_TIME_SYST_MAX,
          "one cycle after CVR = 0 the counter has reloaded to 0x%08X, got 0x%08X",
          (unsigned)BL_TIME_SYST_MAX, (unsigned)tbm_cvr());
    tbm_advance_cycles(1);
    CHECK(tbm_cvr() == BL_TIME_SYST_MAX - 1u,
          "and then counts down");

    bl_time_deinit();
    CHECK(tbm_csr() == 0u, "bl_time_deinit writes SYST_CSR = 0");
    CHECK(tbm_cvr() == 0u, "bl_time_deinit writes SYST_CVR = 0");
    CHECK(tbm_rvr() == 0u, "bl_time_deinit writes SYST_RVR = 0");
    CHECK(tbm_enabled() == 0, "the counter is stopped after bl_time_deinit");
}

/* ------------------------------------------------------------------ */
/* B. Tick accumulation                                                */
/* ------------------------------------------------------------------ */

static void test_accumulation(void)
{
    uint32_t i;
    int f0;

    g_group = "B accumulation";

    /* Whole milliseconds, one at a time, for two seconds. */
    fresh();
    f0 = g_fail;
    for (i = 1u; i <= 2000u; i++) {
        tbm_advance_ms(1);
        CHECK(bl_time_ms() == i, "after %u x 1 ms, bl_time_ms() = %u",
              i, bl_time_ms());
        STOP_ON_FAIL(f0);
    }
    CHECK(bl_time_ms() == 2000u, "2000 one-millisecond ticks read as 2000 ms");
    CHECK(tbm_tb_us() == 0u && tbm_tb_cyc() == 0u,
          "and leave no microsecond or cycle remainder (us=%u cyc=%u)",
          tbm_tb_us(), tbm_tb_cyc());

    /* ONE CYCLE AT A TIME.  This is the test that says no cycle is dropped:
     * 96,000 single-cycle samples must produce exactly 3 ms, which they can
     * only do if tb_cyc carries the sub-microsecond remainder and tb_us
     * carries the sub-millisecond one.  A version that recomputed
     * (cycles >> 5) / 1000 from scratch each call would read 0 for ever. */
    fresh();
    f0 = g_fail;
    for (i = 1u; i <= 96000u; i++) {
        tbm_advance_cycles(1);
        CHECK(bl_time_ms() == i / 32000u,
              "single-cycle sampling: after %u cycles bl_time_ms() = %u, want %u",
              i, bl_time_ms(), i / 32000u);
        STOP_ON_FAIL(f0);
    }
    CHECK(bl_time_ms() == 3u, "96,000 single-cycle samples read as exactly 3 ms");
    CHECK(tbm_tb_cyc() == 0u && tbm_tb_us() == 0u,
          "with both remainders back at zero (us=%u cyc=%u)",
          tbm_tb_us(), tbm_tb_cyc());

    /* 31 cycles at a time: never a whole microsecond, so every sample lands
     * entirely in the cycle remainder and the millisecond only appears when
     * the accumulated remainders say so. */
    fresh();
    f0 = g_fail;
    for (i = 1u; i <= 40000u; i++) {
        tbm_advance_cycles(31);
        CHECK((uint64_t)bl_time_ms() == (uint64_t)i * 31u / 32000u,
              "31-cycle sampling broke at sample %u: bl_time_ms() = %u, "
              "want %llu", i, bl_time_ms(),
              (unsigned long long)((uint64_t)i * 31u / 32000u));
        STOP_ON_FAIL(f0);
    }
    CHECK((uint64_t)bl_time_ms() == 40000ull * 31u / 32000u,
          "40,000 x 31 cycles = %llu ms", 40000ull * 31u / 32000u);

    /* The remainder is genuinely carried, not rounded away: 31,999 cycles is
     * one cycle short of a millisecond and must read 0, then one more cycle
     * must read 1. */
    fresh();
    tbm_advance_cycles(31999);
    CHECK(bl_time_ms() == 0u, "31,999 cycles is not yet a millisecond");
    CHECK(tbm_tb_us() == 999u && tbm_tb_cyc() == 31u,
          "and is held as 999 us + 31 cycles (got %u us + %u cyc)",
          tbm_tb_us(), tbm_tb_cyc());
    tbm_advance_cycles(1);
    CHECK(bl_time_ms() == 1u, "the 32,000th cycle makes it 1 ms");
    CHECK(tbm_tb_us() == 0u && tbm_tb_cyc() == 0u, "with nothing left over");

    /* Monotonic: an extra call with no time passing must not move it. */
    CHECK(bl_time_ms() == 1u && bl_time_ms() == 1u && bl_time_ms() == 1u,
          "repeated calls with a stopped virtual clock return the same value");
}

/* ------------------------------------------------------------------ */
/* C. The 24-bit wrap                                                  */
/* ------------------------------------------------------------------ */

static void test_wrap(void)
{
    uint32_t i;
    uint64_t before;

    g_group = "C wrap";

    /* Two full seconds in 100 ms steps: 64,000,000 cycles, i.e. the counter
     * reloads 3 times inside the run.  Every sample is checked against the
     * oracle, so a mishandled reload shows up at the step that contains it. */
    fresh();
    for (i = 1u; i <= 20u; i++) {
        tbm_advance_ms(100);
        ORACLE("100 ms steps across three counter reloads");
    }
    CHECK(bl_time_ms() == 2000u, "20 x 100 ms = 2000 ms across the wrap");
    CHECK(tbm_cycles_since_epoch() > 3u * TBM_LAP_CYCLES,
          "the run really did cross three laps (%llu cycles, lap = %llu)",
          (unsigned long long)tbm_cycles_since_epoch(),
          (unsigned long long)TBM_LAP_CYCLES);

    /* THE LARGEST LEGAL GAP: one lap minus one cycle.  timebase.h's contract is
     * "call at least twice a second"; this is the exact edge of it, and it must
     * be counted in full rather than aliased.  16,777,215 cycles = 524,287.96
     * us, so 524 ms with 287 us and 31 cycles left over. */
    fresh();
    tbm_advance_cycles(TBM_LAP_CYCLES - 1u);
    CHECK(bl_time_ms() == 524u,
          "a gap of one lap minus one cycle reads as 524 ms, got %u",
          bl_time_ms());
    CHECK(bl_time_ms() == (uint32_t)BL_TIME_MAX_GAP_MS,
          "which is exactly BL_TIME_MAX_GAP_MS (%u)",
          (unsigned)BL_TIME_MAX_GAP_MS);
    CHECK(tbm_tb_us() == 287u && tbm_tb_cyc() == 31u,
          "with 287 us + 31 cycles carried (got %u us + %u cyc)",
          tbm_tb_us(), tbm_tb_cyc());
    CHECK(tbm_gap_violations() == 0u,
          "and the model agrees it is still inside the contract");

    /* Repeated maximum-length gaps: nothing accumulates an error. */
    fresh();
    for (i = 1u; i <= 8u; i++) {
        tbm_advance_cycles(TBM_LAP_CYCLES - 1u);
        ORACLE("back-to-back maximum-length gaps");
    }
    CHECK(tbm_gap_violations() == 0u,
          "eight maximum-length gaps are all legal");
    before = tbm_max_gap_cycles();
    CHECK(before == TBM_LAP_CYCLES - 1u,
          "the model measured the gap as %llu cycles",
          (unsigned long long)before);
}

/* ------------------------------------------------------------------ */
/* D. Stalls that really happen                                        */
/* ------------------------------------------------------------------ */

/* One 512-byte sector write pauses the core — not merely stalls it — for a
 * ~2.4 ms erase plus ~36 us per programmed word, about 4.6 ms in total, during
 * which nothing in the update loop runs (measured on this silicon).  That
 * is thousands of times longer than the 1 ms tick a COUNTFLAG-based design
 * could see, and recovering it exactly is the entire reason the reload is
 * 0x00FFFFFF rather than 31,999. */
#define ERASE_BLACKOUT_CYCLES   (4600u * 32u)      /* 4.6 ms   */
#define FINAL_CRC_CYCLES       (23000u * 32u)      /* ~23 ms   */

static void test_stalls(void)
{
    uint32_t i;
    uint32_t t0, t1;
    uint64_t state;
    int f0;

    g_group = "D stalls";

    /* A sector blackout, counted in full. */
    fresh();
    t0 = bl_time_ms();
    tbm_advance_cycles(ERASE_BLACKOUT_CYCLES);
    t1 = bl_time_ms();
    CHECK(t1 - t0 == 4u,
          "a 4.6 ms sector blackout advances the clock by 4 ms (got %u)",
          t1 - t0);
    ORACLE("after one sector blackout");

    /* Sixty of them — a whole stock fw.bin update's worth of flash writes,
     * with nothing else in between.  This is the pattern that would lose time
     * on any design that sampled a 1 ms counter. */
    fresh();
    for (i = 0u; i < 60u; i++) {
        tbm_advance_cycles(ERASE_BLACKOUT_CYCLES);
        ORACLE("60 consecutive sector blackouts");
    }
    CHECK(bl_time_ms() == 276u, "60 x 4.6 ms reads as 276 ms, got %u",
          bl_time_ms());

    /* The finalize re-CRC, which is the longest single gap the update loop
     * has left after bl_update_flash_read() was made to sample the clock per
     * page.  Still comfortably inside one lap. */
    fresh();
    tbm_advance_cycles(FINAL_CRC_CYCLES);
    CHECK(bl_time_ms() == 23u, "a 23 ms finalize re-CRC reads as 23 ms, got %u",
          bl_time_ms());
    CHECK(tbm_gap_violations() == 0u, "and stays inside the contract");

    /* IRREGULAR SAMPLING, WHICH IS WHAT THE REAL LOOP DOES.  A deterministic
     * LCG picks gaps spanning six orders of magnitude — a few hundred cycles
     * (one bl_usb_poll) up to just under a full lap — and the oracle is
     * checked after every one.  20,000 samples, ~1e11 cycles, i.e. about an
     * hour of virtual device time with the counter reloading thousands of
     * times. */
    fresh();
    f0 = g_fail;
    state = 0x2545F4914F6CDD1Dull;
    for (i = 0u; i < 20000u; i++) {
        uint64_t gap;
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        /* [1, TBM_LAP_CYCLES - 1] — always legal, never aliased. */
        gap = 1u + (state >> 33) % (TBM_LAP_CYCLES - 1u);
        tbm_advance_cycles(gap);
        CHECK((uint64_t)bl_time_ms() == tbm_expect_ms(),
              "irregular sampling broke at sample %u (gap %llu cycles): "
              "bl_time_ms() = %u, oracle = %llu",
              i, (unsigned long long)gap, bl_time_ms(),
              (unsigned long long)tbm_expect_ms());
        STOP_ON_FAIL(f0);
    }
    CHECK((uint64_t)bl_time_ms() == tbm_expect_ms(),
          "20,000 irregular samples track the oracle exactly (%llu ms)",
          tbm_expect_ms());
    CHECK(tbm_gap_violations() == 0u,
          "no sample exceeded the contract (max gap %llu of %llu cycles)",
          (unsigned long long)tbm_max_gap_cycles(),
          (unsigned long long)TBM_LAP_CYCLES);
    CHECK(tbm_cycles_since_epoch() > 100ull * TBM_LAP_CYCLES,
          "and the counter wrapped far more than once (%llu laps)",
          (unsigned long long)(tbm_cycles_since_epoch() / TBM_LAP_CYCLES));
}

/* ------------------------------------------------------------------ */
/* E. Past BL_TIME_MAX_GAP_MS                                          */
/* ------------------------------------------------------------------ */

/* WHAT "DETECTED" CAN AND CANNOT MEAN HERE — read this before changing it.
 *
 * A gap of exactly one lap is, from the counter alone, indistinguishable from
 * a gap of zero: 24 bits carry no lap number and the SysTick exception is
 * disabled by design (TICKINT would trampoline into the erased application
 * vector table).  bl_time_ms() therefore CANNOT detect an over-long gap, and
 * no test can give it that ability — the information is not present.  Saying
 * otherwise in a comment is how the last defect got written.
 *
 * What it does instead is fail in the safe direction, and that is a real
 * property worth pinning: the count is (gap mod 2^24), which is always <= gap,
 * so the accumulator UNDER-counts and every window built on it fires LATE,
 * never early.  Late is the "device appears dead" symptom (bl_proto_idle at
 * 574 ms instead of 50); early would be a truncated transfer or a handover
 * into a half-written image.
 *
 * The DETECTOR lives in the model, where the lap number does exist:
 * tbm_gap_violations() counts intervals between consecutive SYST_CVR accesses
 * that reach a full lap.  It is not a property of the device — it is the tool
 * that lets any host test assert the contract timebase.h states and that
 * nothing on the device enforces.  Group D uses it that way.
 */
static void test_overlong_gap(void)
{
    uint32_t t0, t1, i;
    int f0;

    g_group = "E over-long gap";

    /* Exactly one lap: the worst case, and the one that reads as no time at
     * all.  524 real milliseconds are lost. */
    fresh();
    t0 = bl_time_ms();
    tbm_advance_cycles(TBM_LAP_CYCLES);
    t1 = bl_time_ms();
    CHECK(t1 - t0 == 0u,
          "a gap of exactly one lap is counted as 0 ms (the aliasing is total), "
          "got %u", t1 - t0);
    CHECK(tbm_expect_ms() == 524u,
          "while %llu ms really passed", (unsigned long long)tbm_expect_ms());
    CHECK(tbm_gap_violations() == 1u,
          "THE MODEL DETECTS IT: %u interval(s) reached a full lap",
          tbm_gap_violations());
    CHECK(tbm_max_gap_cycles() == TBM_LAP_CYCLES,
          "and reports the offending gap as %llu cycles",
          (unsigned long long)tbm_max_gap_cycles());

    /* One lap plus 1 ms reads as 1 ms, not 525: the count is gap mod 2^24. */
    fresh();
    t0 = bl_time_ms();
    tbm_advance_cycles(TBM_LAP_CYCLES + 32000u);
    t1 = bl_time_ms();
    CHECK(t1 - t0 == 1u,
          "one lap + 1 ms is counted as 1 ms, not %llu",
          (unsigned long long)tbm_expect_ms());
    CHECK(tbm_gap_violations() == 1u, "detected by the model");

    /* Two laps plus 100 ms — the aliasing is modular, not saturating. */
    fresh();
    t0 = bl_time_ms();
    tbm_advance_cycles(2u * TBM_LAP_CYCLES + 100u * 32000u);
    t1 = bl_time_ms();
    CHECK(t1 - t0 == 100u,
          "two laps + 100 ms is counted as 100 ms, got %u", t1 - t0);
    CHECK(tbm_gap_violations() == 1u, "detected by the model");

    /* THE DIRECTION, SWEPT.  For a range of over-long gaps the module's answer
     * must never exceed the truth.  An OVER-count would be the dangerous
     * failure: a timeout firing early can truncate a transfer or hand over to
     * a half-written image. */
    f0 = g_fail;
    for (i = 0u; i < 64u; i++) {
        uint64_t gap = TBM_LAP_CYCLES + (uint64_t)i * 262144ull;
        uint64_t truth;
        fresh();
        tbm_gap_reset();
        t0 = bl_time_ms();
        tbm_advance_cycles(gap);
        t1 = bl_time_ms();
        truth = gap / TBM_CYC_PER_MS;
        CHECK((uint64_t)(t1 - t0) <= truth,
              "gap %llu cycles: counted %u ms, which is MORE than the true "
              "%llu ms — the error must always be an under-count",
              (unsigned long long)gap, t1 - t0, (unsigned long long)truth);
        CHECK((uint64_t)(t1 - t0) == (gap % TBM_LAP_CYCLES) / TBM_CYC_PER_MS,
              "gap %llu cycles: counted %u ms, want (gap mod lap) = %llu",
              (unsigned long long)gap, t1 - t0,
              (unsigned long long)((gap % TBM_LAP_CYCLES) / TBM_CYC_PER_MS));
        CHECK(tbm_gap_violations() == 1u,
              "gap %llu cycles: the model flagged %u violation(s), want 1",
              (unsigned long long)gap, tbm_gap_violations());
        STOP_ON_FAIL(f0);
    }

    /* Recovery: the module is not left corrupt by a violated contract.  After
     * the over-long gap it goes back to being exact. */
    fresh();
    tbm_advance_cycles(TBM_LAP_CYCLES + 5u * 32000u);
    t0 = bl_time_ms();
    tbm_gap_reset();
    f0 = g_fail;
    for (i = 0u; i < 100u; i++) {
        tbm_advance_ms(1);
        CHECK(bl_time_ms() == t0 + i + 1u,
              "the clock did not resume exactly after an over-long gap "
              "(sample %u: %u, want %u)", i, bl_time_ms(), t0 + i + 1u);
        STOP_ON_FAIL(f0);
    }
    CHECK(bl_time_ms() == t0 + 100u,
          "after an over-long gap the clock resumes counting exactly");
    CHECK(tbm_gap_violations() == 0u,
          "and the model's detector is clean again once the gaps are legal");
}

/* ------------------------------------------------------------------ */
/* F. The clock-stopped guard (defect 3)                               */
/* ------------------------------------------------------------------ */

/* bl_time_ms() used to have no tb_running test, justified with "a stopped
 * SysTick reads CVR as 0".  That is not architectural — ARMv6-M B3.3.3 leaves
 * SYST_CVR's reset value UNKNOWN and a disabled SysTick retains whatever it
 * last held — and src/led.c's bl_led_set_pattern() explicitly invites a call
 * before bl_time_init().  These tests are only meaningful because
 * host/timebase_model.c refuses to pretend a stopped counter reads 0. */
static void test_clock_stopped(void)
{
    uint32_t t;
    uint32_t hits;

    g_group = "F clock stopped";

    /* --- before any bl_time_init(), with a hostile counter value --- */
    tbm_reset();
    tbm_poke_counter(0x00ABCDEFu);      /* what reset or the ISP left there */
    CHECK(tbm_tb_running() == 0u, "the module has not been started");
    CHECK(bl_time_ms() == 0u,
          "bl_time_ms() before bl_time_init() returns 0, got %u", bl_time_ms());
    CHECK(tbm_cvr_accesses() == 0u,
          "and does not touch SYST_CVR at all (%u accesses) — the guard is an "
          "explicit tb_running test, not a consequence of the counter reading 0",
          tbm_cvr_accesses());

    /* The size of the hazard the guard closes, stated so the guard cannot be
     * removed on the argument that it never mattered.  Without it the first
     * call would have computed (tb_last - CVR) & 0xFFFFFF with tb_last = 0
     * from .bss: */
    {
        uint32_t bogus_cycles = (0u - 0x00ABCDEFu) & BL_TIME_SYST_MAX;
        CHECK(bogus_cycles / 32000u > 100u,
              "the unguarded form would have invented %u ms out of a CVR of "
              "0x00ABCDEF", bogus_cycles / 32000u);
    }

    /* Repeated calls stay at 0 and still touch nothing. */
    tbm_advance_ms(1000);
    CHECK(bl_time_ms() == 0u, "still 0 after a second of virtual time");
    CHECK(tbm_cvr_accesses() == 0u, "still no SYST_CVR access");

    /* --- after bl_time_deinit(): frozen, not rewound, not running --- */
    fresh();
    tbm_advance_ms(250);
    t = bl_time_ms();
    CHECK(t == 250u, "250 ms before the stop, got %u", t);

    bl_time_deinit();
    CHECK(tbm_tb_running() == 0u, "bl_time_deinit stops the clock");
    hits = tbm_cvr_accesses();

    tbm_advance_ms(5000);
    CHECK(bl_time_ms() == t,
          "bl_time_ms() is FROZEN at %u across five seconds of stopped clock, "
          "got %u", t, bl_time_ms());
    CHECK(tbm_cvr_accesses() == hits,
          "and touched SYST_CVR %u more times (want 0)",
          tbm_cvr_accesses() - hits);
    CHECK(bl_time_ms() >= t, "it never steps backwards");

    /* --- a fresh epoch after deinit --- */
    bl_time_init();
    CHECK(bl_time_ms() == 0u,
          "bl_time_init() after bl_time_deinit() starts a fresh epoch at 0");
    tbm_advance_ms(7);
    CHECK(bl_time_ms() == 7u, "and counts from there");

    /* --- IDEMPOTENCE.  bl_update_mode() and bl_led_init() both call
     * bl_time_init() and neither knows about the other; a restart under a
     * caller holding a timestamp would make its elapsed arithmetic jump
     * backwards. --- */
    fresh();
    tbm_advance_ms(300);
    t = bl_time_ms();
    bl_time_init();                       /* second call, clock still running */
    CHECK(bl_time_ms() == t,
          "a second bl_time_init() does NOT restart the epoch (%u -> %u)",
          t, bl_time_ms());
    CHECK(tbm_csr() == BL_TIME_CSR_RUN,
          "and leaves SYST_CSR alone");
    tbm_advance_ms(10);
    CHECK(bl_time_ms() == t + 10u, "the epoch continues unbroken");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_registers();
    test_accumulation();
    test_wrap();
    test_stalls();
    test_overlong_gap();
    test_clock_stopped();

    printf("\n== src/timebase.c over a modelled SysTick\n");
    printf("  lap = %llu cycles = %u ms (BL_TIME_MAX_GAP_MS)\n",
           (unsigned long long)TBM_LAP_CYCLES, (unsigned)BL_TIME_MAX_GAP_MS);
    printf("  the accumulator is checked against "
           "floor(cycles_since_epoch / 32000), computed by the model\n");
    printf("\n%d assertions, %d executions passed, %d failed\n",
           g_nsites, g_pass, g_fail);

    if (g_fail) {
        printf("test_timebase: FAIL\n");
        return 1;
    }
    return 0;
}
