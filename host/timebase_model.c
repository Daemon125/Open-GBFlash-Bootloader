/* timebase_model.c — src/timebase.c compiled natively over a model of SysTick.
 *
 * OWNED BY: comp:integrate.  Interface: host/timebase_model.h.
 *
 * ==========================================================================
 * THE FILE UNDER TEST IS THE SHIPPING FILE, BYTE FOR BYTE
 * ==========================================================================
 * include/bl_config.h defines
 *
 *     #define BL_REG32(a)  (*(volatile uint32_t *)(uintptr_t)(a))
 *
 * and src/timebase.c reaches SysTick through nothing else — BL_TIME_SYST_CSR,
 * _RVR and _CVR are its only three addresses and every access is a BL_REG32.
 * This file includes bl_config.h FIRST, #undefs BL_REG32, redefines it to call
 * into the model, and only then #includes ../src/timebase.c, whose own
 * `#include "bl_config.h"` is a no-op because the guard is already defined.
 *
 * So there is no sed transform (as flash.c needs) and no host-only #ifdef in
 * src/timebase.c (as nothing in this tree has).  The arithmetic, the guards,
 * the register order and every _Static_assert are the shipping ones.  This is
 * the same construction host/usb_model.c uses for src/usb.c, for the same
 * reason: the only thing worth testing is the code that ships.
 *
 * Consequence, stated plainly: tb_ms, tb_us, tb_cyc, tb_last and tb_running
 * are in scope below the #include.  They are exposed through tbm_tb_*() and
 * used ONLY for assertions about remainders that are invisible at millisecond
 * resolution.  Everything else in test_timebase.c is black-box.
 *
 * ==========================================================================
 * TIME DOES NOT PASS BY ITSELF
 * ==========================================================================
 * The model has no connection to wall time.  A virtual core-cycle counter
 * advances only when a test calls tbm_advance_cycles().  That is the whole
 * point: a 4.6 ms flash blackout, a 524.288 ms lap of the counter and a single
 * cycle are all equally easy to produce, exactly, and repeatably.
 *
 * ==========================================================================
 * THE COUNTER
 * ==========================================================================
 * SysTick counts DOWN, reloads to RVR when it reaches 0, and is 24 bits.  With
 * modulus M = RVR + 1 its value d cycles after it held `v` is
 *
 *     (v - d) mod M
 *
 * which is what tbm_counter_now() computes from (ref_val, ref_cycle).  While
 * DISABLED it holds still — which is the architecturally honest behaviour and
 * is deliberately NOT "reads as 0": ARMv6-M B3.3.3 leaves the reset value
 * UNKNOWN and says nothing about a disabled counter.  src/timebase.c used to
 * rely on the 0, and the whole reason bl_time_ms() now tests tb_running is
 * that it must not.  A model that returned 0 while stopped could not
 * distinguish the fixed code from the broken code, so this one does not.
 *
 * ==========================================================================
 * DETECTING A WRITE WHEN THE ACCESSOR ONLY HANDS OUT A POINTER
 * ==========================================================================
 * BL_REG32 must expand to an lvalue, so tbm_p32() returns a pointer and the
 * store lands in the register file with no callback.  Writes are therefore
 * detected the way host/usb_model.c detects them: at the head of the NEXT
 * access, by comparing the register file against the value the model last
 * published there.  Every model entry point in timebase_model.h calls
 * tbm_sync() first, so a test never observes un-reconciled state either.
 *
 * The three registers, and what a store to each means:
 *
 *   SYST_CSR  plain storage.  A change to bit 0 (ENABLE) starts or stops the
 *             counter; the counter's VALUE is preserved across both, which is
 *             what the architecture says and what makes the pre-init hazard
 *             reproducible.  COUNTFLAG (bit 16) is deliberately NOT modelled:
 *             src/timebase.c never reads CSR, and publishing a bit the module
 *             never wrote would make every store look like a change.  If a
 *             future timebase reads CSR, model COUNTFLAG before trusting it.
 *
 *   SYST_RVR  plain storage; it is the modulus, read live.  src/timebase.c
 *             only ever writes it while the counter is disabled, so the
 *             mid-lap case (architecturally: the new reload takes effect at
 *             the next wrap) does not arise and is not modelled.  The model
 *             refuses a reload of 0, which would make the modulus 1.
 *
 *   SYST_CVR  a write of ANY value clears the counter to 0 and clears
 *             COUNTFLAG.  src/timebase.c only ever writes 0.  There is one
 *             undetectable store — writing 0 while the model has just
 *             published 0 — and it is a genuine no-op: clearing a counter that
 *             already reads 0 leaves both the value and its future evolution
 *             identical, because the model re-bases (ref_val = 0, ref_cycle =
 *             now) to the same state the counter was already in.
 *
 * Several stores in a row are reconciled together at the next access — for
 * instance bl_time_init()'s CSR=0, RVR=MAX, CVR=0, CSR=RUN are all seen at the
 * CVR read that follows them.  Zero virtual cycles pass between them (only a
 * test advances the clock), so no ordering information is lost.
 *
 * ==========================================================================
 * ANY OTHER ADDRESS IS A FAULT
 * ==========================================================================
 * tbm_p32() aborts on an address that is not one of the three.  A future
 * timebase that reaches for another register does not get a silent zero.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FIRST — so the guard is set and src/timebase.c's own include is a no-op. */
#include "bl_config.h"

#undef BL_REG32

static volatile uint32_t *tbm_p32(uintptr_t a);

#define BL_REG32(a)  (*tbm_p32((uintptr_t)(a)))

#include "timebase_model.h"

/* ---------------------------------------------------------------------- */
/* State                                                                   */
/* ---------------------------------------------------------------------- */

enum { R_CSR = 0, R_RVR = 1, R_CVR = 2, R_N = 3 };

static volatile uint32_t tbm_file[R_N];   /* what src/timebase.c reads/writes */
static uint32_t          tbm_pub[R_N];    /* what the model last published    */

static uint64_t tbm_clock;          /* virtual core cycles since tbm_reset()  */
static uint64_t tbm_epoch;          /* tbm_clock at the last observed enable  */
static uint64_t tbm_ref_cycle;      /* tbm_clock when the counter held ...    */
static uint32_t tbm_ref_val;        /* ... this value                         */
static int      tbm_on;             /* the counter is running                 */

static uint32_t tbm_cvr_hits;       /* SYST_CVR accesses since tbm_reset()    */
static uint64_t tbm_last_cvr_cycle; /* tbm_clock at the previous CVR access   */
static int      tbm_have_last_cvr;
static uint64_t tbm_gap_max;
static uint32_t tbm_gap_bad;

static void tbm_sync(void);

/* ---------------------------------------------------------------------- */
/* The counter                                                             */
/* ---------------------------------------------------------------------- */

static uint32_t tbm_modulus(void)
{
    uint32_t rvr = tbm_file[R_RVR] & BL_TIME_SYST_MAX;
    if (rvr == 0u) {
        /* RVR = 0 disables the reload entirely on real hardware; nothing in
         * this bootloader ever leaves it there while the counter runs, and a
         * modulus of 1 would silently make every measurement 0. */
        fprintf(stderr, "timebase_model: SYST_RVR is 0 with the counter "
                        "running — the modulus would be 1\n");
        abort();
    }
    return rvr + 1u;
}

static uint32_t tbm_counter_now(void)
{
    uint64_t m, d;

    if (!tbm_on) {
        return tbm_ref_val;         /* a disabled counter HOLDS its value */
    }
    m = tbm_modulus();
    d = (tbm_clock - tbm_ref_cycle) % m;
    return (uint32_t)(((uint64_t)tbm_ref_val + m - d) % m);
}

/* Pin the counter to where it is now, so that a change of state (enable,
 * disable, reload) does not move it. */
static void tbm_rebase(void)
{
    tbm_ref_val   = tbm_counter_now();
    tbm_ref_cycle = tbm_clock;
}

/* ---------------------------------------------------------------------- */
/* Reconciliation — turn stores into their side effects                    */
/* ---------------------------------------------------------------------- */

static void tbm_sync(void)
{
    uint32_t v;

    /* CVR first: a write clears the counter, and if the same batch also
     * enables it the enable must see the cleared value.  (No virtual cycles
     * pass between stores, so this ordering is a statement of intent rather
     * than something observable.) */
    v = tbm_file[R_CVR];
    if (v != tbm_pub[R_CVR]) {
        tbm_ref_val   = 0u;         /* ANY written value clears the counter */
        tbm_ref_cycle = tbm_clock;
        tbm_pub[R_CVR] = v;
    }

    v = tbm_file[R_RVR];
    if (v != tbm_pub[R_RVR]) {
        tbm_rebase();               /* modulus changes; hold the value still */
        tbm_pub[R_RVR] = v;
    }

    v = tbm_file[R_CSR];
    if (v != tbm_pub[R_CSR]) {
        int want = (v & BL_TIME_CSR_ENABLE) ? 1 : 0;
        if (want != tbm_on) {
            tbm_rebase();           /* freeze / resume at the current value */
            tbm_on = want;
            if (want) {
                /* A fresh epoch for the oracle: bl_time_init() zeroes its
                 * accumulator here, so this is the instant bl_time_ms()
                 * counts from. */
                tbm_epoch          = tbm_clock;
                tbm_have_last_cvr  = 0;
            }
        }
        tbm_pub[R_CSR] = v;
    }

    /* Publish the live counter so a read of the register file gets it, and so
     * the next sync can tell a store from it. */
    tbm_file[R_CVR] = tbm_counter_now();
    tbm_pub[R_CVR]  = tbm_file[R_CVR];
}

/* ---------------------------------------------------------------------- */
/* The accessor src/timebase.c is compiled against                         */
/* ---------------------------------------------------------------------- */

static volatile uint32_t *tbm_p32(uintptr_t a)
{
    tbm_sync();

    switch ((uint32_t)a) {
    case BL_TIME_SYST_CSR:
        return &tbm_file[R_CSR];

    case BL_TIME_SYST_RVR:
        return &tbm_file[R_RVR];

    case BL_TIME_SYST_CVR:
        tbm_cvr_hits++;
        if (tbm_on) {
            if (tbm_have_last_cvr) {
                uint64_t gap = tbm_clock - tbm_last_cvr_cycle;
                if (gap > tbm_gap_max) {
                    tbm_gap_max = gap;
                }
                if (gap >= TBM_LAP_CYCLES) {
                    tbm_gap_bad++;
                }
            }
            tbm_last_cvr_cycle = tbm_clock;
            tbm_have_last_cvr  = 1;
        }
        return &tbm_file[R_CVR];

    default:
        fprintf(stderr,
            "timebase_model: src/timebase.c touched 0x%08X, which is not\n"
            "  SYST_CSR/RVR/CVR.  The module reached for a register this model\n"
            "  does not have; model it before trusting any result.\n",
            (unsigned)a);
        abort();
    }
}

/* src/timebase.c, unmodified, with BL_REG32 pointing at the model. */
#include "../src/timebase.c"

/* ---------------------------------------------------------------------- */
/* Harness interface                                                       */
/* ---------------------------------------------------------------------- */

void tbm_reset(void)
{
    memset((void *)tbm_file, 0, sizeof tbm_file);
    memset(tbm_pub, 0, sizeof tbm_pub);

    /* A cold part has an UNKNOWN counter and an unprogrammed reload.  Give the
     * reload the full range so the modulus is defined before anyone writes it;
     * tbm_poke_counter() is how a test asks for a hostile initial CVR. */
    tbm_file[R_RVR] = BL_TIME_SYST_MAX;
    tbm_pub[R_RVR]  = BL_TIME_SYST_MAX;

    tbm_clock          = 0;
    tbm_epoch          = 0;
    tbm_ref_cycle      = 0;
    tbm_ref_val        = 0;
    tbm_on             = 0;
    tbm_cvr_hits       = 0;
    tbm_last_cvr_cycle = 0;
    tbm_have_last_cvr  = 0;
    tbm_gap_max        = 0;
    tbm_gap_bad        = 0;

    /* .bss on the device; one process here.  Without this, every test after
     * the first would start from the previous one's accumulator. */
    tb_ms      = 0u;
    tb_us      = 0u;
    tb_cyc     = 0u;
    tb_last    = 0u;
    tb_running = 0u;
}

void tbm_advance_cycles(uint64_t n)
{
    tbm_sync();
    tbm_clock += n;
    tbm_sync();
}

void tbm_advance_us(uint64_t n)
{
    tbm_advance_cycles(n * (uint64_t)BL_TIME_CYCLES_PER_US);
}

void tbm_advance_ms(uint64_t n)
{
    tbm_advance_cycles(n * TBM_CYC_PER_MS);
}

uint64_t tbm_cycles(void)              { tbm_sync(); return tbm_clock; }
uint64_t tbm_cycles_since_epoch(void)  { tbm_sync(); return tbm_clock - tbm_epoch; }
uint64_t tbm_expect_ms(void)           { return tbm_cycles_since_epoch() / TBM_CYC_PER_MS; }

uint32_t tbm_csr(void)     { tbm_sync(); return tbm_pub[R_CSR]; }
uint32_t tbm_rvr(void)     { tbm_sync(); return tbm_pub[R_RVR]; }
uint32_t tbm_cvr(void)     { tbm_sync(); return tbm_counter_now(); }
int      tbm_enabled(void) { tbm_sync(); return tbm_on; }

void tbm_poke_counter(uint32_t v)
{
    tbm_sync();
    tbm_ref_val    = v & BL_TIME_SYST_MAX;
    tbm_ref_cycle  = tbm_clock;
    tbm_file[R_CVR] = tbm_ref_val;
    tbm_pub[R_CVR]  = tbm_ref_val;
}

uint32_t tbm_cvr_accesses(void) { return tbm_cvr_hits; }

uint64_t tbm_max_gap_cycles(void) { return tbm_gap_max; }
uint32_t tbm_gap_violations(void) { return tbm_gap_bad; }

void tbm_gap_reset(void)
{
    tbm_gap_max       = 0;
    tbm_gap_bad       = 0;
    tbm_have_last_cvr = 0;
}

uint32_t tbm_tb_ms(void)      { return tb_ms; }
uint32_t tbm_tb_us(void)      { return tb_us; }
uint32_t tbm_tb_cyc(void)     { return tb_cyc; }
uint32_t tbm_tb_last(void)    { return tb_last; }
uint32_t tbm_tb_running(void) { return tb_running; }
