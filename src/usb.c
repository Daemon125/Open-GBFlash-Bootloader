/* usb.c — GBFlash CH579 bootloader: polled CH340-emulation USB device.
 *
 * Owner: comp:usb.  Stage 3 scope: enumerate as 1A86:7523 and move bulk bytes.
 * No protocol handling lives here; that is proto.c, stage 4.
 *
 * Design authority, read before changing anything: docs/DESIGN.md §2 (why this
 * layer is polled) and docs/PROTOCOL.md (the transport it has to present).
 * Everything below — the register set, the PLL sequence, the init order, the
 * bus-reset handling, the canned vendor replay, which requests STALL, and the
 * endpoint arming and flow-control rules — was read out of the shipping
 * application's own USB implementation.
 *
 * ==========================================================================
 * WHY THIS IS POLLED AND NOT INTERRUPT-DRIVEN
 * ==========================================================================
 * The bootloader's vector table is static in flash sector 0 and forwards every
 * exception to the APPLICATION's table at 0x4000 through the IPSR trampoline.
 * During an update that table is erased, so a forwarded vector 22 (IRQ6 = USB)
 * would load 0xFFFFFFFF, be rejected by the range guard in bl_vector_forward,
 * and spin — stalling the update mid-flash with CodeFlash unlocked.  Cortex-M0
 * has no VTOR, so the vector table cannot be relocated to RAM and design (b)
 * from the brief is not merely hard, it is unavailable on this core.
 *
 * Polling is sound, not a compromise:
 *   - Every event the application services is a latching bit in ONE byte,
 *     R8_USB_INT_FG.  Nothing here is edge-only or self-clearing.
 *   - R8_USB_CTRL carries RB_UC_INT_BUSY, so the SIE auto-NAKs every incoming
 *     transaction while RB_UIF_TRANSFER is pending.  Late service costs
 *     throughput, never data; the host simply re-tries.
 *   - Independently of that, the OUT endpoint is driven to NAK after every
 *     accepted packet in software, so the receive side cannot be overrun even
 *     if the RB_UC_INT_BUSY reasoning were wrong.
 *   - The "2,000,000 baud" the host programs is a ch341 UART divisor for a
 *     UART that does not physically exist.  The wire is USB FS with 32-byte
 *     bulk packets and there is no throughput deadline.
 *   - The only real deadline is bus-reset recovery, ~10 ms, and the flag
 *     latches.
 *
 * R8_USB_INT_EN is left at 0x07, byte-identical to the application, and IRQ6
 * is masked at the NVIC instead.  That keeps the SIE in the exact configuration
 * proven to work on this silicon and sidesteps the one thing the analysis could
 * not settle offline: whether RB_UIF_* are set when RB_UIE_* are clear.
 * NVIC_ISER (0xE000E100) IS NEVER WRITTEN BY THIS FILE.  tools/check_image.py
 * should assert that no store to it exists anywhere in the image.
 *
 * ==========================================================================
 * DELIBERATE DIVERGENCES FROM THE APPLICATION (all justified in the analysis)
 * ==========================================================================
 *  1. The CH340 replay cursor is reset on bus reset.  The application never
 *     resets it, so after one enumeration every 0xC0 read returns FF EC for the
 *     rest of the power cycle.  Cannot change first-enumeration behaviour (the
 *     cursor is already 0 there); makes re-enumeration work.  [descriptors §6.2]
 *  2. Nine further state items are reset on bus reset that the application
 *     leaves stale.  [core §8]
 *  3. The 0x40 vendor-OUT path is "accept anything, do nothing, ZLP".  The
 *     application's port_open / serial_inited bytes are written in four places
 *     and read nowhere, so this is indistinguishable on the wire.  [desc §7.3]
 *  4. Receive credit is a pure function of receive-buffer space.  The
 *     application also grants it from the transmit-drain path, blind to receive
 *     state, which is its one genuine silent-packet-drop path.  [datapath §4.4]
 *  5. Receive is non-blocking; transmit blocks only with a bounded timeout and
 *     reports what it queued instead of discarding silently.  [datapath §6]
 *  6. EP0 gets a full 64-byte DMA buffer instead of the application's 8, which
 *     overlaps EP1's buffer and the entire USB state block.  [core §4]
 *  7. String descriptors are dropped (dead in the application).
 *  8. EP2 OUT is NAKed after EVERY completed OUT token, including a mis-toggled
 *     one; the application branches past that store at 0x4A6C.  [core §5.8a,
 *     and the comment on the TOK_OUT_EP2 case below]
 *  9. Every read-modify-write of R8_UEP2_CTRL goes through uep2_ctrl_rmw(),
 *     which detects a transaction completing across the pair and repairs the
 *     hardware-managed data toggle.  The application has this race open at all
 *     six of its own sites.  [§3b]
 * 10. The bulk staging is flushed, and any armed IN packet withdrawn, when the
 *     host reconfigures the line (0x40/0xA1).  The application flushes nothing,
 *     ever, outside a bus reset, so a tty reopen inherits the previous
 *     session's bytes.  [§6b]
 */

#include <stdint.h>

#include "bl_config.h"
#include "usb.h"          /* which includes timebase.h, for bl_time_ms()      */

/* ------------------------------------------------------------------------- */
/* 0. The timebase contract, machine-checked                                  */
/* ------------------------------------------------------------------------- */
/*
 * timebase.h's one obligation on its callers is that no more than
 * BL_TIME_MAX_GAP_MS (524 ms, one lap of the 24-bit counter) may pass between
 * two bl_time_ms() calls; past that a lap is lost and the accumulator silently
 * under-counts.  This layer has exactly two places that can run for a
 * measurable time without reading the clock, and both are certified in usb.h
 * against what the emitted code actually does:
 *
 *   BL_USB_TX_MAX_GAP_MS    2 ms   at most TWO bl_usb_poll() calls inside
 *                                  bl_usb_tx()'s wait (usb.h derives the two)
 *   BL_USB_INIT_MAX_GAP_MS 16 ms   usb_delay_ms(BL_USB_PLL_SETTLE_MS), the one
 *                                  busy-wait left, in bl_usb_init()
 *
 * They are never concurrent — the settle is in init, the wait is in transmit —
 * so the sum below is deliberately pessimistic.  Both figures are REAL
 * durations of the emitted code, not nominal ones: a busy loop calibrated 1.75x
 * long turns a "250 ms" budget into ~470 ms of a 524 ms lap.
 */
__extension__ _Static_assert((uint32_t)BL_USB_TX_MAX_GAP_MS
                                 + (uint32_t)BL_USB_INIT_MAX_GAP_MS
                             < (uint32_t)BL_TIME_MAX_GAP_MS,
                             "usb.c's longest run between two bl_time_ms() "
                             "calls must fit inside one lap of the timebase");

/* The dead-clock backstop must not be able to fire before the real deadline on
 * a running clock.  The cheapest bl_usb_poll() is a few hundred core cycles, so
 * a millisecond is at most a few hundred polls; requiring an order of magnitude
 * over that keeps the backstop strictly a can't-happen path. */
__extension__ _Static_assert((uint32_t)BL_USB_TX_DEAD_CLOCK_POLLS >= 1024u,
                             "the dead-clock backstop could fire on a healthy "
                             "device before one millisecond has elapsed");

/* ------------------------------------------------------------------------- */
/* 1. Registers.  Base 0x40008000; offsets from the application's own USB.    */
/* ------------------------------------------------------------------------- */

#define USB_BASE            0x40008000u

#define R8_USB_CTRL         BL_REG8(USB_BASE + 0x00u)
#define R8_UDEV_CTRL        BL_REG8(USB_BASE + 0x01u)
#define R8_USB_INT_EN       BL_REG8(USB_BASE + 0x02u)
#define R8_USB_DEV_AD       BL_REG8(USB_BASE + 0x03u)
#define R8_USB_MIS_ST       BL_REG8(USB_BASE + 0x05u)   /* read-only          */
#define R8_USB_INT_FG       BL_REG8(USB_BASE + 0x06u)
#define R8_USB_INT_ST       BL_REG8(USB_BASE + 0x07u)   /* read-only          */
#define R8_USB_RX_LEN       BL_REG8(USB_BASE + 0x08u)   /* read-only          */
#define R8_UEP4_1_MOD       BL_REG8(USB_BASE + 0x0Cu)
#define R8_UEP2_3_MOD       BL_REG8(USB_BASE + 0x0Du)
#define R16_UEP0_DMA        BL_REG16(USB_BASE + 0x10u)
#define R16_UEP1_DMA        BL_REG16(USB_BASE + 0x14u)
#define R16_UEP2_DMA        BL_REG16(USB_BASE + 0x18u)
#define R8_UEP0_T_LEN       BL_REG8(USB_BASE + 0x20u)
#define R8_UEP0_CTRL        BL_REG8(USB_BASE + 0x22u)
#define R8_UEP1_CTRL        BL_REG8(USB_BASE + 0x26u)
#define R8_UEP2_T_LEN       BL_REG8(USB_BASE + 0x28u)
#define R8_UEP2_CTRL        BL_REG8(USB_BASE + 0x2Au)

/* USB analog pad enable.  Plain RW — no safe-access window, and none is used
 * by the application either. */
#define R16_PIN_ANALOG_IE   BL_REG16(0x4000101Au)
#define RB_PIN_USB_IE       0x0080u

/* NVIC.  ICER and ICPR only.  NVIC_ISER (0xE000E100) must never appear. */
#define NVIC_ICER           BL_REG32(0xE000E180u)
#define NVIC_ICPR           BL_REG32(0xE000E280u)
#define USB_IRQ_BIT         (1u << 6)          /* IRQ6 = USB, confirmed       */

/* Bit constants, verbatim from WCH CH579SFR.h. */
#define RB_UC_DEV_PU_EN     0x20u
#define RB_UC_INT_BUSY      0x08u
#define RB_UC_DMA_EN        0x01u

#define RB_UD_PD_DIS        0x80u
#define RB_UD_PORT_EN       0x01u

#define RB_UIE_SUSPEND      0x04u
#define RB_UIE_TRANSFER     0x02u
#define RB_UIE_BUS_RST      0x01u

#define RB_UIF_FIFO_OV      0x10u
#define RB_UIF_SUSPEND      0x04u
#define RB_UIF_TRANSFER     0x02u
#define RB_UIF_BUS_RST      0x01u

#define RB_UIS_TOG_OK       0x40u
#define MASK_UIS_TOKEN_EP   0x3Fu              /* MASK_UIS_TOKEN|MASK_UIS_ENDP */

#define RB_UEP1_TX_EN       0x40u
#define RB_UEP2_RX_EN       0x08u
#define RB_UEP2_TX_EN       0x04u

#define RB_UEP_R_TOG        0x80u   /* SIE-managed on EP2 (AUTO_TOG); see §3b  */
#define RB_UEP_T_TOG        0x40u   /* likewise                               */
#define RB_UEP_AUTO_TOG     0x10u
#define MASK_UEP_R_RES      0x0Cu
#define   UEP_R_RES_ACK     0x00u
#define   UEP_R_RES_NAK     0x08u
#define MASK_UEP_T_RES      0x03u
#define   UEP_T_RES_ACK     0x00u
#define   UEP_T_RES_NAK     0x02u

/* The three composite values the application writes to R8_UEPn_CTRL. */
#define UEP0_CTRL_IDLE      0x02u   /* R_RES=ACK, T_RES=NAK, toggles cleared  */
#define UEP0_CTRL_SETUP_ARM 0xC2u   /* R_TOG|T_TOG, ACK/NAK                   */
#define UEP0_CTRL_DATA      0xC0u   /* R_TOG|T_TOG, ACK/ACK — first IN = DATA1 */
#define UEP0_CTRL_STALL     0xCFu   /* R_TOG|T_TOG, STALL both directions     */
#define UEPn_CTRL_BULK      0x12u   /* AUTO_TOG, R_RES=ACK, T_RES=NAK         */

/* Transfer dispatch values, R8_USB_INT_ST & 0x3F. */
#define TOK_OUT_EP0         0x00u
#define TOK_OUT_EP2         0x02u
#define TOK_IN_EP0          0x20u
#define TOK_IN_EP2          0x22u
#define TOK_SETUP_EP0       0x30u
/* 0x21 (IN, EP1) is deliberately absent: the application has no case for it
 * either, EP1 sits at T_RES=NAK forever, and the ch341 driver tolerates it. */

/* Standard requests the application implements.  Everything else STALLs, and
 * that includes CLEAR_FEATURE(ENDPOINT_HALT) — reproduced rather than
 * "improved", because the shipping behaviour is what the user's macOS host is
 * known to accept. */
#define REQ_SET_ADDRESS     5u
#define REQ_GET_DESCRIPTOR  6u
#define REQ_GET_CONFIG      8u
#define REQ_SET_CONFIG      9u

#define DESC_TYPE_DEVICE    1u
#define DESC_TYPE_CONFIG    2u

/* bmRequestType values matched as WHOLE BYTES, exactly as the application does
 * (cmp r5,#0xc0 at 0x4804).  0xC1/0xC2/0x41/0x42 deliberately fall through to
 * the standard decoder and STALL. */
#define BMREQ_VENDOR_IN     0xC0u
#define BMREQ_VENDOR_OUT    0x40u

/* The one vendor bRequest whose VALUE this file looks at.  Not to act on it —
 * the 0x40 path stays "accept anything, do nothing, ZLP" — but to use it as the
 * session-start marker; see usb_session_flush() and §11a. */
#define CH341_REQ_SERIAL_INIT   0xA1u

/* ------------------------------------------------------------------------- */
/* 2. Buffers and state                                                       */
/* ------------------------------------------------------------------------- */

/* Endpoint DMA buffers.  R16_UEPn_DMA is a 16-bit register holding the low half
 * of a 0x20000000-based address, so these must live in SRAM (they do) and be
 * 4-byte aligned — what WCH's own EVT declares and what all three shipping
 * addresses are.
 *
 * Sizes are what the HARDWARE window is, not what the application allocated:
 *   EP0  single shared 64-byte RX+TX window   (the application allocates 8)
 *   EP1  64-byte TX window, never driven      (the application allocates 8)
 *   EP2  128 bytes: RX at +0x00, TX at +0x40  (BUF_MOD clear)
 * volatile because the SIE writes them behind our back. */
static volatile uint8_t ep0_buf[64]  __attribute__((aligned(4)));
static volatile uint8_t ep1_buf[64]  __attribute__((aligned(4)));
static volatile uint8_t ep2_buf[128] __attribute__((aligned(4)));

#define EP2_RX_OFF          0x00u
#define EP2_TX_OFF          0x40u

/* Control-transfer state.  All of it is reset on bus reset; see usb_bus_reset. */
static const uint8_t *ep0_descr;    /* running source pointer, GET_DESCRIPTOR */
static uint16_t       ep0_req_len;  /* bytes remaining; ALSO the parked device
                                     * address between SET_ADDRESS and its
                                     * status stage — WCH's own idiom, and what
                                     * the application does at 0x4936/0x4A18   */
static uint8_t        ep0_req_code; /* bRequest of the transfer in flight      */
static uint8_t        ep0_req_type; /* bmRequestType of the transfer in flight */
static uint8_t        usb_config;   /* SET_CONFIGURATION value                 */
static uint8_t        ch340_cursor; /* index into the canned reply table       */

/* Bulk staging.  Linear with a rewind when fully drained, the same shape as the
 * application's buffers (which are NOT rings, despite what the vendor SDK
 * says).  Occupancy is head - tail; free linear space is size - head. */
static uint8_t  rx_buf[BL_USB_RX_BUF_SIZE];
static uint16_t rx_head, rx_tail;
static uint8_t  tx_buf[BL_USB_TX_BUF_SIZE];
static uint16_t tx_head, tx_tail;
static uint8_t  tx_armed;           /* explicit, replaces the app's implicit
                                     * "free == tx_size" test                 */

/* Bumped every time the staging buffers are flushed because a NEW host session
 * began: a USB bus reset, or the ch34x port-open marker (§11a).  Exposed as
 * bl_usb_session() so the framer can resynchronise instead of parsing the tail
 * of somebody else's frame. */
static uint32_t session_id;

/* Receive credit threshold.  The application uses 32 (== wMaxPacketSize); 64 is
 * the full hardware RX window and makes an over-long packet from a misbehaving
 * host structurally unable to overrun the staging buffer. */
#define RX_CREDIT_BYTES     64u

/* ------------------------------------------------------------------------- */
/* 3. Small helpers — no libc                                                 */
/* ------------------------------------------------------------------------- */

/* Both pointers are volatile-qualified.  That is correct for the DMA buffers
 * and it also stops GCC's loop-distribution pass rewriting the loop into a call
 * to memcpy, which does not exist under -nostdlib.  (The Makefile passes
 * -fno-builtin -fno-tree-loop-distribute-patterns as well; this belt is here so
 * the file is also safe when compiled with a plainer flag set.) */
static void usb_copy(volatile uint8_t *dst, const volatile uint8_t *src,
                     uint32_t n)
{
    while (n != 0u) {
        *dst = *src;
        dst++;
        src++;
        n--;
    }
}

/* THE ONLY BUSY-WAIT LEFT IN THIS FILE; its only caller is the USB PLL settle
 * in usb_pll_power_on().
 *
 * WHY THIS ONE STAYS.  It is a HARDWARE settle, not a software timeout: the
 * SIE's PLL needs a few milliseconds after RB_CLK_PLL_PON before the USB
 * registers may be touched, and that must hold whether or not bl_time_init()
 * has been called.  A clocked wait here would hang (bl_time_ms() freezes when
 * the time base is stopped) or need this loop as a fallback anyway.
 *
 * REAL DURATION, counted off the emitted code: the inner loop is 14 core cycles
 * (ldr/subs/str/b = 8, ldr/cmp/bne = 6), so 4000 iterations is 56,000 cycles =
 * 1.75 ms of wall clock per NOMINAL millisecond at Fsys = 32 MHz.  The one call
 * passes BL_USB_PLL_SETTLE_MS = 5, i.e. ~8.75 ms real, against
 * BL_USB_INIT_MAX_GAP_MS = 16 ms.  Biased LONG, the correct direction for a PLL
 * settle, which is why the count is not recalibrated.
 *
 * THE COUNTER IS volatile ON PURPOSE.  It forces GCC to materialise the
 * loop-entry test even when the argument is a known non-zero constant.  Without
 * it the optimiser folds the guard and emits a do-while, and a future computed
 * 0 would underflow to 0xFFFFFFFF and spin ~43 days at 32 MHz inside a
 * bootloader with no watchdog.
 *
 * Compiled only alongside the PLL settle: with BL_USB_PLL_POWER_ON 0 (the host
 * model's build, whose safe-access window is Thumb asm that cannot be assembled
 * natively) this would be an unused static. */
#if BL_USB_PLL_POWER_ON
static void usb_delay_ms(uint32_t ms)
{
    volatile uint32_t left = ms;

    while (left != 0u) {
        volatile uint32_t n = 4000u;
        while (n != 0u) {
            n--;
        }
        left--;
    }
}
#endif /* BL_USB_PLL_POWER_ON */

/* ------------------------------------------------------------------------- */
/* 3b. The ONE unsafe operation on this endpoint: R8_UEP2_CTRL read-modify-write
 * ------------------------------------------------------------------------- */
/*
 * R8_UEP2_CTRL is a single byte that BOTH software and the SIE write:
 *
 *   software owns  MASK_UEP_R_RES (0x0C), MASK_UEP_T_RES (0x03),
 *                  RB_UEP_AUTO_TOG (0x10)
 *   hardware owns  RB_UEP_R_TOG (0x80)  — advanced after an accepted OUT
 *                  RB_UEP_T_TOG (0x40)  — advanced after an accepted IN
 *
 * EP2 has RB_UEP_AUTO_TOG set, so the toggles are hardware-managed and there is
 * no software toggle handling anywhere on the bulk path.
 * Changing only a response field therefore requires ldrb / orrs / strb — and if
 * the SIE completes a transaction between the ldrb and the strb, the strb writes
 * back the PRE-transaction toggle.
 *
 * Consequence, and it is silent: the endpoint's expected DATA0/DATA1 is rolled
 * back one step.  The host's next packet carries the toggle the SIE is no longer
 * expecting; the SIE ACKs it on the wire (so the host records it as delivered)
 * but reports RB_UIS_TOG_OK clear, and the TOK_OUT_EP2 case below correctly
 * refuses to take it as new data.  A packet vanishes.  On the transmit side the
 * mirror image happens: an IN packet goes out with the stale PID and the host
 * discards it as a retransmission.  At the protocol layer that is a truncated
 * frame; an echo test is too coarse to see it reliably.
 *
 * WHAT MAKES A WRITE SAFE.  R8_USB_CTRL carries RB_UC_INT_BUSY, so while
 * RB_UIF_TRANSFER is set the SIE answers every incoming transaction with
 * busy/NAK on its own and completes nothing (see the polling note above).
 * Inside that window — exactly the body of bl_usb_poll()'s transfer
 * branch, up to the store that clears the flag — the SIE is quiescent and a
 * plain read-modify-write is atomic with respect to it.  Masking interrupts
 * would NOT help: the competing writer is the SIE, a hardware block, not an
 * exception handler.
 *
 * WHY A HELPER RATHER THAN "MOVE THE CALLERS INSIDE".  Receive credit has to be
 * re-opened after the consumer drains the staging buffer, and the transmit
 * "kick" has to arm the first IN packet.  Deferring either to the next transfer
 * window deadlocks the link in exactly the case where no transfer can complete
 * BECAUSE the endpoint is parked (a NAKed OUT and a NAKed IN both raise no
 * RB_UIF_TRANSFER; RB_UIE_DEV_NAK is not set in R8_USB_INT_EN = 0x07).
 *
 * THE PROTOCOL.  Bracket the read-modify-write with reads of RB_UIF_TRANSFER,
 * using the fact that the flag LATCHES and is cleared by exactly one store in
 * this file.
 *
 *   flag set before the pair      -> window already open, SIE quiescent for the
 *                                    whole pair.  Plain RMW; done.
 *   flag set across the pair      -> a transaction completed.  Repaired rather
 *                                    than trusted: the SIE is quiescent NOW and
 *                                    R8_USB_INT_ST is latched alongside the flag
 *                                    and says which toggle moved, so the correct
 *                                    absolute value is written.
 *   flag clear throughout         -> nothing completed; the value written is
 *                                    current by construction.
 *
 * "Completed before our read" vs "after our read" is resolved by re-reading the
 * flag BEFORE the store: if it went set there the loop repeats, and the second
 * pass necessarily takes the window-open branch because nothing clears the flag
 * in between.  At most two passes; it cannot spin.
 *
 * WHAT THIS DOES NOT REMOVE.  It rests on the SIE having updated RB_UEP_R_TOG /
 * RB_UEP_T_TOG by the time RB_UIF_TRANSFER reads back set.  That ordering is
 * inferred, not documented — but it is the same assumption the whole polled
 * design rests on and that hardware has validated: bl_usb_poll() reads
 * R8_USB_RX_LEN and the DMA buffer only after seeing that flag.  Nor does it
 * make the register safe against a second concurrent software writer; there is
 * none, and if one is ever added this helper is not the answer.
 *
 * If RB_UEP_AUTO_TOG turns out to make the toggle bits ignore software stores
 * altogether, the clobber never happened and the repair store is likewise
 * ignored: this code is correct under both readings.
 */

/* clear_mask must name ONLY software-owned bits; RB_UEP_R_TOG / RB_UEP_T_TOG
 * are preserved from the read and never selected by any caller here. */
static void uep2_ctrl_rmw(uint8_t clear_mask, uint8_t set_bits)
{
    uint8_t ctrl;
    uint8_t val;
    uint8_t st;
    uint8_t tok;
    uint8_t fix;
    uint32_t pass;

    for (pass = 0u; pass < 2u; pass++) {

        if ((R8_USB_INT_FG & RB_UIF_TRANSFER) != 0u) {
            /* Busy window open.  RB_UC_INT_BUSY holds the SIE off until the
             * flag is cleared, which only bl_usb_poll() does, and only after
             * this returns.  Plain RMW. */
            ctrl = R8_UEP2_CTRL;
            R8_UEP2_CTRL = (uint8_t)((ctrl & (uint8_t)~clear_mask) | set_bits);
            return;
        }

        ctrl = R8_UEP2_CTRL;

        if ((R8_USB_INT_FG & RB_UIF_TRANSFER) != 0u) {
            /* A transaction completed on or before the read above; whether the
             * value in `ctrl' predates or postdates its toggle update cannot be
             * told apart from here.  Go round again — the flag is latched, so
             * the next pass takes the window-open branch and reads a value that
             * is unambiguously current. */
            continue;
        }

        /* Nothing had completed as of the read.  Commit. */
        val = (uint8_t)((ctrl & (uint8_t)~clear_mask) | set_bits);
        R8_UEP2_CTRL = val;

        if ((R8_USB_INT_FG & RB_UIF_TRANSFER) == 0u) {
            return;                     /* nothing completed across the store  */
        }

        /* A transaction completed across the store.  `ctrl' is known to predate
         * it, so the toggle just written is one step behind — unless the SIE's
         * update landed after our store, in which case it is correct and
         * rewriting the same absolute value is a no-op.  Either way, writing the
         * value the SIE should now be holding is right, and it is safe because
         * the flag is set and the SIE is therefore quiescent.
         *
         * R8_USB_INT_ST is read-only and latched with the flag; bl_usb_poll()
         * has not consumed it yet and will read the same bytes when it does. */
        st  = R8_USB_INT_ST;
        tok = (uint8_t)(st & MASK_UIS_TOKEN_EP);
        fix = 0u;

        if (tok == TOK_OUT_EP2) {
            /* Only an in-sequence packet advances the receive toggle; a
             * mis-toggled one is a host retransmission and leaves it alone. */
            if ((st & RB_UIS_TOG_OK) != 0u) {
                fix = RB_UEP_R_TOG;
            }
        } else if (tok == TOK_IN_EP2) {
            fix = RB_UEP_T_TOG;
        } else {
            /* An EP0 token.  The SIE does not touch R8_UEP2_CTRL for those. */
        }

        if (fix != 0u) {
            R8_UEP2_CTRL = (uint8_t)(val ^ fix);
        }
        return;
    }

    /* Unreachable: pass 0 can only `continue' with the flag set, and nothing
     * clears it before pass 1 tests it.  Falling out would mean the requested
     * change was not applied, which the next bl_usb_poll() re-applies anyway. */
}

/* ------------------------------------------------------------------------- */
/* 4. Clock/power precondition: the USB PLL                                   */
/* ------------------------------------------------------------------------- */
/*
 * bsp_init (0x42B6) calls PWR_UnitModCfg(ENABLE, UNIT_SYS_PLL) — a safe-access
 * window then R8_HFCK_PWR_CTRL |= 0x10 (RB_CLK_PLL_PON) — waits 3 ms, and only
 * then sets the system clock.  Fsys does not use the PLL (R16_CLK_SYS_CFG =
 * 0x0088 is "directly from the 32 MHz RC"), so the PLL is powered for the USB
 * SIE and nothing else.  start.S does not do this today.
 *
 * READ-MODIFY-WRITE, never a plain store: RB_CLK_INT32M_PON (0x08) is already
 * set in that register and Fsys is running off it — clobbering it stops the
 * core.  The vendor's PWR_UnitModCfg is an RMW for the same reason.
 *
 * Written as inline assembly with the read hoisted OUT of the safe-access
 * window so that exactly zero instructions sit between the 0xA8 signature store
 * and the protected store.  The window is only ~16 Tsys wide and an overrun
 * silently DROPS the write; leaving the sequence to the compiler's scheduler is
 * not a risk worth taking for four instructions.  Failure mode if it were
 * missed is "PLL stays off", i.e. no enumeration — not corruption.
 */
#if BL_USB_PLL_POWER_ON
#define RB_CLK_PLL_PON      0x10u

/* The signature bytes are spelled out in the asm template rather than passed as
 * operands, so that the instruction sequence cannot be perturbed by register
 * allocation.  These assertions keep them tied to bl_config.h. */
__extension__ _Static_assert(BL_SAFE_ACCESS_SIG1 == 0x57, "SAM signature 1");
__extension__ _Static_assert(BL_SAFE_ACCESS_SIG2 == 0xA8, "SAM signature 2");

static void usb_pll_power_on(void)
{
    uint32_t sig = (uint32_t)BL_R8_SAFE_ACCESS_SIG;   /* 0x40001040 */
    uint32_t hfc = (uint32_t)BL_R8_HFCK_PWR_CTRL;     /* 0x4000100A */
    uint32_t t, u;

    __asm volatile (
        /* GCC wraps inline asm in `.syntax divided' unless built with
         * -masm-syntax-unified, and re-asserts unified afterwards, so this line
         * is required for the three-operand form below and is a no-op if the
         * surrounding mode is already unified. */
        ".syntax unified          \n\t"
        "ldrb  %[t], [%[hf]]      \n\t"   /* read current, PON bits live      */
        "movs  %[u], #0x10        \n\t"   /* RB_CLK_PLL_PON                   */
        "orrs  %[t], %[t], %[u]   \n\t"
        "movs  %[u], #0x57        \n\t"
        "strb  %[u], [%[sg]]      \n\t"   /* unlock 1 of 2                    */
        "movs  %[u], #0xA8        \n\t"
        "strb  %[u], [%[sg]]      \n\t"   /* unlock 2 of 2 — window opens     */
        "strb  %[t], [%[hf]]      \n\t"   /* ...protected store is next       */
        "movs  %[u], #0           \n\t"
        "strb  %[u], [%[sg]]      \n\t"   /* re-lock                          */
        : [t] "=&l" (t), [u] "=&l" (u)
        : [hf] "l" (hfc), [sg] "l" (sig)
        : "memory", "cc");

    /* The application waits 3 ms; the WCH community example waits 5 with the
     * comment "CPU waits ~5 ms so the PLL fully opens". */
    usb_delay_ms(BL_USB_PLL_SETTLE_MS);
}
#endif /* BL_USB_PLL_POWER_ON */

/* ------------------------------------------------------------------------- */
/* 5. Bulk receive plumbing                                                   */
/* ------------------------------------------------------------------------- */

static uint16_t rx_free(void)
{
    return (uint16_t)(BL_USB_RX_BUF_SIZE - rx_head);
}

/* Open or close the bulk OUT endpoint purely on receive-buffer space.  Called
 * from the poll loop and from bl_usb_rx.  NEVER called from the transmit path:
 * the application's usb_tx_pump re-ACKs OUT blind to receive state, which is
 * how it can march rx_head to the end of the buffer and then silently discard a
 * whole packet. */
static void rx_credit_update(void)
{
    uint8_t want = (rx_free() >= RX_CREDIT_BYTES) ? UEP_R_RES_ACK
                                                  : UEP_R_RES_NAK;

    /* Write only on a transition — a store that changes nothing is still a
     * store, and each one is an exposure to the hazard §3b describes.  The
     * probe read here is not part of the read-modify-write: uep2_ctrl_rmw()
     * re-reads under its own guard, and a toggle bit moving between this read
     * and that one cannot affect the comparison, which looks at MASK_UEP_R_RES
     * only — a software-owned field the SIE never writes.
     *
     * Called from the poll loop and from bl_usb_rx(), both OUTSIDE the
     * RB_UIF_TRANSFER window, so it must go through the guarded helper. */
    if ((uint8_t)(R8_UEP2_CTRL & MASK_UEP_R_RES) != want) {
        uep2_ctrl_rmw(MASK_UEP_R_RES, want);
    }
}

/* One accepted bulk OUT packet.  Credit is only ever granted when a whole
 * hardware window fits, so the clamp below is defence in depth against a host
 * that sends more than wMaxPacketSize, not normal flow control. */
static void rx_deliver(uint32_t len)
{
    uint16_t space = rx_free();

    if (len > sizeof ep2_buf - EP2_TX_OFF) {   /* the RX window is 64 bytes   */
        len = sizeof ep2_buf - EP2_TX_OFF;
    }
    if (len > space) {
        len = space;                           /* unreachable given credit    */
    }
    if (len != 0u) {
        usb_copy(&rx_buf[rx_head], &ep2_buf[EP2_RX_OFF], len);
        rx_head = (uint16_t)(rx_head + len);
    }
}

/* ------------------------------------------------------------------------- */
/* 6. Bulk transmit plumbing                                                  */
/* ------------------------------------------------------------------------- */

/* Arm the next IN packet, or park the endpoint at NAK when drained.  This is
 * the ONLY place R8_UEP2_T_LEN and the T_RES field are written on the bulk
 * path.  There is no zero-length-packet logic and there must not be: the host
 * reads a byte stream and no ch341 driver waits for a short packet. */
static void tx_pump(void)
{
    uint16_t n = (uint16_t)(tx_head - tx_tail);

    /* Both arms touch only MASK_UEP_T_RES and both go through the guarded
     * helper (§3b).  Two of the three call sites are outside the transfer
     * window: the "kick" at the tail of bl_usb_poll(), where receive credit has
     * not been re-evaluated yet so EP2 OUT may still be at R_RES=ACK and an OUT
     * can complete underneath the pair; and the one in tx_queue().  Only the
     * TOK_IN_EP2 call is inside the window. */
    if (n != 0u) {
        if (n > BL_USB_EP2_PKT) {
            n = BL_USB_EP2_PKT;
        }
        usb_copy(&ep2_buf[EP2_TX_OFF], &tx_buf[tx_tail], n);
        R8_UEP2_T_LEN = (uint8_t)n;
        tx_tail = (uint16_t)(tx_tail + n);
        uep2_ctrl_rmw(MASK_UEP_T_RES, UEP_T_RES_ACK);
        tx_armed = 1u;
    } else {
        tx_head = 0u;                 /* rewind: linear buffer, empty         */
        tx_tail = 0u;
        R8_UEP2_T_LEN = 0u;
        uep2_ctrl_rmw(MASK_UEP_T_RES, UEP_T_RES_NAK);
        tx_armed = 0u;
    }
}

/* Non-blocking queue.  Returns how many bytes were taken. */
static uint32_t tx_queue(const uint8_t *buf, uint32_t n)
{
    uint32_t room = (uint32_t)(BL_USB_TX_BUF_SIZE - tx_head);

    if (n > room) {
        n = room;
    }
    if (n != 0u) {
        usb_copy(&tx_buf[tx_head], buf, n);
        tx_head = (uint16_t)(tx_head + n);
        if (tx_armed == 0u) {
            tx_pump();                /* the "kick": nothing else starts the
                                       * chain, and without it the endpoint
                                       * sits at NAK forever (datapath §3.5)  */
        }
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* 6b. Session boundaries — flushing stale bytes on tty close/reopen          */
/* ------------------------------------------------------------------------- */
/*
 * THE PROBLEM.  A bus reset is NOT what happens when a tty is closed and
 * reopened.  Closing a ch341 port kills the driver's URBs and (on macOS and on
 * Linux) leaves the device addressed, configured and electrically attached; no
 * reset is issued.  So on the next open:
 *
 *   - rx_buf still holds whatever the previous session had sent and not
 *     consumed, and bl_usb_rx() hands those bytes to the new session as if they
 *     had just arrived;
 *   - tx_buf still holds whatever the previous session had queued and not
 *     drained, and — worse — tx_armed may be 1 with a packet already staged at
 *     ep2_buf+0x40 and T_RES=ACK, so the very first bulk IN of the NEW session
 *     is answered with the OLD session's bytes.
 *
 * The framer's first read is then garbage and the first exchange desyncs.
 *
 * THE PROXY.  A close cannot be observed — the device is never told.  The OPEN
 * can: every ch34x driver configures the chip before it submits any bulk URB,
 * using bmRequestType 0x40 / bRequest 0xA1, CH341_REQ_SERIAL_INIT (the
 * baud-divisor write).  The application itself gates
 * its port_open bookkeeping on 0xA1, which is direct evidence that this is what
 * the driver sends at open.
 *
 * Flushing on 0xA1 is therefore "flush when the host reconfigures the line",
 * which is also correct if a driver re-issues it mid-session for a tcsetattr —
 * reconfiguring a real UART discards what was in flight too (TCSAFLUSH).
 *
 * WHAT IS NOT DONE HERE.  The CH340 vendor-IN replay cursor is deliberately NOT
 * rewound: the canned table is enumeration state and is reset on bus reset.  The
 * 0x40 path still acts on nothing — ZLP as before, wValue/wIndex discarded.
 *
 * And the toggles are left alone.  A bus reset makes the host restart both data
 * toggles at DATA0, which is why usb_bus_reset() writes the absolute 0x12; a
 * port reopen does not, so the endpoint's toggles must survive this flush or the
 * first packet of the new session is discarded by one side or the other.
 * uep2_ctrl_rmw() preserves them.
 */
static void usb_session_flush(void)
{
    rx_head  = 0u;
    rx_tail  = 0u;
    tx_head  = 0u;
    tx_tail  = 0u;

    /* Cancel any packet already staged and armed for the next IN.  Clearing the
     * staging pointers alone is NOT enough: tx_armed==1 means a packet is
     * already sitting at ep2_buf+0x40 with T_RES=ACK, and the SIE would hand
     * those bytes to the first bulk IN of the new session.  T_RES=NAK withdraws
     * it.  The host is between sessions and has no read outstanding, so nothing
     * is owed. */
    R8_UEP2_T_LEN = 0u;
    uep2_ctrl_rmw(MASK_UEP_T_RES, UEP_T_RES_NAK);
    tx_armed = 0u;

    /* Receive credit is re-derived from the (now empty) staging buffer, so this
     * can only open the endpoint, never close it.  bl_usb_poll()'s tail would
     * do it anyway on the way out of this transfer; doing it here keeps the
     * function correct on its own terms rather than by coupling. */
    rx_credit_update();

    session_id++;
}

/* ------------------------------------------------------------------------- */
/* 7. Control transfers                                                       */
/* ------------------------------------------------------------------------- */

/* bmRequestType 0xC0 — the CH340 vendor-IN replay.
 *
 * bRequest, wValue and wIndex are NEVER examined, exactly as at 0x4804: the
 * gate is a full-byte compare on bmRequestType and the answer depends only on
 * how many 0xC0 requests have arrived.  CH341_REQ_READ_VERSION (0x5F) and
 * CH341_REQ_READ_REG (0x95) are indistinguishable to this firmware, which is
 * why the table can only be reproduced, not decoded. */
static void ep0_vendor_in(void)
{
    uint8_t i = ch340_cursor;

    ep0_buf[0] = bl_usb_ch340_vendor_tbl[i];
    ep0_buf[1] = bl_usb_ch340_vendor_tbl[i + 1u];

    /* Saturates at the last pair; it does NOT wrap (cmp #0x18 / bge). */
    ch340_cursor = (uint8_t)((i >= BL_USB_VENDOR_TBL_LAST)
                             ? BL_USB_VENDOR_TBL_LAST : (i + 2u));
}

/* Standard requests.  Returns 1 to STALL.
 *
 * Written as an if/else chain, AND compiled with jump tables disabled: given
 * four codes spanning 5..9 GCC will otherwise build a Thumb-1 dispatch table
 * and call __gnu_thumb1_case_sqi, which lives in libgcc — an unresolved symbol
 * under the Makefile's -nostdlib link (verified: the helper appears with the
 * switch and with the plain chain, and disappears with this attribute).  The
 * application's own decoder at 0x48B4 is a compare chain for the same reason.
 * The cost is a handful of bytes; the alternative is a link failure. */
__attribute__((optimize("no-jump-tables")))
static int ep0_standard(uint16_t wvalue)
{
    uint32_t type;
    uint32_t n;

    if (ep0_req_code == REQ_SET_ADDRESS) {
        /* Parked in ep0_req_len and applied at the status stage, after the
         * transaction that set it has completed.  Applying it here would make
         * the device stop answering that very transaction. */
        ep0_req_len = (uint16_t)(wvalue & 0xFFu);

    } else if (ep0_req_code == REQ_GET_DESCRIPTOR) {
        type = (uint32_t)(wvalue >> 8);
        if (type == DESC_TYPE_DEVICE) {
            ep0_descr = bl_usb_desc_device;
            n = bl_usb_desc_device[0];              /* bLength                */
        } else if (type == DESC_TYPE_CONFIG) {
            ep0_descr = bl_usb_desc_config;
            n = bl_usb_desc_config[2];              /* low byte of wTotalLen  */
        } else {
            return 1;                               /* incl. type 3, STRING   */
        }
        /* The descriptor INDEX (low byte of wValue) is ignored, as at 0x48CC. */
        if (ep0_req_len > n) {
            ep0_req_len = (uint16_t)n;
        }
        n = (ep0_req_len >= BL_USB_EP0_PKT) ? BL_USB_EP0_PKT : ep0_req_len;
        usb_copy(ep0_buf, ep0_descr, n);
        ep0_descr += n;

    } else if (ep0_req_code == REQ_GET_CONFIG) {
        ep0_buf[0] = usb_config;
        if (ep0_req_len > 1u) {
            ep0_req_len = 1u;
        }

    } else if (ep0_req_code == REQ_SET_CONFIG) {
        usb_config = (uint8_t)(wvalue & 0xFFu);

    } else {
        /* GET_STATUS, CLEAR_FEATURE, SET_FEATURE, SET_DESCRIPTOR,
         * GET_INTERFACE, SET_INTERFACE, SYNCH_FRAME — all STALL, as they do in
         * the application.  Do not add handlers: the shipping behaviour is what
         * the host this device works with today has accepted. */
        return 1;
    }
    return 0;
}

static void ep0_setup(void)
{
    uint16_t wvalue;
    uint32_t n;
    int      stall = 0;

    R8_UEP0_CTRL = UEP0_CTRL_SETUP_ARM;

    if (R8_USB_RX_LEN != 8u) {
        R8_UEP0_CTRL = UEP0_CTRL_STALL;
        return;
    }

    ep0_req_len  = (uint16_t)(ep0_buf[6] | ((uint16_t)ep0_buf[7] << 8));
    ep0_req_code = ep0_buf[1];
    ep0_req_type = ep0_buf[0];
    wvalue       = (uint16_t)(ep0_buf[2] | ((uint16_t)ep0_buf[3] << 8));

    if (ep0_req_type == BMREQ_VENDOR_IN) {
        ep0_vendor_in();
    } else if (ep0_req_type == BMREQ_VENDOR_OUT) {
        /* Accept anything, do nothing, answer with a ZLP status.  The
         * application's three arms (0x9A WRITE_REG, 0xA1 SERIAL_INIT, 0xA4
         * MODEM_CTRL) only ever write two bytes that nothing in the image
         * reads, and its unknown-request arm already does nothing.  So this is
         * indistinguishable on the wire, and it is also the answer to "what
         * about the baud divisor": there is no UART, the divisor is discarded,
         * and it must NOT be validated.
         *
         * The one thing bRequest is consulted for is the session marker: 0xA1
         * CH341_REQ_SERIAL_INIT is the host reconfiguring the line, which is
         * the only observable proxy for "a tty was just opened".  See §6b.  It
         * changes nothing about the response — every 0x40 request, this one
         * included, still falls through to the ZLP status stage below. */
        (void)wvalue;
        if (ep0_req_code == CH341_REQ_SERIAL_INIT) {
            usb_session_flush();
        }
    } else {
        stall = ep0_standard(wvalue);
    }

    if (stall != 0) {
        R8_UEP0_CTRL = UEP0_CTRL_STALL;
        return;
    }

    if ((ep0_req_type & 0x80u) != 0u) {          /* device-to-host            */
        n = (ep0_req_len > BL_USB_EP0_PKT) ? BL_USB_EP0_PKT : ep0_req_len;
        ep0_req_len = (uint16_t)(ep0_req_len - n);
    } else {                                     /* host-to-device: no data   */
        n = 0u;
    }
    R8_UEP0_T_LEN = (uint8_t)n;
    R8_UEP0_CTRL  = UEP0_CTRL_DATA;              /* T_TOG=1: first IN is DATA1 */
}

static void ep0_in(void)
{
    uint32_t n;

    switch (ep0_req_code) {

    case REQ_GET_DESCRIPTOR:
        n = (ep0_req_len >= BL_USB_EP0_PKT) ? BL_USB_EP0_PKT : ep0_req_len;
        if (ep0_descr == 0) {                    /* stale request after reset */
            n = 0u;
        } else {
            usb_copy(ep0_buf, ep0_descr, n);
            ep0_descr += n;
        }
        ep0_req_len = (uint16_t)(ep0_req_len - n);
        R8_UEP0_T_LEN = (uint8_t)n;
        /* EP0 has no AUTO_TOG, so the data toggle is flipped by hand.  A short
         * or zero-length packet terminates the transfer; no STALL is needed.
         *
         * This is the file's only other read-modify-write of an endpoint
         * control register and it needs no guard, for two independent reasons:
         * R8_UEP0_CTRL has no hardware-managed bits at all (§3b applies to EP2,
         * which is the only endpoint with AUTO_TOG set), and this runs from
         * bl_usb_poll()'s transfer branch, i.e. inside the RB_UIF_TRANSFER
         * window where the SIE is quiescent regardless. */
        R8_UEP0_CTRL = (uint8_t)(R8_UEP0_CTRL ^ RB_UEP_T_TOG);
        break;

    case REQ_SET_ADDRESS:
        /* Preserve RB_UDA_GP_BIT (0x80), as the application does at 0x4A18. */
        R8_USB_DEV_AD = (uint8_t)((R8_USB_DEV_AD & 0x80u)
                                  | (uint8_t)(ep0_req_len & 0x7Fu));
        R8_UEP0_CTRL  = UEP0_CTRL_IDLE;
        break;

    default:
        R8_UEP0_T_LEN = 0u;
        R8_UEP0_CTRL  = UEP0_CTRL_IDLE;
        break;
    }
}

static void ep0_out(void)
{
    /* No control transfer this device answers has an OUT data stage, so there
     * is nothing to consume.  The application calls a hook here that is a bare
     * `bx lr` (0xABD0). */
    R8_UEP0_T_LEN = 0u;
    R8_UEP0_CTRL  = UEP0_CTRL_IDLE;
}

/* ------------------------------------------------------------------------- */
/* 8. Bus reset                                                               */
/* ------------------------------------------------------------------------- */
/*
 * The application does six writes here and that is NOT enough — copying it
 * verbatim produces exactly the "enumerates once and then dies" failure, because
 * the CH340 replay cursor is clamped at its last entry and never returns to 0
 * (see the divergence list above).  Everything below the endpoint
 * registers is state the application leaves stale across a reset.
 *
 * What must NOT happen here: re-running the init sequence.  A USB bus reset does
 * not reset the SIE's configuration — R8_USB_CTRL, R8_UDEV_CTRL, R8_USB_INT_EN,
 * the UEPn_MOD registers, the DMA pointers and R16_PIN_ANALOG_IE all survive and
 * must be left alone.  usb_device_init()'s first write is R8_USB_CTRL = 0, which
 * drops the D+ pull-up and forces the host to start over.
 *
 * Also NOT here: anything touching the update magic or boot state.  A host that
 * resets the bus must not be able to knock the bootloader out of update mode.
 *
 * A normal macOS/Linux enumeration issues more than one bus reset (reset ->
 * GET_DESCRIPTOR(8) -> reset -> SET_ADDRESS -> ...), so surviving at least two
 * resets in the first 200 ms with no loss of function is the acceptance test.
 */
static void usb_bus_reset(void)
{
    /* The six the application does. */
    R8_USB_DEV_AD = 0u;
    R8_UEP0_CTRL  = UEP0_CTRL_IDLE;   /* clears both EP0 toggles              */
    R8_UEP1_CTRL  = UEPn_CTRL_BULK;   /* EP1 stays NAKed forever              */
    R8_UEP2_CTRL  = UEPn_CTRL_BULK;   /* OUT re-armed, IN cancelled, DATA0    */

    /* Belt and braces: UEPn_CTRL_BULK already NAKs the IN direction. */
    R8_UEP2_T_LEN = 0u;

    /* The ones it forgets. */
    ch340_cursor = 0u;                /* deliberate divergence, §6.2          */
    ep0_descr    = 0;
    ep0_req_len  = 0u;
    ep0_req_code = 0u;
    ep0_req_type = 0u;
    usb_config   = 0u;                /* unconfigured after a reset           */

    rx_head  = 0u;
    rx_tail  = 0u;
    tx_head  = 0u;
    tx_tail  = 0u;
    tx_armed = 0u;

    /* A bus reset is also a session boundary, and the loudest one there is.
     * Counted here rather than by calling usb_session_flush(): the absolute
     * R8_UEP2_CTRL = 0x12 above has already disarmed the IN direction AND
     * cleared both toggles, which is what a bus reset requires and what a
     * reopen must not do (§6b). */
    session_id++;
}

/* ------------------------------------------------------------------------- */
/* 9. Bring-up                                                                */
/* ------------------------------------------------------------------------- */

/* USB_DeviceInit (0x4B24) reproduced write for write and in the same ORDER.
 * The order is the author's, not stock WCH, and so is the endpoint enable map
 * (UEP4_1_MOD = 0x40, UEP2_3_MOD = 0x0C — EP1 IN only, EP2 both ways, EP3/EP4
 * absent).  It is the map the descriptors advertise; do not substitute WCH's
 * 0xCC/0xCC. */
static void usb_device_init(void)
{
    R8_USB_CTRL   = 0x00u;                        /* off while configuring    */
    R8_UEP4_1_MOD = RB_UEP1_TX_EN;                /* 0x40                     */
    R8_UEP2_3_MOD = RB_UEP2_RX_EN | RB_UEP2_TX_EN;/* 0x0C, BUF_MOD clear      */

    R16_UEP0_DMA = (uint16_t)(uint32_t)(uintptr_t)ep0_buf;
    R16_UEP1_DMA = (uint16_t)(uint32_t)(uintptr_t)ep1_buf;
    R16_UEP2_DMA = (uint16_t)(uint32_t)(uintptr_t)ep2_buf;

    R8_UEP0_CTRL = UEP0_CTRL_IDLE;                /* 0x02                     */
    R8_UEP1_CTRL = UEPn_CTRL_BULK;                /* 0x12                     */
    R8_UEP2_CTRL = UEPn_CTRL_BULK;                /* 0x12                     */

    R8_USB_DEV_AD = 0x00u;

    /* RB_UC_INT_BUSY is what makes polling safe: the SIE auto-NAKs while
     * RB_UIF_TRANSFER is pending.  RB_UC_DEV_PU_EN attaches the D+ pull-up, so
     * the host sees the device from this store onward. */
    R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;   /* 0x29 */

    R8_USB_INT_FG = 0xFFu;                        /* clear every W1C flag     */

    R16_PIN_ANALOG_IE = (uint16_t)(R16_PIN_ANALOG_IE | RB_PIN_USB_IE);

    R8_UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;  /* 0x81                     */

    /* Byte-identical to the application.  This enables the SIE's own flag
     * sources; it does NOT deliver an exception, because IRQ6 is masked at the
     * NVIC and NVIC_ISER is never written.  Keeping it at 0x07 means the
     * unresolved question "are RB_UIF_* set when RB_UIE_* is clear?" never has
     * to be answered. */
    R8_USB_INT_EN = RB_UIE_SUSPEND | RB_UIE_TRANSFER | RB_UIE_BUS_RST; /* 0x07 */
}

void bl_usb_init(void)
{
    /* Mask IRQ6 at the controller and drop anything already pending, BEFORE
     * the pull-up goes up.  ISER is all-zero out of reset, but a warm reset
     * arriving from a running application (which does enable IRQ6) costs one
     * store to make certain. */
    NVIC_ICER = USB_IRQ_BIT;
    NVIC_ICPR = USB_IRQ_BIT;

#if BL_USB_PLL_POWER_ON
    usb_pll_power_on();
#endif

    ep0_descr    = 0;
    ep0_req_len  = 0u;
    ep0_req_code = 0u;
    ep0_req_type = 0u;
    usb_config   = 0u;
    ch340_cursor = 0u;
    rx_head      = 0u;
    rx_tail      = 0u;
    tx_head      = 0u;
    tx_tail      = 0u;
    tx_armed     = 0u;
    session_id   = 0u;

    usb_device_init();
}

/* ------------------------------------------------------------------------- */
/* 10. The poll loop                                                          */
/* ------------------------------------------------------------------------- */

#if BL_USB_ECHO
/* Stage-3 loopback.  One packet's worth per poll, and only as much as the
 * transmit stage can take, so the receive credit keeps back-pressure honest
 * instead of dropping bytes.  Consumes the receive buffer, so with echo built
 * in, bl_usb_rx() returns nothing to any other consumer — which is the point:
 * stage 4 builds with BL_USB_ECHO=0. */
static void usb_echo_pump(void)
{
    uint8_t  tmp[BL_USB_EP2_PKT];
    uint32_t room = (uint32_t)(BL_USB_TX_BUF_SIZE - tx_head);
    uint32_t n;

    if (room == 0u) {
        return;                      /* transmit full: leave it in rx_buf     */
    }
    if (room > sizeof tmp) {
        room = sizeof tmp;
    }
    n = bl_usb_rx(tmp, room);
    if (n != 0u) {
        (void)tx_queue(tmp, n);
    }
}
#endif

/* The tail of every poll: drain the echo loopback (stage 3 only), start the
 * transmit chain if something is queued and the endpoint is idle, and re-derive
 * receive credit.  Factored out so it can run INSIDE the RB_UIF_TRANSFER window
 * on the path where there is one.
 *
 * All three touch R8_UEP2_CTRL.  Calling them before the flag is cleared is what
 * the application does too — usb_rx_push re-ACKs EP2 OUT from inside the
 * handler, ahead of its own `R8_USB_INT_FG = RB_UIF_TRANSFER` — and it costs
 * nothing, because RB_UC_INT_BUSY only makes the SIE answer busy while the flag
 * is pending; the endpoint states written here take effect the moment it is
 * cleared, two instructions later.
 *
 * On the paths with no transfer pending there is no window to run in and the
 * guard inside uep2_ctrl_rmw() is doing the work instead. */
static void usb_pumps(void)
{
#if BL_USB_ECHO
    usb_echo_pump();
#endif

    /* If bytes were queued while the endpoint was idle, start the chain.  In
     * the application this "kick" lives inside cdc_write and missing it
     * deadlocks the link; doing it from the loop is strictly more robust. */
    if ((tx_armed == 0u) && (tx_head != tx_tail)) {
        tx_pump();
    }

    rx_credit_update();
}

void bl_usb_poll(void)
{
    /* Read the flag byte ONCE and test the cached copy, as the application does
     * at 0x47A4: the register is volatile and its read-only status bits change
     * underneath.  Priority is transfer, then bus reset, then suspend — if a
     * transfer and a reset are pending together the transfer wins and the reset
     * is picked up on the next call.  The flag latches; nothing is lost. */
    uint8_t fg = R8_USB_INT_FG;

    if ((fg & RB_UIF_TRANSFER) != 0u) {
        /* R8_USB_INT_ST must be read BEFORE the flag is cleared. */
        uint8_t st = R8_USB_INT_ST;

        switch ((uint8_t)(st & MASK_UIS_TOKEN_EP)) {

        case TOK_SETUP_EP0:
            ep0_setup();
            break;

        case TOK_IN_EP0:
            ep0_in();
            break;

        case TOK_OUT_EP0:
            ep0_out();
            break;

        case TOK_OUT_EP2:
            /* RB_UIS_TOG_OK clear means the host re-transmitted a packet we
             * already accepted; taking it again would duplicate data. */
            if ((st & RB_UIS_TOG_OK) != 0u) {
                rx_deliver(R8_USB_RX_LEN);
            }
            /* NAK UNCONDITIONALLY — a deliberate divergence, not a copy.  The
             * application's branch at 0x4A6C jumps PAST its NAK at 0x4A7C when
             * RB_UIS_TOG_OK is clear, leaving the endpoint at R_RES=ACK.
             *
             * Closing here keeps one invariant the application does not have:
             * receive credit is a pure function of receive-buffer space, granted
             * in exactly one place, so a mis-toggled retransmission can never
             * leave the endpoint open while the staging buffer is full.  It
             * costs nothing on the wire — rx_credit_update() at the tail of this
             * same poll re-opens the endpoint whenever there is room.
             *
             * This store IS inside the RB_UIF_TRANSFER window, so it was never
             * exposed; it goes through the guarded helper only so every write to
             * this register has one shape (which takes the plain-RMW fast
             * path). */
            uep2_ctrl_rmw(MASK_UEP_R_RES, UEP_R_RES_NAK);
            break;

        case TOK_IN_EP2:
            /* The ONLY "transmit completed" signal this hardware gives.  The
             * packet staged at ep2_buf+0x40 has been consumed by the host. */
            tx_pump();
            break;

        default:
            /* Includes 0x21, IN on EP1 — never serviced, by design. */
            break;
        }

        /* Run the pumps while the SIE is still held off, THEN clear.  Until
         * that store the SIE is auto-NAKing the bus on our behalf, which is
         * precisely what gives a polled loop unbounded service time — and it is
         * also the only interval in which R8_UEP2_CTRL can be modified with no
         * inference about SIE timing at all (§3b). */
        usb_pumps();
        R8_USB_INT_FG = RB_UIF_TRANSFER;
        return;
    }

    if ((fg & RB_UIF_BUS_RST) != 0u) {
        usb_bus_reset();
        R8_USB_INT_FG = RB_UIF_BUS_RST;
    } else if ((fg & RB_UIF_SUSPEND) != 0u) {
        /* Suspend and resume are indistinguishable here, exactly as in the
         * application, which reads R8_USB_MIS_ST and discards the result.  No
         * state is torn down: a suspend must not cost an in-flight session. */
        (void)R8_USB_MIS_ST;
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    } else if ((fg & RB_UIF_FIFO_OV) != 0u) {
        R8_USB_INT_FG = RB_UIF_FIFO_OV;
    } else {
        /* Only read-only status bits set.  The application stores unconditionally
         * here; skipping the write keeps a bus access out of the hot path. */
    }

    /* No window on these paths: the endpoint may be live, and uep2_ctrl_rmw()'s
     * detect-and-repair is what keeps the stores below safe.  This is also the
     * path that must NOT be skipped or deferred — a NAKed OUT and a NAKed IN
     * both complete nothing and raise no flag, so if credit were only ever
     * granted from inside a transfer window the link would deadlock exactly
     * when it was most needed. */
    usb_pumps();
}

/* ------------------------------------------------------------------------- */
/* 11. Public data path                                                       */
/* ------------------------------------------------------------------------- */

uint32_t bl_usb_rx(uint8_t *buf, uint32_t max)
{
    uint32_t n = (uint32_t)(uint16_t)(rx_head - rx_tail);

    if (n > max) {
        n = max;
    }
    if (n != 0u) {
        usb_copy(buf, &rx_buf[rx_tail], n);
        rx_tail = (uint16_t)(rx_tail + n);
    }
    /* Rewind when fully drained — the buffer is linear, not a ring. */
    if ((rx_head != 0u) && (rx_head == rx_tail)) {
        rx_head = 0u;
        rx_tail = 0u;
    }
    rx_credit_update();
    return n;
}

/* THE WAIT IS CLOCKED, NOT COUNTED.
 *
 * Every millisecond named here is a real one, read from bl_time_ms() — the same
 * free-running 24-bit SysTick every other timeout in the bootloader is built on.
 * Counting the wait with usb_delay_ms() instead would put the whole stall
 * between two consecutive bl_time_ms() calls, and timebase.h's one contract is
 * that no more than BL_TIME_MAX_GAP_MS (524 ms, one lap) passes between them.
 * Past that the lap is lost and the accumulator UNDER-counts, so every window
 * spanning the stall fires up to 524 ms LATE — bl_proto_idle at 574 ms instead
 * of 50, handover at ~774 ms.  That is the "device appears dead" failure this
 * timeout exists to prevent, reachable in exactly the case it was written for: a
 * host that has stopped issuing IN tokens.
 *
 * So the loop polls continuously rather than burst-then-sleep, and reads the
 * clock inside the wait.  The gap this function contributes is a POLL OR TWO,
 * not the budget — BL_USB_TX_MAX_GAP_MS in usb.h, 2 ms, certified there against
 * the longest path through bl_usb_poll().  Polling continuously also keeps the
 * link serviced for the whole wait, so the transfer resumes the instant the host
 * comes back.
 *
 * "OR TWO" IS EXACT, AND THE `continue` BELOW IS WHY.  When a poll frees
 * staging room the loop restarts without reading the clock, so two polls can
 * fall between two bl_time_ms() calls.  It cannot be three: a poll that frees
 * room frees a whole BL_USB_EP2_PKT (32 bytes), no caller in this bootloader
 * transmits more than BL_MAX_TX (28), so the tx_queue() after the continue
 * takes the remainder and `sent >= n` ends the loop.  A poll that frees
 * nothing leaves tx_head at BL_USB_TX_BUF_SIZE and falls through to the clock
 * read.  usb.h certifies the resulting bound; the count is stated here because
 * the shape of the loop is what has to keep it true.
 *
 * LIVENESS.  bl_time_ms() freezes when the time base is not running, and a
 * frozen clock makes a deadline unreachable, so the wait also gives up after
 * BL_USB_TX_DEAD_CLOCK_POLLS polls with the clock not having moved at all.
 * With the clock running that cannot fire (a millisecond arrives within a few
 * hundred polls, the cap is 4096); with it stopped it bounds the wait at ~40 ms
 * instead of forever.  Between the two tests this function terminates
 * unconditionally, which is the property that matters in a bootloader with no
 * watchdog. */
uint32_t bl_usb_tx(const uint8_t *buf, uint32_t n)
{
    uint32_t sent  = 0u;
    uint32_t start = BL_USB_TIME_MS();
    uint32_t frozen_polls = 0u;

    while (sent < n) {
        uint32_t now;

        sent += tx_queue(&buf[sent], n - sent);
        if (sent >= n) {
            break;
        }

        /* The staging buffer is full.  Pump the link: each bl_usb_poll()
         * services at most one transfer, so draining it takes one call per
         * 32-byte packet the host actually collects. */
        bl_usb_poll();
        if (tx_head < (uint16_t)BL_USB_TX_BUF_SIZE) {
            /* Room appeared — queue the rest.  This skips the clock read
             * below, which is why the certified gap is two polls and not one;
             * it can happen at most once per call.  See the header comment. */
            continue;
        }

        /* Still full.  A host that has stopped reading must not be able to hang
         * the bootloader, so this gives up and reports short — rather than
         * blocking forever, and rather than discarding silently the way the
         * application's cdc_write does.  Differences, never absolute values:
         * bl_time_ms() wraps at 2^32 ms. */
        now = BL_USB_TIME_MS();
        if ((uint32_t)(now - start) >= (uint32_t)BL_USB_TX_TIMEOUT_MS) {
            break;
        }
        if (now == start) {
            /* The clock has not advanced by even one millisecond.  Normal for
             * the first few hundred polls; past the cap it means the time base
             * is not running and the deadline above can never be reached. */
            frozen_polls++;
            if (frozen_polls >= (uint32_t)BL_USB_TX_DEAD_CLOCK_POLLS) {
                break;
            }
        }
    }
    return sent;
}

int bl_usb_configured(void)
{
    return (usb_config != 0u) ? 1 : 0;
}

uint32_t bl_usb_session(void)
{
    return session_id;
}
