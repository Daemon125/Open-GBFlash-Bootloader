/* usb_model.c — CH579 USB SIE model + scripted host, wrapped around the real
 * src/usb.c.
 *
 * Owner: cover:usb.  See usb_model.h for what this is and is not.
 *
 * ==========================================================================
 * HOW src/usb.c IS REDIRECTED
 * ==========================================================================
 * include/bl_config.h defines
 *
 *     #define BL_REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
 *
 * and src/usb.c builds every register name out of it.  This file includes
 * bl_config.h FIRST, #undefs the three accessors, redefines them to call into
 * the model, and only then #includes ../src/usb.c — whose own
 * `#include "bl_config.h"` is a no-op because the header guard is already
 * defined.  src/usb.c is compiled BYTE FOR BYTE AS SHIPPED; nothing in it is
 * edited, patched or stubbed.
 *
 * Consequence worth stating plainly: every static object inside src/usb.c —
 * ep0_buf, ep2_buf, rx_head, tx_armed — is in scope for the model code below
 * the #include.  The model uses that ONLY for the endpoint DMA buffers, which
 * the SIE genuinely does write, and for one address comparison.  Assertions in
 * test_usb.c are otherwise black-box: they look at the register file and at the
 * bytes on the wire, not at the DUT's variables.
 *
 * ==========================================================================
 * DETECTING A WRITE WHEN THE ACCESSOR ONLY HANDS OUT A POINTER
 * ==========================================================================
 * BL_REG8 must expand to an lvalue, so the accessor returns
 * `volatile uint8_t *' and a store lands directly in the register file.  There
 * is no callback on the store.  For every register in this device that is
 * plain read/write storage that is exactly right and nothing else is needed.
 *
 * R8_USB_INT_FG is the one register where it is not: bits 0..4 are
 * WRITE-1-TO-CLEAR, so a store must be turned into
 * `latched &= ~written'.  A store is detected by comparing the register file
 * byte against the value the model last published there, which fails in
 * exactly one case — a store of the value that is already present.  And that
 * case is not hypothetical: bl_usb_poll() ends the transfer branch with
 *
 *     R8_USB_INT_FG = RB_UIF_TRANSFER;      // 0x02
 *
 * at a moment when the register reads 0x02.
 *
 * The model therefore always publishes RB_U_SIE_FREE (0x20) set in that byte.
 * That is not a hack bolted on for the test: bit 5 is a REAL read-only status
 * bit in this register and "SIE free" is its true state whenever the model is
 * between transactions, which is whenever src/usb.c is running.  It is never
 * written by any store src/usb.c makes (0x01, 0x02, 0x04, 0x10, 0xFF are the
 * complete set, asserted below), so the published value can never equal a
 * stored one and every store is detected.  src/usb.c masks the flag byte with
 * 0x01/0x02/0x04/0x10 at all four of its test sites and never looks at bit 5.
 *
 * Reconciliation runs at the head of EVERY access, so a store is turned into
 * its side effect before the next read of anything can observe stale state,
 * and at the head of every model entry point.
 *
 * ==========================================================================
 * THE TOGGLE, WHICH IS THE POINT OF THE EXERCISE
 * ==========================================================================
 * RB_UEP_R_TOG (0x80) and RB_UEP_T_TOG (0x40) of R8_UEP2_CTRL are advanced by
 * the SIE, in the same byte software read-modify-writes to change R_RES/T_RES.
 * The model advances them by writing the register file directly, so a stale
 * write-back from src/usb.c really does roll the endpoint's expected PID back
 * one step, and the very next host packet is then reported with RB_UIS_TOG_OK
 * clear and dropped by the TOK_OUT_EP2 case.  A model that tracked the toggle
 * in its own private variable would be blind to the entire defect class this
 * suite exists to cover.
 *
 * Semantics modelled:
 *   OUT, PID matches RB_UEP_R_TOG   -> data to DMA, RB_UIS_TOG_OK set,
 *                                      RB_UEP_R_TOG flips
 *   OUT, PID does not match         -> ACK on the wire anyway (the host counts
 *                                      it as delivered), RB_UIS_TOG_OK CLEAR,
 *                                      RB_UEP_R_TOG does NOT flip
 *   IN accepted by the host         -> RB_UEP_T_TOG flips, RB_UIS_TOG_OK set
 * and RB_UEP_AUTO_TOG only governs whether the SIE does that flipping; it never
 * changes MASK_UEP_R_RES / MASK_UEP_T_RES, which stay exactly as software left
 * them.  EP0 has no AUTO_TOG and its toggles are software's alone.
 *
 * ==========================================================================
 * RB_UC_INT_BUSY
 * ==========================================================================
 * While RB_UIF_TRANSFER is latched and RB_UC_INT_BUSY is set in R8_USB_CTRL the
 * SIE answers every token with a busy NAK and completes nothing.  That is
 * modelled, and it is what makes the injection sweep honest: an injected
 * transaction inside bl_usb_poll()'s transfer branch is correctly a no-op, so
 * the only interleavings the sweep can actually produce are the ones the real
 * part can produce.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* 1. MMIO redirection                                                 */
/* ------------------------------------------------------------------ */

#include "bl_config.h"

#undef BL_REG8
#undef BL_REG16
#undef BL_REG32

static volatile uint8_t  *usbm_p8(uintptr_t a);
static volatile uint16_t *usbm_p16(uintptr_t a);
static volatile uint32_t *usbm_p32(uintptr_t a);

#define BL_REG8(a)   (*usbm_p8((uintptr_t)(a)))
#define BL_REG16(a)  (*usbm_p16((uintptr_t)(a)))
#define BL_REG32(a)  (*usbm_p32((uintptr_t)(a)))

/* Build configuration for the device under test.
 *
 * BL_USB_ECHO 0 is the stage-4 configuration — the one that ships next, and
 * the only one in which bl_usb_rx() returns anything to a consumer.  The
 * stage-3 loopback is reproduced by the test's own service function, which
 * does the same bl_usb_rx() -> queue round trip usb_echo_pump() does.
 *
 * BL_USB_PLL_POWER_ON 0 because usb_pll_power_on() is Thumb inline assembly
 * for the safe-access window and cannot be assembled for the build host.  It
 * touches no USB register and gates nothing; see the coverage note in
 * test_usb.c. */
#define BL_USB_ECHO             0
#define BL_USB_PLL_POWER_ON     0

#include "usb_model.h"

/* clang has no `optimize' function attribute; src/usb.c uses it to keep GCC
 * from emitting a libgcc jump-table helper under -nostdlib, which is a
 * cross-build concern only. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#endif

#include "../src/usb.c"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/* ------------------------------------------------------------------ */
/* 2. Model state                                                      */
/* ------------------------------------------------------------------ */

#define USBM_REGS_SIZE  0x40u

static union {
    uint8_t  b[USBM_REGS_SIZE];
    uint32_t align[USBM_REGS_SIZE / 4u];
} m_usb;

static uint16_t m_pin_ie;
static uint32_t m_icer, m_icpr;
static int      m_iser_touched;

static uint8_t  m_fg;            /* the true latched W1C bits 0..4        */
static uint8_t  m_fg_published;  /* what the model last put in the byte   */
static uint8_t  m_ro_mis, m_ro_st, m_ro_len;

static void   (*m_service)(void);
static unsigned long m_service_calls;
static unsigned long m_service_budget;

#define USBM_ACC_SLOTS  32u
static unsigned long m_acc_total;
static unsigned long m_acc_addr[USBM_ACC_SLOTS];
static uintptr_t     m_acc_addr_key[USBM_ACC_SLOTS];
static unsigned      m_acc_addr_n;

static unsigned long m_n_out_ep2, m_n_out_tog_bad, m_n_in_ep2, m_n_fifo_ov;

static uint8_t  m_force_rx_len;
static int      m_in_model;              /* recursion guard for the hook    */

/* injection */
static struct {
    int        armed;
    uintptr_t  addr;
    unsigned   nth;
    unsigned   seen;
    void     (*fn)(void *);
    void      *arg;
    int        fired;
} m_inj;

/* ------------------------------------------------------------------ */
/* 3. Fatal / watchdog                                                 */
/* ------------------------------------------------------------------ */

void usbm_fatal(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fputs("\nFATAL (usb model): ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    _exit(70);
}

static void usbm_alarm(int sig)
{
    static const char msg[] =
        "\nFATAL (usb model): wall-clock watchdog fired — a loop in src/usb.c "
        "or in the harness did not terminate\n";
    (void)sig;
    (void)!write(2, msg, sizeof msg - 1u);
    _exit(71);
}

void usbm_watchdog(unsigned secs)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = usbm_alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm(secs);
}

/* ------------------------------------------------------------------ */
/* 4. Register-file plumbing                                           */
/* ------------------------------------------------------------------ */

static void usbm_fg_publish(void)
{
    m_fg_published = (uint8_t)((m_fg & USBM_FG_W1C_MASK) | USBM_U_SIE_FREE);
    m_usb.b[USBM_O_INT_FG] = m_fg_published;
}

static void usbm_fg_set(uint8_t bits)
{
    m_fg |= (uint8_t)(bits & USBM_FG_W1C_MASK);
    usbm_fg_publish();
}

static void usbm_ro_publish(void)
{
    m_usb.b[USBM_O_MIS_ST] = m_ro_mis;
    m_usb.b[USBM_O_INT_ST] = m_ro_st;
    m_usb.b[USBM_O_RX_LEN] = m_ro_len;
}

/* Turn any store the DUT made since the last access into its side effect. */
static void usbm_settle(void)
{
    uint8_t v = m_usb.b[USBM_O_INT_FG];

    if (v != m_fg_published) {
        /* A store to R8_USB_INT_FG.  The complete set of values src/usb.c
         * writes is asserted here: if it ever writes something else the model
         * must be re-examined before the result is trusted. */
        if (v != 0x01u && v != 0x02u && v != 0x04u && v != 0x08u &&
            v != 0x10u && v != 0xFFu && v != 0x1Fu) {
            usbm_fatal("unexpected store 0x%02X to R8_USB_INT_FG "
                       "(model's write-detection assumptions need review)", v);
        }
        m_fg &= (uint8_t)~(v & USBM_FG_W1C_MASK);
        usbm_fg_publish();
    }

    if (m_usb.b[USBM_O_MIS_ST] != m_ro_mis) {
        usbm_fatal("store to read-only R8_USB_MIS_ST");
    }
    if (m_usb.b[USBM_O_INT_ST] != m_ro_st) {
        usbm_fatal("store to read-only R8_USB_INT_ST");
    }
    if (m_usb.b[USBM_O_RX_LEN] != m_ro_len) {
        usbm_fatal("store to read-only R8_USB_RX_LEN");
    }
}

static void usbm_note_access(uintptr_t a)
{
    unsigned i;

    m_acc_total++;
    for (i = 0; i < m_acc_addr_n; i++) {
        if (m_acc_addr_key[i] == a) {
            m_acc_addr[i]++;
            break;
        }
    }
    if (i == m_acc_addr_n && m_acc_addr_n < USBM_ACC_SLOTS) {
        m_acc_addr_key[m_acc_addr_n] = a;
        m_acc_addr[m_acc_addr_n] = 1u;
        m_acc_addr_n++;
    }
}

static void usbm_hook_check(uintptr_t a)
{
    void (*fn)(void *);
    void *arg;

    if (!m_inj.armed || m_in_model) {
        return;
    }
    if (m_inj.addr != 0u && m_inj.addr != a) {
        return;
    }
    if (m_inj.seen++ != m_inj.nth) {
        return;
    }
    fn = m_inj.fn;
    arg = m_inj.arg;
    m_inj.armed = 0;
    m_inj.fired = 1;
    fn(arg);
}

static void usbm_access(uintptr_t a)
{
    usbm_settle();
    usbm_note_access(a);
    usbm_hook_check(a);
}

static volatile uint8_t *usbm_p8(uintptr_t a)
{
    usbm_access(a);

    if (a >= 0x40008000u && a < 0x40008000u + USBM_REGS_SIZE) {
        return &m_usb.b[a - 0x40008000u];
    }
    usbm_fatal("8-bit access to unmodelled address 0x%08lX",
               (unsigned long)a);
}

static volatile uint16_t *usbm_p16(uintptr_t a)
{
    usbm_access(a);

    if (a == 0x4000101Au) {                 /* R16_PIN_ANALOG_IE */
        return &m_pin_ie;
    }
    if (a >= 0x40008000u && a + 1u < 0x40008000u + USBM_REGS_SIZE &&
        ((a & 1u) == 0u)) {
        return (volatile uint16_t *)(void *)&m_usb.b[a - 0x40008000u];
    }
    usbm_fatal("16-bit access to unmodelled address 0x%08lX",
               (unsigned long)a);
}

static volatile uint32_t *usbm_p32(uintptr_t a)
{
    usbm_access(a);

    if (a == 0xE000E100u) {
        /* HARD CONSTRAINT.  Vector 22 trampolines into the application's
         * table, which is erased mid-update; an enabled IRQ6 would fetch
         * 0xFFFFFFFF and spin with CodeFlash unlocked.  tools/check_image.py
         * asserts no store to this address exists in the linked binary; this
         * catches it at the C level too. */
        m_iser_touched = 1;
        usbm_fatal("src/usb.c touched NVIC_ISER (0xE000E100) — "
                   "IRQ6 must never be enabled");
    }
    if (a == 0xE000E180u) {
        return &m_icer;
    }
    if (a == 0xE000E280u) {
        return &m_icpr;
    }
    usbm_fatal("32-bit access to unmodelled address 0x%08lX",
               (unsigned long)a);
}

/* ------------------------------------------------------------------ */
/* 5. Lifecycle                                                        */
/* ------------------------------------------------------------------ */

void usbm_power_on(void)
{
    memset(&m_usb, 0, sizeof m_usb);
    m_pin_ie = 0u;
    m_icer = 0u;
    m_icpr = 0u;
    m_iser_touched = 0;

    m_fg = 0u;
    m_ro_mis = 0u;
    m_ro_st = 0u;
    m_ro_len = 0u;
    usbm_fg_publish();
    usbm_ro_publish();

    m_service = bl_usb_poll;
    m_service_calls = 0u;
    m_service_budget = 2000000u;

    m_acc_total = 0u;
    m_acc_addr_n = 0u;
    memset(m_acc_addr, 0, sizeof m_acc_addr);
    memset(m_acc_addr_key, 0, sizeof m_acc_addr_key);

    m_n_out_ep2 = 0u;
    m_n_out_tog_bad = 0u;
    m_n_in_ep2 = 0u;
    m_n_fifo_ov = 0u;

    m_force_rx_len = 0u;
    m_in_model = 0;
    memset(&m_inj, 0, sizeof m_inj);
}

void usbm_set_service(void (*fn)(void))
{
    m_service = (fn != NULL) ? fn : bl_usb_poll;
}

void usbm_set_service_budget(unsigned long n)
{
    m_service_budget = n;
}

unsigned long usbm_service_count(void)
{
    return m_service_calls;
}

void usbm_pump(unsigned n)
{
    while (n-- != 0u) {
        m_service_calls++;
        if (m_service_budget != 0u && m_service_calls > m_service_budget) {
            usbm_fatal("service budget exhausted after %lu calls — livelock",
                       m_service_calls);
        }
        m_service();
    }
}

/* ------------------------------------------------------------------ */
/* 6. Endpoint decode                                                  */
/* ------------------------------------------------------------------ */

static uint8_t ep_ctrl_off(uint8_t ep)
{
    switch (ep) {
    case 0: return USBM_O_UEP0_CTRL;
    case 1: return USBM_O_UEP1_CTRL;
    case 2: return USBM_O_UEP2_CTRL;
    default: usbm_fatal("token to unimplemented endpoint %u", ep);
    }
}

/* Where the SIE DMAs for this endpoint.  The 16-bit UEPn_DMA registers cannot
 * hold a host pointer, so the buffers are reached by symbol; usbm_dma_matches()
 * checks the registers separately. */
static volatile uint8_t *ep_rx_buf(uint8_t ep)
{
    return (ep == 0u) ? ep0_buf : &ep2_buf[0];
}

static volatile uint8_t *ep_tx_buf(uint8_t ep)
{
    if (ep == 0u) return ep0_buf;
    if (ep == 1u) return ep1_buf;
    return &ep2_buf[0x40];
}

static uint8_t ep_t_len(uint8_t ep)
{
    if (ep == 0u) return m_usb.b[USBM_O_UEP0_T_LEN];
    if (ep == 2u) return m_usb.b[USBM_O_UEP2_T_LEN];
    return 0u;                                  /* EP1 is never given a length */
}

static int sie_attached(void)
{
    return (m_usb.b[USBM_O_USB_CTRL] & USBM_UC_DEV_PU_EN) != 0u;
}

static int sie_busy(void)
{
    return ((m_usb.b[USBM_O_USB_CTRL] & USBM_UC_INT_BUSY) != 0u) &&
           ((m_fg & USBM_UIF_TRANSFER) != 0u);
}

static int addr_matches(uint8_t addr)
{
    return (uint8_t)(m_usb.b[USBM_O_DEV_AD] & 0x7Fu) == addr;
}

static void sie_complete(uint8_t token, uint8_t ep, int tog_ok, uint8_t rxlen)
{
    m_ro_st = (uint8_t)(token | ep | (tog_ok ? USBM_UIS_TOG_OK : 0u));
    m_ro_len = rxlen;
    usbm_ro_publish();
    usbm_fg_set(USBM_UIF_TRANSFER);
}

/* ------------------------------------------------------------------ */
/* 7. Raw bus transactions                                             */
/* ------------------------------------------------------------------ */

usbm_res_t usbm_tok_setup(uint8_t addr, const uint8_t d[8])
{
    unsigned i;

    usbm_settle();
    if (!sie_attached() || !addr_matches(addr)) {
        return USBM_R_NONE;
    }
    if (sie_busy()) {
        return USBM_R_NAK;
    }

    /* A SETUP is accepted unconditionally and clears any EP0 stall — required
     * by the USB spec and necessarily what the shipping part does, or the
     * device could never recover from the STALL its own decoder emits. */
    m_in_model = 1;
    for (i = 0; i < 8u; i++) {
        ep0_buf[i] = d[i];
    }
    m_in_model = 0;

    sie_complete(USBM_TOK_SETUP, 0u, 1, 8u);
    return USBM_R_ACK;
}

usbm_res_t usbm_tok_out(uint8_t addr, uint8_t ep,
                        const uint8_t *d, uint32_t n, int pid)
{
    uint8_t  off = ep_ctrl_off(ep);
    uint8_t  ctrl;
    uint8_t  res;
    int      expect;
    int      tog_ok;
    uint32_t i;
    uint32_t window;
    volatile uint8_t *buf;

    usbm_settle();
    if (!sie_attached() || !addr_matches(addr)) {
        return USBM_R_NONE;
    }
    if (sie_busy()) {
        return USBM_R_NAK;
    }

    ctrl = m_usb.b[off];
    res = (uint8_t)(ctrl & USBM_UEP_R_RES_M);
    if (res == USBM_UEP_R_RES_NAK) {
        return USBM_R_NAK;
    }
    if (res == USBM_UEP_R_RES_STL) {
        return USBM_R_STALL;
    }
    if (res == USBM_UEP_R_RES_TOUT) {
        return USBM_R_NONE;
    }

    window = (ep == 0u) ? 64u : USBM_EP2_RX_WINDOW;
    if (n > window) {
        /* The DMA window cannot take the packet.  The part raises
         * RB_UIF_FIFO_OV and no transfer completes. */
        m_n_fifo_ov++;
        usbm_fg_set(USBM_UIF_FIFO_OV);
        return USBM_R_NAK;
    }

    expect = ((ctrl & USBM_UEP_R_TOG) != 0u) ? 1 : 0;
    tog_ok = (pid == expect);

    /* The data is DMAed either way: the SIE ACKs a mis-toggled packet on the
     * wire, so the host counts it as delivered.  Only RB_UIS_TOG_OK tells the
     * firmware it is a retransmission. */
    m_in_model = 1;
    buf = ep_rx_buf(ep);
    for (i = 0; i < n; i++) {
        buf[i] = d[i];
    }
    m_in_model = 0;

    if (ep == 2u) {
        m_n_out_ep2++;
        if (!tog_ok) {
            m_n_out_tog_bad++;
        }
    }

    if (tog_ok && (ep != 0u)) {
        /* AUTO_TOG: the SIE advances the expected receive PID in the very byte
         * software read-modify-writes.  EP0 has no AUTO_TOG. */
        if ((ctrl & USBM_UEP_AUTO_TOG) != 0u) {
            m_usb.b[off] = (uint8_t)(m_usb.b[off] ^ USBM_UEP_R_TOG);
        }
    }

    sie_complete(USBM_TOK_OUT, ep, tog_ok,
                 (m_force_rx_len != 0u) ? m_force_rx_len : (uint8_t)n);
    m_force_rx_len = 0u;
    return USBM_R_ACK;
}

usbm_res_t usbm_tok_in(uint8_t addr, uint8_t ep,
                       uint8_t *d, uint32_t *n, int *pid)
{
    uint8_t  off = ep_ctrl_off(ep);
    uint8_t  ctrl;
    uint8_t  res;
    uint32_t len;
    uint32_t i;
    volatile uint8_t *buf;

    usbm_settle();
    *n = 0u;
    if (!sie_attached() || !addr_matches(addr)) {
        return USBM_R_NONE;
    }
    if (sie_busy()) {
        return USBM_R_NAK;
    }

    ctrl = m_usb.b[off];
    res = (uint8_t)(ctrl & USBM_UEP_T_RES_M);
    if (res == USBM_UEP_T_RES_NAK) {
        return USBM_R_NAK;
    }
    if (res == USBM_UEP_T_RES_STL) {
        return USBM_R_STALL;
    }
    if (res == USBM_UEP_T_RES_TOUT) {
        return USBM_R_NONE;
    }

    len = ep_t_len(ep);
    if (pid != NULL) {
        *pid = ((ctrl & USBM_UEP_T_TOG) != 0u) ? 1 : 0;
    }

    m_in_model = 1;
    buf = ep_tx_buf(ep);
    for (i = 0; i < len; i++) {
        d[i] = buf[i];
    }
    m_in_model = 0;
    *n = len;

    if (ep == 2u) {
        m_n_in_ep2++;
        if ((ctrl & USBM_UEP_AUTO_TOG) != 0u) {
            m_usb.b[off] = (uint8_t)(m_usb.b[off] ^ USBM_UEP_T_TOG);
        }
    }

    sie_complete(USBM_TOK_IN, ep, 1, 0u);
    return USBM_R_ACK;
}

void usbm_bus_reset(void)
{
    usbm_settle();
    /* A bus reset does NOT reset the SIE's configuration and does not clear
     * R8_USB_DEV_AD — the firmware does that, which is exactly why the model
     * keeps answering at the old address until bl_usb_poll() has run. */
    usbm_fg_set(USBM_UIF_BUS_RST);
}

void usbm_suspend(void)
{
    usbm_settle();
    usbm_fg_set(USBM_UIF_SUSPEND);
}

void usbm_force_next_rx_len(uint8_t v)
{
    m_force_rx_len = v;
}

/* ------------------------------------------------------------------ */
/* 8. Observation                                                      */
/* ------------------------------------------------------------------ */

uint8_t usbm_reg8(unsigned off)
{
    usbm_settle();
    if (off >= USBM_REGS_SIZE) usbm_fatal("usbm_reg8 offset %u", off);
    if (off == USBM_O_INT_FG) return m_fg_published;
    return m_usb.b[off];
}

uint16_t usbm_reg16(unsigned off)
{
    usbm_settle();
    if (off + 1u >= USBM_REGS_SIZE) usbm_fatal("usbm_reg16 offset %u", off);
    return (uint16_t)(m_usb.b[off] | ((uint16_t)m_usb.b[off + 1u] << 8));
}

uint32_t usbm_nvic_icer(void) { return m_icer; }
uint32_t usbm_nvic_icpr(void) { return m_icpr; }
int      usbm_iser_touched(void) { return m_iser_touched; }
uint16_t usbm_pin_analog_ie(void) { return m_pin_ie; }

int usbm_ep2_out_open(void)
{
    usbm_settle();
    return (m_usb.b[USBM_O_UEP2_CTRL] & USBM_UEP_R_RES_M) == USBM_UEP_R_RES_ACK;
}

int usbm_ep2_in_armed(void)
{
    usbm_settle();
    return (m_usb.b[USBM_O_UEP2_CTRL] & USBM_UEP_T_RES_M) == USBM_UEP_T_RES_ACK;
}

int usbm_ep2_r_tog(void)
{
    usbm_settle();
    return (m_usb.b[USBM_O_UEP2_CTRL] & USBM_UEP_R_TOG) ? 1 : 0;
}

int usbm_ep2_t_tog(void)
{
    usbm_settle();
    return (m_usb.b[USBM_O_UEP2_CTRL] & USBM_UEP_T_TOG) ? 1 : 0;
}

int usbm_ep0_stalled(void)
{
    usbm_settle();
    return (m_usb.b[USBM_O_UEP0_CTRL] & USBM_UEP_T_RES_M) == USBM_UEP_T_RES_STL;
}

int usbm_dma_matches(void)
{
    return usbm_reg16(USBM_O_UEP0_DMA) == (uint16_t)(uintptr_t)ep0_buf &&
           usbm_reg16(USBM_O_UEP1_DMA) == (uint16_t)(uintptr_t)ep1_buf &&
           usbm_reg16(USBM_O_UEP2_DMA) == (uint16_t)(uintptr_t)ep2_buf;
}

unsigned long usbm_count_out_ep2(void)     { return m_n_out_ep2; }
unsigned long usbm_count_out_tog_bad(void) { return m_n_out_tog_bad; }
unsigned long usbm_count_in_ep2(void)      { return m_n_in_ep2; }
unsigned long usbm_count_fifo_ov(void)     { return m_n_fifo_ov; }

/* ------------------------------------------------------------------ */
/* 9. Injection                                                        */
/* ------------------------------------------------------------------ */

/* The positive control.  This is the shipping application's shape (six sites in
 * the application) and is what src/usb.c would look like without divergence 9:
 * a bare ldrb / modify / strb with the SIE free to move RB_UEP_R_TOG in
 * between.
 *
 * R8_UEP2_CTRL expands to the redirected accessor, so access #0 here is the
 * read and access #1 is the store — usbm_inject_at(USBM_A_UEP2_CTRL, 1, ...)
 * lands a completed transaction exactly in the gap. */
void usbm_selftest_naive_rmw(uint8_t clear_mask, uint8_t set_bits)
{
    uint8_t ctrl = R8_UEP2_CTRL;
    R8_UEP2_CTRL = (uint8_t)((ctrl & (uint8_t)~clear_mask) | set_bits);
}

void usbm_selftest_guarded_rmw(uint8_t clear_mask, uint8_t set_bits)
{
    uep2_ctrl_rmw(clear_mask, set_bits);
}

void usbm_inject_at(uintptr_t addr, unsigned nth, void (*fn)(void *), void *arg)
{
    m_inj.armed = 1;
    m_inj.addr = addr;
    m_inj.nth = nth;
    m_inj.seen = 0u;
    m_inj.fn = fn;
    m_inj.arg = arg;
    m_inj.fired = 0;
}

void usbm_inject_cancel(void)
{
    m_inj.armed = 0;
}

int usbm_inject_fired(void) { return m_inj.fired; }

unsigned long usbm_access_count(void) { return m_acc_total; }

void usbm_access_count_reset(void)
{
    m_acc_total = 0u;
    m_acc_addr_n = 0u;
    memset(m_acc_addr, 0, sizeof m_acc_addr);
    memset(m_acc_addr_key, 0, sizeof m_acc_addr_key);
}

unsigned long usbm_access_count_at(uintptr_t addr)
{
    unsigned i;
    for (i = 0; i < m_acc_addr_n; i++) {
        if (m_acc_addr_key[i] == addr) return m_acc_addr[i];
    }
    return 0u;
}

/* ------------------------------------------------------------------ */
/* 10. Scripted host                                                   */
/* ------------------------------------------------------------------ */

void usbm_host_init(usbm_host_t *h)
{
    memset(h, 0, sizeof *h);
    h->addr = 0u;
    h->out_pid = 0;
    h->in_pid = 0;
    h->retry = 64u;
}

int usbm_h_ctrl_in(usbm_host_t *h, uint8_t bmreq, uint8_t breq,
                   uint16_t wval, uint16_t widx, uint16_t wlen,
                   uint8_t *out, uint32_t *outn)
{
    uint8_t    setup[8];
    uint8_t    pkt[64];
    uint32_t   got = 0u;
    unsigned   tries;
    usbm_res_t r;
    int        pid;
    int        expect = 1;         /* first data-stage IN is DATA1 */

    setup[0] = bmreq;
    setup[1] = breq;
    setup[2] = (uint8_t)(wval & 0xFFu);
    setup[3] = (uint8_t)(wval >> 8);
    setup[4] = (uint8_t)(widx & 0xFFu);
    setup[5] = (uint8_t)(widx >> 8);
    setup[6] = (uint8_t)(wlen & 0xFFu);
    setup[7] = (uint8_t)(wlen >> 8);

    if (usbm_tok_setup(h->addr, setup) != USBM_R_ACK) return -2;
    usbm_pump(1);

    for (;;) {
        uint32_t n = 0u;

        for (tries = 0; tries <= h->retry; tries++) {
            r = usbm_tok_in(h->addr, 0u, pkt, &n, &pid);
            if (r != USBM_R_NAK) break;
            usbm_pump(1);
        }
        if (r == USBM_R_STALL) return -1;
        if (r != USBM_R_ACK) return -2;
        if (pid != expect) return -2;
        expect ^= 1;

        if (n != 0u) {
            if (got + n > wlen + 64u) return -2;
            if (out != NULL) memcpy(out + got, pkt, n);
            got += n;
        }
        usbm_pump(1);
        if (n < USBM_EP0_PKT || got >= wlen) break;
    }

    /* Status stage: a zero-length OUT, DATA1. */
    for (tries = 0; tries <= h->retry; tries++) {
        r = usbm_tok_out(h->addr, 0u, pkt, 0u, 1);
        if (r != USBM_R_NAK) break;
        usbm_pump(1);
    }
    if (r == USBM_R_STALL) return -1;
    if (r != USBM_R_ACK) return -2;
    usbm_pump(1);

    if (outn != NULL) *outn = got;
    return 0;
}

int usbm_h_ctrl_out(usbm_host_t *h, uint8_t bmreq, uint8_t breq,
                    uint16_t wval, uint16_t widx)
{
    uint8_t    setup[8];
    uint8_t    pkt[64];
    uint32_t   n = 0u;
    unsigned   tries;
    usbm_res_t r;
    int        pid = 0;

    setup[0] = bmreq;
    setup[1] = breq;
    setup[2] = (uint8_t)(wval & 0xFFu);
    setup[3] = (uint8_t)(wval >> 8);
    setup[4] = (uint8_t)(widx & 0xFFu);
    setup[5] = (uint8_t)(widx >> 8);
    setup[6] = 0u;
    setup[7] = 0u;

    if (usbm_tok_setup(h->addr, setup) != USBM_R_ACK) return -2;
    usbm_pump(1);

    /* Status stage: a zero-length IN, DATA1. */
    for (tries = 0; tries <= h->retry; tries++) {
        r = usbm_tok_in(h->addr, 0u, pkt, &n, &pid);
        if (r != USBM_R_NAK) break;
        usbm_pump(1);
    }
    if (r == USBM_R_STALL) return -1;
    if (r != USBM_R_ACK) return -2;
    if (n != 0u) return -2;
    if (pid != 1) return -2;
    usbm_pump(1);
    return 0;
}

usbm_res_t usbm_h_out_pkt(usbm_host_t *h, const uint8_t *d, uint32_t n)
{
    usbm_res_t r = usbm_tok_out(h->addr, 2u, d, n, h->out_pid);
    if (r == USBM_R_ACK) {
        /* A real host flips on the wire-level ACK.  It cannot see
         * RB_UIS_TOG_OK, so a device-side toggle rollback desynchronises the
         * link permanently — which is the failure this suite must catch. */
        h->out_pid ^= 1;
    } else if (r == USBM_R_NAK) {
        h->nak_out++;
    }
    return r;
}

usbm_res_t usbm_h_out_pkt_retransmit(usbm_host_t *h, const uint8_t *d,
                                     uint32_t n)
{
    usbm_res_t r = usbm_tok_out(h->addr, 2u, d, n, h->out_pid ^ 1);
    if (r == USBM_R_NAK) h->nak_out++;
    return r;
}

usbm_res_t usbm_h_in_pkt(usbm_host_t *h, uint8_t *d, uint32_t *n)
{
    int        pid = 0;
    usbm_res_t r = usbm_tok_in(h->addr, 2u, d, n, &pid);

    if (r == USBM_R_ACK) {
        if (pid != h->in_pid) {
            /* The host ACKs but discards: this is a retransmission of a packet
             * it already took.  The device still sees an ACK and flips. */
            h->dup_in++;
            *n = 0u;
            return USBM_R_ACK;
        }
        h->in_pid ^= 1;
    } else if (r == USBM_R_NAK) {
        h->nak_in++;
    }
    return r;
}

uint32_t usbm_h_bulk_out(usbm_host_t *h, const uint8_t *d, uint32_t n)
{
    uint32_t sent = 0u;

    while (sent < n) {
        uint32_t chunk = n - sent;
        unsigned tries;
        usbm_res_t r = USBM_R_NAK;

        if (chunk > USBM_EP2_PKT) chunk = USBM_EP2_PKT;

        for (tries = 0; tries <= h->retry; tries++) {
            r = usbm_h_out_pkt(h, d + sent, chunk);
            if (r != USBM_R_NAK) break;
            usbm_pump(1);
        }
        if (r != USBM_R_ACK) break;
        sent += chunk;
        usbm_pump(1);
    }
    return sent;
}

uint32_t usbm_h_bulk_in(usbm_host_t *h, uint8_t *d, uint32_t max)
{
    uint32_t got = 0u;
    unsigned idle = 0u;

    while (got < max) {
        uint8_t    pkt[64];
        uint32_t   n = 0u;
        usbm_res_t r = usbm_h_in_pkt(h, pkt, &n);

        if (r != USBM_R_ACK || n == 0u) {
            usbm_pump(1);
            if (++idle > h->retry) break;
            continue;
        }
        idle = 0u;
        if (n > max - got) n = max - got;
        memcpy(d + got, pkt, n);
        got += n;
        usbm_pump(1);
    }
    return got;
}

int usbm_enumerate(usbm_host_t *h, uint8_t addr)
{
    uint8_t  buf[64];
    uint32_t n = 0u;

    /* macOS/Linux shape: reset, short device-descriptor read, reset again,
     * SET_ADDRESS, then the full walk. */
    usbm_bus_reset();
    usbm_pump(4);

    h->addr = 0u;
    if (usbm_h_ctrl_in(h, 0x80u, 6u, 0x0100u, 0u, 64u, buf, &n) != 0) return -1;
    if (n != 18u) return -1;

    usbm_bus_reset();
    usbm_pump(4);

    h->addr = 0u;
    if (usbm_h_ctrl_out(h, 0x00u, 5u, addr, 0u) != 0) return -1;
    h->addr = addr;

    if (usbm_h_ctrl_in(h, 0x80u, 6u, 0x0100u, 0u, 18u, buf, &n) != 0) return -1;
    if (n != 18u) return -1;

    if (usbm_h_ctrl_in(h, 0x80u, 6u, 0x0200u, 0u, 9u, buf, &n) != 0) return -1;
    if (n != 9u) return -1;

    if (usbm_h_ctrl_in(h, 0x80u, 6u, 0x0200u, 0u, 0x27u, buf, &n) != 0) return -1;
    if (n != 39u) return -1;

    if (usbm_h_ctrl_out(h, 0x00u, 9u, 1u, 0u) != 0) return -1;

    /* Bulk toggles both start at DATA0 after a reset, and the firmware's
     * absolute R8_UEP2_CTRL = 0x12 cleared the device side. */
    h->out_pid = 0;
    h->in_pid = 0;
    return 0;
}
