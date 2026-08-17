/* timebase_model.h — the harness side of the native SysTick model.
 *
 * OWNED BY: comp:integrate.  Implementation and the full fidelity argument:
 * host/timebase_model.c — read its header before writing a test against this.
 *
 * WHAT IT IS FOR
 * --------------
 * src/timebase.c is the module the whole update loop's sense of time rests on:
 * bl_proto_idle()'s 50 ms window, the handover delay, bl_usb_tx()'s 250 ms
 * deadline and the LED's blink phases are all differences of bl_time_ms().
 * Until this file existed its arithmetic — a 24-bit down-counter sampled at
 * arbitrary intervals, with cycle and microsecond remainders carried by hand
 * because Cortex-M0 has no divide — was verified by reading emitted ARM code
 * and by nothing else.  host/check_headers.c mentioned the timebase only to
 * compare two constants.
 *
 * So: src/timebase.c is compiled natively, unmodified, against a model of
 * SysTick whose clock the test advances explicitly.  Nothing here guesses at
 * timing; a test says "3,200,000 cycles pass" and that is exactly what passes.
 *
 * THE INDEPENDENT ORACLE
 * ----------------------
 * bl_time_ms() converts cycles -> microseconds -> milliseconds carrying BOTH
 * remainders (tb_cyc, tb_us), so no cycle is ever dropped.  That makes its
 * answer exactly
 *
 *     floor(total_cycles_since_epoch / 32000)
 *
 * for any sequence of samples in which no single gap reaches one full lap of
 * the counter.  The oracle is computed from a uint64 the test keeps itself, so
 * it is not derived from the module's own state — see tbm_cycles().
 *
 * THE ONE THING THE MODEL KNOWS AND THE MODULE CANNOT
 * ---------------------------------------------------
 * A gap of one full lap (2^24 cycles = BL_TIME_MAX_GAP_MS = 524 ms) is
 * indistinguishable, from the counter alone, from a gap of zero: the module
 * counts (gap mod 2^24) and therefore UNDER-counts.  bl_time_ms() cannot
 * detect that and no amount of testing will give it the ability — a bare
 * free-running counter with its interrupt disabled carries no lap number.
 * The MODEL can, because it holds the virtual cycle count: it records the
 * interval between consecutive CVR accesses and counts the ones that reach a
 * lap.  tbm_gap_violations() is therefore a real detector, available to every
 * host test that links this file, for the contract timebase.h states and that
 * nothing on the device enforces.
 */

#ifndef TIMEBASE_MODEL_H
#define TIMEBASE_MODEL_H

#include <stdint.h>

#include "timebase.h"

/* One lap of the free-running counter, in core cycles.  16,777,216 at
 * RVR = 0x00FFFFFF — 524.288 ms at 32 MHz, i.e. BL_TIME_MAX_GAP_MS. */
#define TBM_LAP_CYCLES   ((uint64_t)BL_TIME_SYST_MAX + 1u)
#define TBM_CYC_PER_MS   ((uint64_t)BL_TIME_CYCLES_PER_US * BL_TIME_US_PER_MS)

/* ---------------------------------------------------------------------- */
/* The virtual clock                                                       */
/* ---------------------------------------------------------------------- */

/* Reset everything: the register file, the counter, the virtual clock, the
 * gap detector, AND src/timebase.c's own statics (tb_ms/tb_us/tb_cyc/tb_last/
 * tb_running), which .bss would have zeroed on the device but which persist
 * between tests in one process.  Call this at the top of every test. */
void tbm_reset(void);

/* Advance the virtual core clock.  Nothing else advances it: the model has no
 * notion of wall time, so a test that does not call these has a stopped clock
 * and bl_time_ms() will keep answering the same number for ever. */
void tbm_advance_cycles(uint64_t n);
void tbm_advance_us(uint64_t n);        /* n * BL_TIME_CYCLES_PER_US        */
void tbm_advance_ms(uint64_t n);        /* n * 32,000                       */

/* Total cycles advanced since tbm_reset().  The oracle's input. */
uint64_t tbm_cycles(void);

/* Cycles advanced since the last bl_time_init() the model observed, i.e. since
 * the epoch bl_time_ms() is counting from.  floor(this / 32000) is what
 * bl_time_ms() must return. */
uint64_t tbm_cycles_since_epoch(void);

/* The oracle itself, spelled once so no test can get it subtly wrong. */
uint64_t tbm_expect_ms(void);

/* ---------------------------------------------------------------------- */
/* The register file, as the model currently publishes it                  */
/* ---------------------------------------------------------------------- */

uint32_t tbm_csr(void);         /* last value written to SYST_CSR           */
uint32_t tbm_rvr(void);         /* last value written to SYST_RVR           */
uint32_t tbm_cvr(void);         /* the counter, live                        */
int      tbm_enabled(void);     /* CSR & BL_TIME_CSR_ENABLE                 */

/* Force the counter to a value without going through a register write, to set
 * up the state a cold reset or the ISP can leave behind: ARMv6-M B3.3.3 makes
 * SYST_CVR's reset value UNKNOWN.  This is what makes the "bl_time_ms() before
 * bl_time_init()" hazard reproducible — with a zero counter it is invisible. */
void tbm_poke_counter(uint32_t v);

/* Number of times src/timebase.c has touched SYST_CVR since tbm_reset().  The
 * clock-stopped path must not touch it at all, and that is checkable only from
 * here: the return value of bl_time_ms() looks the same either way. */
uint32_t tbm_cvr_accesses(void);

/* ---------------------------------------------------------------------- */
/* The gap detector — see the header comment                               */
/* ---------------------------------------------------------------------- */

/* Longest interval, in cycles, between two consecutive SYST_CVR accesses while
 * the counter was running.  timebase.h's contract is that this stays below
 * TBM_LAP_CYCLES. */
uint64_t tbm_max_gap_cycles(void);

/* How many intervals reached or exceeded one full lap.  Every one of them is a
 * silent under-count in bl_time_ms(); zero is the only acceptable value for a
 * test that is not deliberately provoking the condition. */
uint32_t tbm_gap_violations(void);

/* Forget the gap history without disturbing the clock or the module — for a
 * test that provokes a violation on purpose and then continues. */
void tbm_gap_reset(void);

/* ---------------------------------------------------------------------- */
/* src/timebase.c's private state, for white-box assertions                */
/* ---------------------------------------------------------------------- */

/* These exist because "the accumulator is correct" and "the accumulator's
 * REMAINDERS are correct" are different claims, and only the second one
 * catches a dropped cycle that happens to be invisible at millisecond
 * resolution.  Use the oracle for behaviour; use these to say why. */
uint32_t tbm_tb_ms(void);
uint32_t tbm_tb_us(void);
uint32_t tbm_tb_cyc(void);
uint32_t tbm_tb_last(void);
uint32_t tbm_tb_running(void);

#endif /* TIMEBASE_MODEL_H */
