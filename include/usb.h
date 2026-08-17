/* usb.h — GBFlash CH579 bootloader: polled CH340-emulation USB device.
 *
 * Owner: comp:usb.  Implemented by src/usb.c and src/usb_desc.c.
 *
 * Design authority: docs/DESIGN.md §2 (why this layer is polled) and
 * docs/PROTOCOL.md (the transport the descriptors have to present).  The
 * register set, descriptor bytes and endpoint arming rules were read out of
 * the shipping application's own USB implementation.
 *
 * THE ONE THING THAT MUST NOT BE CHANGED WITHOUT READING THE ANALYSIS:
 * this layer is *polled*.  It never enables NVIC IRQ6 and it contains no
 * interrupt handler.  The bootloader's vector table forwards vector 22 (IRQ6 =
 * USB) to the APPLICATION's table at 0x4000, which is erased during an update;
 * an enabled IRQ6 would then fetch 0xFFFFFFFF, be rejected by the range guard
 * in bl_vector_forward, and spin forever with CodeFlash unlocked.  bl_usb_init()
 * therefore writes NVIC_ICER/ICPR bit 6 and never NVIC_ISER.
 *
 * Contract with the update-mode loop:
 *   - bl_usb_init() once, then bl_usb_poll() at least every few milliseconds.
 *   - No path may run longer than ~10 ms without calling bl_usb_poll(): a USB
 *     bus reset must be answered within the host's ~10 ms reset-recovery
 *     window.  The flag latches, so late service is degraded, not fatal; the
 *     host simply re-tries (RB_UC_INT_BUSY makes the SIE auto-NAK meanwhile).
 *   - A single 512-byte flash sector erase/program is well inside that budget.
 *
 * Contract WITH the time base (include/timebase.h), which runs the other way:
 *   - Every timeout in this layer is measured in real milliseconds from
 *     bl_time_ms().  The one remaining busy-wait is the USB PLL settle, which
 *     is a hardware settle and predates any software clock.
 *   - This layer never lets more than BL_USB_TX_MAX_GAP_MS (in bl_usb_tx) or
 *     BL_USB_INIT_MAX_GAP_MS (in bl_usb_init) pass between two bl_time_ms()
 *     calls, so it cannot on its own cost the accumulator a lap.  src/usb.c
 *     _Static_asserts both against BL_TIME_MAX_GAP_MS.
 */

#ifndef BL_USB_H
#define BL_USB_H

#include <stdint.h>

/* For bl_time_ms(), which is what BL_USB_TX_TIMEOUT_MS is measured on below.
 * timebase.h pulls in nothing but <stdint.h> — in particular it does NOT drag
 * bl_config.h in behind it, which is why it is safe to include from a header
 * that src/boot.c also sees. */
#include "timebase.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Compile-time options                                                       */
/* ------------------------------------------------------------------------- */

/* BRING-UP ONLY: echo every received byte straight back, so a hardware test is
 * a pyserial round-trip with no protocol involved.  The shipping image builds
 * with BL_USB_ECHO=0 and must: with echo on, the echo pump consumes the receive
 * buffer and bl_usb_rx() returns 0 to any other consumer. */
#ifndef BL_USB_ECHO
#define BL_USB_ECHO             1
#endif

/* Power the USB PLL (R8_HFCK_PWR_CTRL |= RB_CLK_PLL_PON) and wait for it in
 * bl_usb_init().  The shipping application does this in bsp_init before it
 * touches USB, and it is believed to be REQUIRED — Fsys runs directly off the
 * 32 MHz RC and uses the PLL for nothing, so the only consumer is the USB SIE
 * (inferred, not proven; cost of replicating it ~24 B + 5 ms).
 * start.S does not do it today.  Set to 0 only if start.S takes it over. */
#ifndef BL_USB_PLL_POWER_ON
#define BL_USB_PLL_POWER_ON     1
#endif

/* Milliseconds to wait after powering the PLL.  The application waits 3; the
 * WCH community example waits 5 "so the PLL fully opens".  A bootloader has no
 * schedule to keep.
 *
 * NOMINAL milliseconds: this is the ONE wait in this file that is still a
 * calibrated busy loop rather than a read of bl_time_ms(), because it is a
 * HARDWARE settle that must hold whether or not any software time base has
 * been started (see usb_delay_ms() in src/usb.c, and BL_USB_INIT_MAX_GAP_MS
 * below for what its real duration is and why that is safe). */
#ifndef BL_USB_PLL_SETTLE_MS
#define BL_USB_PLL_SETTLE_MS    5u
#endif

/* Staging buffer sizes.  The credit scheme never lets the receive side exceed
 * one packet in steady state, so these are slack, not depth.  RX holds a whole
 * protocol chunk (BL_PAGE_SIZE + framing) with room to spare; TX comfortably
 * holds the largest response the framer builds (BL_MAX_TX = 28). */
#ifndef BL_USB_RX_BUF_SIZE
#define BL_USB_RX_BUF_SIZE      512u
#endif
#ifndef BL_USB_TX_BUF_SIZE
#define BL_USB_TX_BUF_SIZE      256u
#endif

/* ------------------------------------------------------------------------- */
/* The transmit stall budget, and the time base it is measured on             */
/* ------------------------------------------------------------------------- */
/*
 * Upper bound on how long bl_usb_tx() will keep pumping the poll loop waiting
 * for staging-buffer room before giving up and returning short.  A host that
 * has stopped reading must not be able to hang the bootloader.
 *
 * REAL milliseconds, read from bl_time_ms() (include/timebase.h) — clocked, not
 * counted.  Counting it with usb_delay_ms() instead would put the whole stall
 * between two consecutive bl_time_ms() calls, within a hair of
 * BL_TIME_MAX_GAP_MS (524 ms): one lost lap of the 24-bit counter, and every
 * window spanning the stall firing up to half a second late.  As written, the
 * wait reads bl_time_ms() once per bl_usb_poll(), so the gap it contributes is
 * one poll rather than the whole budget.
 */
#ifndef BL_USB_TX_TIMEOUT_MS
#define BL_USB_TX_TIMEOUT_MS    250u
#endif

/* THE CERTIFIED FIGURE FOR THE TIMEBASE CONTRACT: an upper bound, in real
 * milliseconds, on how long bl_usb_tx() can run between two consecutive
 * bl_time_ms() calls — however long it stalls in total.
 *
 * Derivation.  bl_usb_poll() handles at most one hardware event and contains
 * no unbounded loop; its longest path (a 64-byte bulk OUT copied into staging,
 * then usb_pumps() copying a 32-byte IN out, each usb_copy byte ~10 cycles,
 * plus two bounded uep2_ctrl_rmw() retries) is under 1,600 core cycles = ~50 us
 * at 32 MHz, or ~100 us with every access taking a flash wait state.
 *
 * AT MOST TWO SUCH POLLS SEPARATE TWO CLOCK READS, not one.  The wait loop is
 * NOT literally flat: when a poll frees staging room it takes a `continue`
 * that skips the bl_time_ms() (the emitted branch at 0x18D4..0x18DA).  It can
 * only do that once per call, and the bound is structural rather than lucky —
 * a poll that frees room frees a whole 32-byte packet (BL_USB_EP2_PKT), every
 * caller's payload is at most BL_MAX_TX = 28 bytes, so the queue after the
 * continue takes the rest and the loop exits.  A poll that frees nothing
 * leaves the buffer full and falls through to the clock read.  So the gap is
 * ~100 us, or ~200 us fully pessimised; 2 ms is a 10x margin, and it is what
 * src/boot.c's static assertion is written against — NOT
 * BL_USB_TX_TIMEOUT_MS, which does not bound a gap at all. */
#ifndef BL_USB_TX_MAX_GAP_MS
#define BL_USB_TX_MAX_GAP_MS    2u
#endif

/* LIVENESS BACKSTOP, and the reason bl_usb_tx() cannot hang even with the
 * clock stopped.  bl_time_ms() FREEZES when the time base is not running
 * (before the first bl_time_init(), after bl_time_deinit() — see timebase.h),
 * and against a frozen clock a deadline is unreachable by construction.  So
 * the wait also gives up if this many bl_usb_poll() calls pass without the
 * clock advancing by even one millisecond.
 *
 * It cannot fire while the clock runs: the CHEAPEST bl_usb_poll() — nothing
 * latched in R8_USB_INT_FG, both pumps idle — is a few hundred core cycles, so
 * at 32 MHz a millisecond is at most a few hundred polls.  4096 is more than an
 * order of magnitude above that.  With the clock stopped it ends the wait after
 * ~40 ms of polling instead of never. */
#ifndef BL_USB_TX_DEAD_CLOCK_POLLS
#define BL_USB_TX_DEAD_CLOCK_POLLS  4096u
#endif

/* The millisecond source bl_usb_tx() measures BL_USB_TX_TIMEOUT_MS with.
 *
 * On the target this is bl_time_ms() and nothing else.  The hook exists for
 * host/usb_model.c, which compiles src/usb.c BYTE FOR BYTE AS SHIPPED (no sed
 * transform, no host-only #ifdef in the .c) against a model of the CH579 USB
 * registers only: it links neither src/timebase.c nor a SysTick, and its
 * register model deliberately faults on any address it does not model, so a
 * real bl_time_ms() there would be a link error followed by an unmodelled
 * access to 0xE000E018.
 *
 * The host substitute advances one millisecond per call, modelling the SHAPE of
 * the deadline — a wait that is bounded and returns short — not its duration.
 * That is conservative: the device polls far faster than 1 kHz, so it makes
 * MORE attempts inside the same budget than the host does, never fewer.
 *
 * The stub lives here rather than in timebase.h on purpose.  timebase.h must
 * keep declaring the REAL accumulator, so that a host test of tick
 * accumulation, the 24-bit wrap and the BL_TIME_MAX_GAP_MS boundary can compile
 * src/timebase.c itself without a fake definition shadowing it. */
#ifndef BL_USB_TIME_MS
#  ifdef BL_HOST
#    include <stdint.h>
static inline uint32_t bl_usb_host_time_ms(void)
{
    static uint32_t t;      /* per translation unit; only src/usb.c calls it */
    return ++t;
}
#    define BL_USB_TIME_MS()    bl_usb_host_time_ms()
#  else
#    define BL_USB_TIME_MS()    bl_time_ms()
#  endif
#endif

/* THE OTHER CERTIFIED FIGURE: an upper bound, in real milliseconds, on the one
 * busy-wait left in this layer — usb_delay_ms(BL_USB_PLL_SETTLE_MS) inside
 * bl_usb_init(), which cannot use bl_time_ms() because it is a hardware settle
 * that must hold whether or not a software clock has been started.
 *
 * It is bounded by construction (the iteration count is computed before the
 * loop and cannot depend on hardware) and its real duration is exactly
 * measurable from the emitted code: 14 cycles x 4000 iterations x 5 ms
 * = 280,000 cycles = 8.75 ms at 32 MHz, plus flash wait states.  16 ms is
 * therefore a comfortable bound, and it is 33x under BL_TIME_MAX_GAP_MS.
 *
 * It is also on the cold path: boot.c starts the time base (bl_time_init())
 * BEFORE calling bl_usb_init(), reads bl_time_ms() immediately after, and then
 * enters the loop, so this settle sits inside a single ~9 ms gap that is taken
 * once per entry into update mode. */
#ifndef BL_USB_INIT_MAX_GAP_MS
#define BL_USB_INIT_MAX_GAP_MS  16u
#endif

/* ------------------------------------------------------------------------- */
/* Wire constants — from the application image, do not change                 */
/* ------------------------------------------------------------------------- */

/* VID/PID have no referencing code: src/usb_desc.c is a verbatim byte-for-byte
 * transcription of the application's descriptors and must stay one, so the two
 * bytes pairs there are literals. These name what those bytes mean. */
#define BL_USB_VID              0x1A86u   /* WCH                               */
#define BL_USB_PID              0x7523u   /* CH340                             */
#define BL_USB_EP0_PKT          8u        /* bMaxPacketSize0                   */
#define BL_USB_EP2_PKT          32u       /* bulk wMaxPacketSize, both ways    */

/* ------------------------------------------------------------------------- */
/* Interface                                                                  */
/* ------------------------------------------------------------------------- */

/* Bring the device up and attach the D+ pull-up.  Powers the USB PLL, waits
 * for it, masks NVIC IRQ6, then replays the application's USB_DeviceInit
 * (0x4B24) write for write.  Safe to call once; not re-entrant. */
void bl_usb_init(void);

/* Service USB.  Handles at most one hardware event (one transfer, or one bus
 * reset, or one suspend) per call, exactly as the application's interrupt
 * handler does per entry, then runs the transmit and echo pumps.  Contains no
 * unbounded loop and never waits on hardware. */
void bl_usb_poll(void);

/* Take up to `max' received bytes.  Non-blocking; returns 0 when nothing has
 * arrived.  Replaces the application's blocking cdc_read (0xAC54), whose
 * literal translation would deadlock a polled design.  Also re-opens the bulk
 * OUT endpoint when draining has made room — the receive credit is a pure
 * function of receive-buffer space and is never granted from the transmit
 * path (the application's usb_tx_pump does grant it there, which is its one
 * genuine silent-packet-drop path). */
uint32_t bl_usb_rx(uint8_t *buf, uint32_t max);

/* Queue `n' bytes for transmission.  Returns the number actually queued, which
 * is less than n only if the call could not finish within BL_USB_TX_TIMEOUT_MS
 * of REAL time (bl_time_ms(), not a busy-loop count) because the host stopped
 * consuming.  Unlike the application's cdc_write (0xAD40) it never discards
 * silently.
 *
 * Pumps bl_usb_poll() itself while it waits, and reads bl_time_ms() once per
 * poll, so the gap it contributes to the timebase contract is one poll —
 * BL_USB_TX_MAX_GAP_MS — and not the whole timeout. */
uint32_t bl_usb_tx(const uint8_t *buf, uint32_t n);

/* A counter that increments every time this layer decides a NEW host session
 * has begun and throws the bulk staging away: on a USB bus reset, and on the
 * ch34x line-reconfiguration request (bmRequestType 0x40, bRequest 0xA1
 * CH341_REQ_SERIAL_INIT) that every known driver issues when a tty is opened.
 *
 * WHY IT EXISTS.  A tty close/reopen does not reset the bus, so without this
 * the receive buffer still holds the previous session's unread bytes and an
 * already-armed IN packet is delivered into the new one — a first-frame desync
 * for the framer.  usb.c drops that state on its own; this accessor is
 * how a caller LEARNS it happened, so it can call bl_proto_reset() rather than
 * trying to parse the tail of somebody else's frame.
 *
 * Usage: latch the value alongside bl_usb_init(), compare each time round the
 * update-mode loop, and resynchronise the framer when it differs.  Wrap-around
 * is not a concern (2^32 reopens), and a missed increment cannot happen — the
 * counter is only ever read, never cleared. */
uint32_t bl_usb_session(void);

/* Non-zero once the host has issued SET_CONFIGURATION with a non-zero value.
 * Bulk traffic is NOT gated on this, and must not be: the CH340 "port open"
 * flag the application tracks is write-only dead state, and its 0xA4
 * MODEM_CTRL comparison does not match what real ch341 drivers send. */
int bl_usb_configured(void);

/* ------------------------------------------------------------------------- */
/* Descriptor data — src/usb_desc.c, internal to this layer                   */
/* ------------------------------------------------------------------------- */
/* Transcribed byte-for-byte from the L15 application image (flash 0xB408,
 * 0xB41A, 0xB441).  They are the contract with the host's ch341 driver and
 * must not be normalised, tidied or "corrected". */

extern const uint8_t bl_usb_desc_device[18];
extern const uint8_t bl_usb_desc_config[39];
extern const uint8_t bl_usb_ch340_vendor_tbl[26];

/* The replay cursor saturates here (cmp #0x18 / bge at 0x482A); it does not
 * wrap, so the 13th pair is returned for every request after the 13th. */
#define BL_USB_VENDOR_TBL_LAST  24u

#ifdef __cplusplus
}
#endif

#endif /* BL_USB_H */
