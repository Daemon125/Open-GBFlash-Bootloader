/* usb_model.h — host-side model of the CH579 USB SIE, and a scripted host.
 *
 * Owner: cover:usb.  Consumed by host/test_usb.c only.
 *
 * WHAT THIS IS.  src/usb.c is a polled USB device driver whose only previous
 * verification was reading it and one hardware echo run.  This header is the
 * interface to a native model of the silicon it talks to, sufficient to run
 * src/usb.c unmodified on the build host:
 *
 *   - the USB register block is backed by a byte array and src/usb.c's MMIO
 *     accessors are redirected at it at compile time (usb_model.c #includes
 *     ../src/usb.c after re-#defining BL_REG8/16/32);
 *   - R8_USB_INT_FG is modelled as LATCHING, write-1-to-clear on bits 0..4;
 *   - R8_USB_INT_ST / R8_USB_RX_LEN / R8_USB_MIS_ST are read-only and a store
 *     to any of them is a fatal model error;
 *   - the hardware-managed data toggles RB_UEP_R_TOG / RB_UEP_T_TOG on EP2 are
 *     advanced BY THE MODEL inside the register byte that src/usb.c
 *     read-modify-writes, which is the whole point: a stale write-back really
 *     does roll the endpoint's expected PID back one step here, exactly as it
 *     would on the part, and the next host packet is then reported with
 *     RB_UIS_TOG_OK clear and dropped;
 *   - RB_UC_INT_BUSY is honoured: while RB_UIF_TRANSFER is pending the SIE
 *     completes nothing, which is what makes a polled design safe and what
 *     makes the two read-modify-write sites OUTSIDE that window the only ones
 *     that can race.
 *
 * WHAT IT IS NOT.  It is not a USB stack.  There is no bit stuffing, no frame
 * timing, no SOF, no error/retry counting, no low-level handshake beyond
 * ACK/NAK/STALL, and the 16-bit DMA pointer registers cannot hold a host
 * pointer so the endpoint buffers are reached by symbol rather than through
 * them (usbm_dma_matches() checks that src/usb.c programmed the registers with
 * the low half of the right addresses, which is all that is checkable here).
 *
 * INDEPENDENCE.  The bit constants below are spelled out again rather than
 * reused from src/usb.c.  A model that imports the DUT's constants cannot
 * detect a wrong constant in the DUT; these were taken from
 * the register-set analysis directly.
 */

#ifndef USB_MODEL_H
#define USB_MODEL_H

#include <stdint.h>

#include "usb.h"

/* ------------------------------------------------------------------ */
/* Register offsets from 0x40008000, and the bits that matter.         */
/* Transcribed from the register-set analysis, NOT from src/usb.c.     */
/* ------------------------------------------------------------------ */

#define USBM_O_USB_CTRL     0x00u
#define USBM_O_UDEV_CTRL    0x01u
#define USBM_O_INT_EN       0x02u
#define USBM_O_DEV_AD       0x03u
#define USBM_O_MIS_ST       0x05u
#define USBM_O_INT_FG       0x06u
#define USBM_O_INT_ST       0x07u
#define USBM_O_RX_LEN       0x08u
#define USBM_O_UEP4_1_MOD   0x0Cu
#define USBM_O_UEP2_3_MOD   0x0Du
#define USBM_O_UEP0_DMA     0x10u
#define USBM_O_UEP1_DMA     0x14u
#define USBM_O_UEP2_DMA     0x18u
#define USBM_O_UEP0_T_LEN   0x20u
#define USBM_O_UEP0_CTRL    0x22u
#define USBM_O_UEP1_CTRL    0x26u
#define USBM_O_UEP2_T_LEN   0x28u
#define USBM_O_UEP2_CTRL    0x2Au

#define USBM_A_UEP2_CTRL    0x4000802Au
#define USBM_A_UEP0_CTRL    0x40008022u
#define USBM_A_INT_FG       0x40008006u
#define USBM_A_UEP2_T_LEN   0x40008028u

/* R8_USB_CTRL */
#define USBM_UC_DEV_PU_EN   0x20u
#define USBM_UC_INT_BUSY    0x08u
#define USBM_UC_DMA_EN      0x01u

/* R8_USB_INT_FG.  Bits 0..4 write-1-to-clear, bits 5..7 read-only. */
#define USBM_UIF_BUS_RST    0x01u
#define USBM_UIF_TRANSFER   0x02u
#define USBM_UIF_SUSPEND    0x04u
#define USBM_UIF_HST_SOF    0x08u
#define USBM_UIF_FIFO_OV    0x10u
#define USBM_U_SIE_FREE     0x20u   /* read-only; see the note in usb_model.c */
#define USBM_FG_W1C_MASK    0x1Fu

/* R8_USB_INT_ST */
#define USBM_UIS_TOG_OK     0x40u
#define USBM_UIS_TOKEN_EP   0x3Fu

/* R8_UEPn_CTRL */
#define USBM_UEP_R_TOG      0x80u
#define USBM_UEP_T_TOG      0x40u
#define USBM_UEP_AUTO_TOG   0x10u
#define USBM_UEP_R_RES_M    0x0Cu
#define USBM_UEP_R_RES_ACK  0x00u
#define USBM_UEP_R_RES_TOUT 0x04u
#define USBM_UEP_R_RES_NAK  0x08u
#define USBM_UEP_R_RES_STL  0x0Cu
#define USBM_UEP_T_RES_M    0x03u
#define USBM_UEP_T_RES_ACK  0x00u
#define USBM_UEP_T_RES_TOUT 0x01u
#define USBM_UEP_T_RES_NAK  0x02u
#define USBM_UEP_T_RES_STL  0x03u

/* R8_USB_INT_ST token codes, (token << 4) | endpoint. */
#define USBM_TOK_OUT        0x00u
#define USBM_TOK_IN         0x20u
#define USBM_TOK_SETUP      0x30u

#define USBM_EP0_PKT        8u
#define USBM_EP2_PKT        32u
#define USBM_EP2_RX_WINDOW  64u     /* bytes of DMA the SIE may write on EP2 OUT */

/* ------------------------------------------------------------------ */
/* Transaction results                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    USBM_R_ACK = 0,   /* handshake ACK; for IN, data came back        */
    USBM_R_NAK,       /* endpoint NAKed, or the SIE answered busy     */
    USBM_R_STALL,     /* endpoint STALLed                             */
    USBM_R_NONE       /* no response at all (wrong address, detached) */
} usbm_res_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Cold start: zero the register file, drop every latch, forget every
 * injection.  Does NOT call bl_usb_init(); the caller does that so the writes
 * it performs can be observed. */
void usbm_power_on(void);

/* Wall-clock backstop.  Installs a SIGALRM handler that prints and _exit()s
 * non-zero, so any unbounded loop inside src/usb.c fails the run instead of
 * hanging CI.  Call once per test with a generous budget. */
void usbm_watchdog(unsigned secs);

/* Model-invariant violation: prints and exits non-zero.  Never returns. */
void usbm_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ------------------------------------------------------------------ */
/* Driving the device                                                  */
/* ------------------------------------------------------------------ */

/* The function the model calls to give the firmware CPU time.  Defaults to
 * bl_usb_poll().  A test that also needs to consume/produce bulk data
 * installs its own, which must call bl_usb_poll() itself. */
void usbm_set_service(void (*fn)(void));
void usbm_pump(unsigned n);
unsigned long usbm_service_count(void);

/* Hard cap on service calls, so a livelock inside a host helper is a failure
 * rather than a hang.  0 disables.  Reset by usbm_power_on(). */
void usbm_set_service_budget(unsigned long n);

/* ------------------------------------------------------------------ */
/* Raw bus transactions — one token each, no retry                     */
/* ------------------------------------------------------------------ */

usbm_res_t usbm_tok_setup(uint8_t addr, const uint8_t d[8]);
usbm_res_t usbm_tok_out(uint8_t addr, uint8_t ep,
                        const uint8_t *d, uint32_t n, int pid);
usbm_res_t usbm_tok_in(uint8_t addr, uint8_t ep,
                       uint8_t *d, uint32_t *n, int *pid);

void usbm_bus_reset(void);
void usbm_suspend(void);

/* Adversarial: make the NEXT accepted EP2 OUT report this R8_USB_RX_LEN
 * regardless of how many bytes were actually delivered.  Exercises the clamp
 * in rx_deliver().  0 disables. */
void usbm_force_next_rx_len(uint8_t v);

/* ------------------------------------------------------------------ */
/* Observation                                                         */
/* ------------------------------------------------------------------ */

uint8_t  usbm_reg8(unsigned off);
uint16_t usbm_reg16(unsigned off);
uint32_t usbm_nvic_icer(void);
uint32_t usbm_nvic_icpr(void);
int      usbm_iser_touched(void);      /* always 0 unless the DUT regressed  */
uint16_t usbm_pin_analog_ie(void);

int usbm_ep2_out_open(void);           /* R_RES == ACK                        */
int usbm_ep2_in_armed(void);           /* T_RES == ACK                        */
int usbm_ep2_r_tog(void);
int usbm_ep2_t_tog(void);
int usbm_ep0_stalled(void);

/* 1 if R16_UEPn_DMA hold the low halves of the three endpoint buffers. */
int usbm_dma_matches(void);

/* Counters, all reset by usbm_power_on(). */
unsigned long usbm_count_out_ep2(void);      /* accepted OUT packets, any tog */
unsigned long usbm_count_out_tog_bad(void);  /* of those, TOG_OK clear        */
unsigned long usbm_count_in_ep2(void);       /* IN packets handed to the host */
unsigned long usbm_count_fifo_ov(void);

/* ------------------------------------------------------------------ */
/* Scripted host                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t       addr;
    int           out_pid;      /* PID the host will put on the next bulk OUT */
    int           in_pid;       /* PID the host expects on the next bulk IN   */
    unsigned long dup_in;       /* IN packets discarded as retransmissions    */
    unsigned long nak_out;
    unsigned long nak_in;
    unsigned      retry;        /* NAK retries per packet before giving up    */
} usbm_host_t;

void usbm_host_init(usbm_host_t *h);

/* Control transfers.  Return 0 on success, -1 on STALL, -2 on protocol
 * breakage (unexpected PID, no response).  *outn is set on success. */
int usbm_h_ctrl_in(usbm_host_t *h, uint8_t bmreq, uint8_t breq,
                   uint16_t wval, uint16_t widx, uint16_t wlen,
                   uint8_t *out, uint32_t *outn);
int usbm_h_ctrl_out(usbm_host_t *h, uint8_t bmreq, uint8_t breq,
                    uint16_t wval, uint16_t widx);

/* Bulk.  Both retry through NAK, pumping the device between attempts, and
 * return how many bytes moved. */
uint32_t usbm_h_bulk_out(usbm_host_t *h, const uint8_t *d, uint32_t n);
uint32_t usbm_h_bulk_in(usbm_host_t *h, uint8_t *d, uint32_t max);

/* One packet, no retry, host toggle bookkeeping applied on ACK. */
usbm_res_t usbm_h_out_pkt(usbm_host_t *h, const uint8_t *d, uint32_t n);
usbm_res_t usbm_h_in_pkt(usbm_host_t *h, uint8_t *d, uint32_t *n);

/* One packet deliberately sent with the WRONG PID (a retransmission of the
 * packet the host already had ACKed).  Host toggle is not advanced. */
usbm_res_t usbm_h_out_pkt_retransmit(usbm_host_t *h, const uint8_t *d,
                                     uint32_t n);

/* Reset + the standard descriptor walk + SET_ADDRESS + SET_CONFIGURATION.
 * Returns 0 on success. */
int usbm_enumerate(usbm_host_t *h, uint8_t addr);

/* ------------------------------------------------------------------ */
/* Race injection                                                      */
/* ------------------------------------------------------------------ */
/*
 * Every MMIO access made by src/usb.c passes through the model, so an
 * arbitrary bus transaction can be made to complete at an arbitrary
 * instruction boundary.  usbm_inject_at() fires `fn' immediately BEFORE the
 * nth (0-based) access that matches `addr' (0 = any address) after arming.
 *
 * This is what makes the RB_UEP_R_TOG / RB_UEP_T_TOG race testable: arm on
 * USBM_A_UEP2_CTRL and the callback runs between src/usb.c's read of the
 * register and its store, which is precisely the window a hardware-managed
 * toggle can move in.
 */
/* Positive control for the injection machinery.
 *
 * usbm_selftest_naive_rmw() performs the read-modify-write on R8_UEP2_CTRL the
 * way the shipping application does it and the way src/usb.c did before
 * uep2_ctrl_rmw() existed: ldrb, modify, strb, with nothing in between.  Access
 * #0 to R8_UEP2_CTRL is the read and #1 is the store, so arming an injection at
 * #1 drops a completed transaction squarely into the gap.
 *
 * usbm_selftest_guarded_rmw() calls src/usb.c's own uep2_ctrl_rmw() with the
 * same arguments, so the two can be run against the identical injection and
 * compared.  If the naive one does NOT lose data, the model is not modelling
 * the toggle and every other result in this suite is worth less. */
void usbm_selftest_naive_rmw(uint8_t clear_mask, uint8_t set_bits);
void usbm_selftest_guarded_rmw(uint8_t clear_mask, uint8_t set_bits);

void usbm_inject_at(uintptr_t addr, unsigned nth, void (*fn)(void *), void *arg);
void usbm_inject_cancel(void);
int  usbm_inject_fired(void);
unsigned long usbm_access_count(void);        /* since the last reset */
void usbm_access_count_reset(void);
unsigned long usbm_access_count_at(uintptr_t addr);

#endif /* USB_MODEL_H */
