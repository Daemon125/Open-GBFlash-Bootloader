/* test_usb.c — native test suite for src/usb.c, driven through the CH579 USB
 * SIE model in usb_model.c.
 *
 * Owner: cover:usb.
 *
 * BUILD (until host/Makefile carries it; the Makefile is owned elsewhere):
 *
 *   cc -std=c99 -g -O1 -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
 *      -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter \
 *      -fsanitize=undefined,local-bounds -fno-sanitize-recover=all \
 *      -I../include -I. -DBL_HOST \
 *      -o build/test_usb test_usb.c usb_model.c ../src/usb_desc.c
 *
 * WHY THIS EXISTS.  src/usb.c passed on hardware once, in BL_USB_ECHO mode, as
 * a pyserial round trip.  That run cannot reach — on demand or at all — a bus
 * reset landing mid-transfer, a host that stops reading, a data-toggle
 * mismatch, receive-buffer exhaustion, or a SIE transaction completing between
 * the ldrb and the strb of a read-modify-write on R8_UEP2_CTRL.  Every one of
 * those is a silent-data-loss failure at stage 4 and needs a case opened to
 * recover.  This suite reaches all of them deterministically.
 *
 * WHAT IS ASSERTED, AND AGAINST WHAT.  The descriptor and vendor-table bytes
 * below are typed in again from the descriptor analysis, NOT read from
 * src/usb_desc.c: an assertion sourced from the object under test proves
 * nothing.  Same for the register bit constants, which usb_model.h restates
 * from the register-set analysis.
 *
 * THE DEVICE IS BUILT WITH BL_USB_ECHO=0 — the stage-4 configuration, and the
 * only one in which bl_usb_rx() returns anything to a consumer.  The stage-3
 * loopback is reproduced here by svc_echo(), which performs the same
 * bl_usb_rx() -> queue round trip usb_echo_pump() does, so the packet-boundary
 * sizes that passed on hardware are exercised against the same data path.
 * usb_echo_pump() itself is therefore not compiled; see the coverage notes at
 * the end of main().
 *
 * EVERY LOOP IS BOUNDED.  usbm_watchdog() arms a SIGALRM that _exit()s
 * non-zero, usbm_set_service_budget() caps total bl_usb_poll() calls, and every
 * retry loop here carries an explicit trip count.  A hang is a test failure,
 * not a hung CI job.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "usb.h"
#include "usb_model.h"

/* ------------------------------------------------------------------ */
/* Tiny test framework — same shape as test_proto.c, so the two suites  */
/* report comparable numbers.                                           */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail, g_warn;
static const char *g_group = "";
static int g_sites[512];
static int g_nsites;

static void note_site(int line)
{
    int i;
    for (i = 0; i < g_nsites; i++) if (g_sites[i] == line) return;
    if (g_nsites < (int)(sizeof g_sites / sizeof g_sites[0]))
        g_sites[g_nsites++] = line;
}

static void group(const char *name)
{
    g_group = name;
    printf("\n== %s\n", name);
}

static int check_at(int line, int cond, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int check_at(int line, int cond, const char *fmt, ...)
{
    va_list ap;
    note_site(line);
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL [%s:%d] ", g_group, line);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
    return cond;
}

#define check(cond, ...) check_at(__LINE__, (cond), __VA_ARGS__)

static void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void warn(const char *fmt, ...)
{
    va_list ap;
    g_warn++;
    printf("  NOTE [%s] ", g_group);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* The bytes the host must see.  Transcribed from                       */
/* the descriptor analysis, independently of src/usb_desc.c.            */
/* ------------------------------------------------------------------ */

static const uint8_t k_dev_desc[18] = {
    0x12, 0x01, 0x10, 0x01, 0xFF, 0x00, 0x02, 0x08, 0x86,
    0x1A, 0x23, 0x75, 0x04, 0x03, 0x00, 0x00, 0x00, 0x01,
};

static const uint8_t k_cfg_desc[39] = {
    0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0x80, 0xF0,
    0x09, 0x04, 0x00, 0x00, 0x03, 0xFF, 0x01, 0x02, 0x00,
    0x07, 0x05, 0x82, 0x02, 0x20, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x20, 0x00, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x01,
};

/* The thirteen canned two-byte replies, in call order, saturating on the
 * thirteenth (cmp #0x18 / bge at 0x482A in the application). */
static const uint8_t k_vendor_tbl[26] = {
    0x30, 0x00, 0xC3, 0x00, 0xFF, 0xEC, 0x9F, 0xEC, 0xFF, 0xEC,
    0xDF, 0xEC, 0xDF, 0xEC, 0xDF, 0xEC, 0x9F, 0xEC, 0x9F, 0xEC,
    0x9F, 0xEC, 0x9F, 0xEC, 0xFF, 0xEC,
};

/* ------------------------------------------------------------------ */
/* Service functions — what the update-mode loop would be doing         */
/* ------------------------------------------------------------------ */

#define SINK_CAP    262144u
#define EFIFO_CAP   262144u

static uint8_t  g_sink[SINK_CAP];
static uint32_t g_sinkn;

static uint8_t  g_ef[EFIFO_CAP];
static uint32_t g_ef_head, g_ef_tail;

static void svc_reset(void)
{
    g_sinkn = 0u;
    g_ef_head = 0u;
    g_ef_tail = 0u;
}

/* Poll only.  Nothing consumes the receive staging, so this is the
 * back-pressure case. */
static void svc_poll(void)
{
    bl_usb_poll();
}

/* Poll and consume.  Everything bl_usb_rx() hands back is appended to g_sink,
 * so the suite can assert byte-exact delivery and ordering. */
static void svc_drain(void)
{
    uint8_t tmp[512];
    uint32_t n;

    bl_usb_poll();
    n = bl_usb_rx(tmp, sizeof tmp);
    if (n != 0u) {
        if (g_sinkn + n > SINK_CAP) usbm_fatal("test sink overflow");
        memcpy(&g_sink[g_sinkn], tmp, n);
        g_sinkn += n;
    }
}

/* The stage-3 loopback, in the shape usb_echo_pump() has: at most one packet
 * per service call, handed straight back through the public transmit path. */
static void svc_echo(void)
{
    uint8_t tmp[BL_USB_EP2_PKT];
    uint32_t n;

    bl_usb_poll();

    n = bl_usb_rx(tmp, sizeof tmp);
    if (n != 0u) {
        if (g_ef_head + n > EFIFO_CAP) usbm_fatal("test echo fifo overflow");
        memcpy(&g_ef[g_ef_head], tmp, n);
        g_ef_head += n;
    }
    if (g_ef_head != g_ef_tail) {
        uint32_t want = g_ef_head - g_ef_tail;
        uint32_t k;
        if (want > BL_USB_EP2_PKT) want = BL_USB_EP2_PKT;
        k = bl_usb_tx(&g_ef[g_ef_tail], want);
        g_ef_tail += k;
        if (g_ef_head == g_ef_tail) {
            g_ef_head = 0u;
            g_ef_tail = 0u;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void cold_start(void (*svc)(void))
{
    usbm_power_on();
    usbm_set_service(svc);
    svc_reset();
    bl_usb_init();
}

static void fill_pattern(uint8_t *p, uint32_t n, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = (uint8_t)(seed >> 16);
    }
}

/* Round-trip n bytes through the echo service, interleaving OUT and IN so the
 * 256-byte transmit staging never has to absorb the whole stream.  Bounded:
 * gives up after `budget' unproductive iterations. */
static uint32_t echo_roundtrip(usbm_host_t *h, const uint8_t *out, uint32_t n,
                               uint8_t *in, uint32_t inmax)
{
    uint32_t sent = 0u, got = 0u;
    unsigned idle = 0u;
    const unsigned budget = 4000u;

    while ((sent < n || got < n) && idle < budget) {
        int progress = 0;
        uint8_t pkt[64];
        uint32_t k = 0u;

        if (sent < n) {
            uint32_t chunk = n - sent;
            if (chunk > BL_USB_EP2_PKT) chunk = BL_USB_EP2_PKT;
            if (usbm_h_out_pkt(h, out + sent, chunk) == USBM_R_ACK) {
                sent += chunk;
                progress = 1;
            }
        }
        usbm_pump(1);

        if (usbm_h_in_pkt(h, pkt, &k) == USBM_R_ACK && k != 0u) {
            if (got + k <= inmax) memcpy(in + got, pkt, k);
            got += k;
            progress = 1;
        }
        usbm_pump(1);

        idle = progress ? 0u : idle + 1u;
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* 1. Bring-up                                                         */
/* ------------------------------------------------------------------ */

static void t_init_registers(void)
{
    cold_start(svc_poll);

    /* USB_DeviceInit (0x4B24), write for write. */
    check(usbm_reg8(USBM_O_USB_CTRL) == 0x29u,
          "R8_USB_CTRL = 0x%02X, want 0x29 (DEV_PU_EN|INT_BUSY|DMA_EN)",
          usbm_reg8(USBM_O_USB_CTRL));
    check(usbm_reg8(USBM_O_UDEV_CTRL) == 0x81u,
          "R8_UDEV_CTRL = 0x%02X, want 0x81", usbm_reg8(USBM_O_UDEV_CTRL));
    check(usbm_reg8(USBM_O_INT_EN) == 0x07u,
          "R8_USB_INT_EN = 0x%02X, want 0x07 (byte-identical to the app)",
          usbm_reg8(USBM_O_INT_EN));
    check(usbm_reg8(USBM_O_DEV_AD) == 0x00u, "R8_USB_DEV_AD must start at 0");
    check(usbm_reg8(USBM_O_UEP4_1_MOD) == 0x40u,
          "R8_UEP4_1_MOD = 0x%02X, want 0x40 (EP1 IN only)",
          usbm_reg8(USBM_O_UEP4_1_MOD));
    check(usbm_reg8(USBM_O_UEP2_3_MOD) == 0x0Cu,
          "R8_UEP2_3_MOD = 0x%02X, want 0x0C (EP2 both ways, BUF_MOD clear)",
          usbm_reg8(USBM_O_UEP2_3_MOD));
    check(usbm_reg8(USBM_O_UEP0_CTRL) == 0x02u, "R8_UEP0_CTRL must be 0x02");
    check(usbm_reg8(USBM_O_UEP1_CTRL) == 0x12u, "R8_UEP1_CTRL must be 0x12");
    check(usbm_reg8(USBM_O_UEP2_CTRL) == 0x12u, "R8_UEP2_CTRL must be 0x12");
    check((usbm_pin_analog_ie() & 0x0080u) != 0u,
          "RB_PIN_USB_IE must be set in R16_PIN_ANALOG_IE");
    check(usbm_dma_matches(),
          "R16_UEPn_DMA must hold the low halves of the three endpoint buffers");

    /* The hard constraint.  usbm_p32() makes any access to NVIC_ISER fatal, so
     * reaching here at all is the proof; this states it as a claim. */
    check(usbm_nvic_icer() == 0x40u,
          "NVIC_ICER must have bit 6 (IRQ6 = USB) set, got 0x%08X",
          usbm_nvic_icer());
    check(usbm_nvic_icpr() == 0x40u,
          "NVIC_ICPR must have bit 6 set, got 0x%08X", usbm_nvic_icpr());
    check(!usbm_iser_touched(), "NVIC_ISER must never be written");

    check(usbm_reg8(USBM_O_INT_FG) == USBM_U_SIE_FREE,
          "every W1C flag must be clear after init");
    check(bl_usb_configured() == 0, "not configured before SET_CONFIGURATION");
    check(bl_usb_session() == 0u, "session counter starts at 0");
}

/* ------------------------------------------------------------------ */
/* 2. Enumeration — the exact bytes                                     */
/* ------------------------------------------------------------------ */

static void t_enumeration(void)
{
    usbm_host_t h;
    uint8_t buf[128];
    uint32_t n = 0u;
    int rc;

    cold_start(svc_poll);
    usbm_host_init(&h);

    usbm_bus_reset();
    usbm_pump(4);

    /* The first thing every host asks for, with the 64-byte wLength it uses
     * before it knows bMaxPacketSize0. */
    memset(buf, 0xA5, sizeof buf);
    rc = usbm_h_ctrl_in(&h, 0x80u, 6u, 0x0100u, 0u, 64u, buf, &n);
    check(rc == 0, "GET_DESCRIPTOR(DEVICE, 64) must not stall (rc %d)", rc);
    check(n == 18u, "device descriptor must be 18 bytes, got %u", n);
    check(memcmp(buf, k_dev_desc, 18) == 0,
          "device descriptor bytes differ from the descriptor analysis");
    check(buf[8] == 0x86u && buf[9] == 0x1Au,
          "idVendor must be 1A86");
    check(buf[10] == 0x23u && buf[11] == 0x75u,
          "idProduct must be 7523");
    check(buf[7] == 0x08u,
          "bMaxPacketSize0 must be 8 — the whole EP0 path clamps to it");

    /* A real host resets again here before addressing. */
    usbm_bus_reset();
    usbm_pump(4);

    /* SET_ADDRESS must not take effect until AFTER its own status stage. */
    {
        uint8_t setup[8] = { 0x00u, 5u, 7u, 0u, 0u, 0u, 0u, 0u };
        uint32_t zn = 0u;
        int pid = 0;

        check(usbm_tok_setup(0u, setup) == USBM_R_ACK,
              "SET_ADDRESS SETUP must be accepted at address 0");
        usbm_pump(1);
        check(usbm_tok_in(7u, 0u, buf, &zn, &pid) == USBM_R_NONE,
              "device must NOT answer at the new address before the status stage");
        check(usbm_tok_in(0u, 0u, buf, &zn, &pid) == USBM_R_ACK,
              "status IN must be answered at address 0");
        check(zn == 0u && pid == 1, "status IN must be a DATA1 ZLP");
        usbm_pump(1);
        check(usbm_reg8(USBM_O_DEV_AD) == 7u,
              "R8_USB_DEV_AD must be 7 after the status stage, got %u",
              usbm_reg8(USBM_O_DEV_AD));
        h.addr = 7u;
    }

    rc = usbm_h_ctrl_in(&h, 0x80u, 6u, 0x0100u, 0u, 18u, buf, &n);
    check(rc == 0 && n == 18u && memcmp(buf, k_dev_desc, 18) == 0,
          "device descriptor at the assigned address");

    /* The nine-byte probe, then the full set. */
    memset(buf, 0xA5, sizeof buf);
    rc = usbm_h_ctrl_in(&h, 0x80u, 6u, 0x0200u, 0u, 9u, buf, &n);
    check(rc == 0 && n == 9u, "GET_DESCRIPTOR(CONFIG, 9) must return 9 bytes");
    check(memcmp(buf, k_cfg_desc, 9) == 0, "config header bytes");
    check(buf[2] == 0x27u && buf[3] == 0x00u, "wTotalLength must be 0x0027");

    memset(buf, 0xA5, sizeof buf);
    rc = usbm_h_ctrl_in(&h, 0x80u, 6u, 0x0200u, 0u, 0x27u, buf, &n);
    check(rc == 0 && n == 39u, "GET_DESCRIPTOR(CONFIG, 39) must return 39 bytes");
    check(memcmp(buf, k_cfg_desc, 39) == 0,
          "config descriptor set bytes differ from the descriptor analysis");
    check(buf[20] == 0x82u && buf[27] == 0x02u && buf[34] == 0x81u,
          "endpoint addresses must be 0x82 IN, 0x02 OUT, 0x81 interrupt IN");
    check(buf[22] == 0x20u && buf[29] == 0x20u,
          "bulk wMaxPacketSize must be 32 both ways");

    /* The descriptor INDEX is ignored, as at 0x48CC — a non-zero index must
     * still return the same descriptor rather than stalling. */
    rc = usbm_h_ctrl_in(&h, 0x80u, 6u, 0x0103u, 0u, 18u, buf, &n);
    check(rc == 0 && n == 18u && memcmp(buf, k_dev_desc, 18) == 0,
          "descriptor index is ignored (0x48CC)");

    rc = usbm_h_ctrl_out(&h, 0x00u, 9u, 1u, 0u);
    check(rc == 0, "SET_CONFIGURATION(1) must succeed");
    check(bl_usb_configured() == 1, "bl_usb_configured() after SET_CONFIGURATION");

    rc = usbm_h_ctrl_in(&h, 0x80u, 8u, 0u, 0u, 1u, buf, &n);
    check(rc == 0 && n == 1u && buf[0] == 1u,
          "GET_CONFIGURATION must return 1");

    rc = usbm_h_ctrl_out(&h, 0x00u, 9u, 0u, 0u);
    check(rc == 0 && bl_usb_configured() == 0,
          "SET_CONFIGURATION(0) must un-configure");
}

static void t_vendor_replay(void)
{
    usbm_host_t h;
    uint8_t buf[64];
    uint32_t n = 0u;
    int i;
    int ok = 1;

    cold_start(svc_poll);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 5u) == 0, "enumeration for the vendor replay");

    /* Thirteen distinct pairs, then the thirteenth forever.  bRequest is
     * varied deliberately: the gate is a whole-byte compare on bmRequestType
     * and nothing else (0x4804). */
    for (i = 0; i < 20; i++) {
        static const uint8_t reqs[4] = { 0x5Fu, 0x95u, 0x5Fu, 0x00u };
        int idx = (i < 13) ? (i * 2) : 24;

        memset(buf, 0xA5, sizeof buf);
        if (usbm_h_ctrl_in(&h, 0xC0u, reqs[i & 3], 0u, 0u, 2u, buf, &n) != 0) {
            ok = 0;
            break;
        }
        if (n != 2u || buf[0] != k_vendor_tbl[idx] ||
            buf[1] != k_vendor_tbl[idx + 1]) {
            check(0, "vendor reply %d = %02X %02X, want %02X %02X",
                  i + 1, buf[0], buf[1],
                  k_vendor_tbl[idx], k_vendor_tbl[idx + 1]);
            ok = 0;
            break;
        }
    }
    check(ok, "the canned vendor-IN table must replay in order and saturate");

    /* Deliberate divergence #1: the cursor is rewound on bus reset, so a
     * re-enumeration starts from the chip version again.  The application
     * never does this and answers FF EC for the rest of the power cycle. */
    usbm_bus_reset();
    usbm_pump(4);
    h.addr = 0u;
    memset(buf, 0xA5, sizeof buf);
    check(usbm_h_ctrl_in(&h, 0xC0u, 0x5Fu, 0u, 0u, 2u, buf, &n) == 0 &&
          buf[0] == 0x30u && buf[1] == 0x00u,
          "the replay cursor must rewind on bus reset (got %02X %02X)",
          buf[0], buf[1]);
}

/* A 0xC0 vendor-IN whose wLength is larger than the two bytes the canned table
 * supplies.  ep0_vendor_in() writes only ep0_buf[0..1], and the EP0 window is
 * the same buffer the SETUP packet was DMAed into, so bytes 2..7 of the reply
 * are the SETUP packet's own wValue/wIndex/wLength.  Recorded as a note, not a
 * failure: it is byte-identical to the shipping application (0x4804 clamps
 * nothing), no ch341 driver asks a 0xC0 request for more than 2 bytes, and
 * "reproduce, do not improve" is the standing instruction for this table. */
static void t_vendor_overread_note(void)
{
    usbm_host_t h;
    uint8_t buf[64];
    uint32_t n = 0u;

    cold_start(svc_poll);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 13u) == 0, "enumerate before the over-read probe");

    memset(buf, 0x00, sizeof buf);
    check(usbm_h_ctrl_in(&h, 0xC0u, 0x5Fu, 0x1234u, 0x5678u, 8u, buf, &n) == 0,
          "an over-long 0xC0 request must not stall");
    check(n == 8u && buf[0] == 0x30u && buf[1] == 0x00u,
          "the first two bytes must still be the canned pair");
    if (n == 8u && buf[2] == 0x34u && buf[3] == 0x12u &&
        buf[4] == 0x78u && buf[5] == 0x56u) {
        warn("0xC0 with wLength 8 returns the SETUP packet's own "
             "wValue/wIndex in bytes 2..5 (%02X %02X %02X %02X) — the EP0 DMA "
             "window is shared and only bytes 0..1 are written. Identical to "
             "the application; no ch341 driver asks for more than 2.",
             buf[2], buf[3], buf[4], buf[5]);
    }
}

static void t_stalls(void)
{
    usbm_host_t h;
    uint8_t buf[64];
    uint32_t n = 0u;

    cold_start(svc_poll);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 3u) == 0, "enumeration for the stall matrix");

    check(usbm_h_ctrl_in(&h, 0x80u, 6u, 0x0300u, 0u, 8u, buf, &n) == -1,
          "GET_DESCRIPTOR(STRING) must STALL (0x48F8)");
    check(usbm_h_ctrl_in(&h, 0x80u, 0u, 0u, 0u, 2u, buf, &n) == -1,
          "GET_STATUS must STALL");
    check(usbm_h_ctrl_out(&h, 0x02u, 1u, 0u, 0x82u) == -1,
          "CLEAR_FEATURE(ENDPOINT_HALT) must STALL — reproduced, not improved");
    check(usbm_h_ctrl_out(&h, 0x00u, 3u, 0u, 0u) == -1,
          "SET_FEATURE must STALL");
    check(usbm_h_ctrl_in(&h, 0x81u, 10u, 0u, 0u, 1u, buf, &n) == -1,
          "GET_INTERFACE must STALL");
    check(usbm_h_ctrl_in(&h, 0xC1u, 0x5Fu, 0u, 0u, 2u, buf, &n) == -1,
          "bmRequestType 0xC1 must fall through to the standard decoder"
          " and STALL");
    check(usbm_h_ctrl_out(&h, 0x41u, 0xA1u, 0u, 0u) == -1,
          "bmRequestType 0x41 must STALL");

    /* Recovery: a SETUP always clears the stall, or enumeration could never
     * survive the decoder's own STALL. */
    check(usbm_h_ctrl_in(&h, 0x80u, 6u, 0x0100u, 0u, 18u, buf, &n) == 0 &&
          n == 18u && memcmp(buf, k_dev_desc, 18) == 0,
          "the device must recover from a STALL on the next SETUP");

    /* A SETUP that is not 8 bytes long stalls (0x4A0C / R8_USB_RX_LEN != 8). */
    {
        uint8_t setup[8] = { 0x80u, 6u, 0x00u, 0x01u, 0u, 0u, 18u, 0u };
        uint32_t zn = 0u;
        int pid = 0;
        check(usbm_tok_out(h.addr, 0u, setup, 4u, 1) == USBM_R_ACK,
              "a short packet on EP0 OUT is accepted by the SIE");
        usbm_pump(1);
        (void)usbm_tok_in(h.addr, 0u, buf, &zn, &pid);
        usbm_pump(1);
        check(usbm_h_ctrl_in(&h, 0x80u, 6u, 0x0100u, 0u, 18u, buf, &n) == 0,
              "still alive after a malformed EP0 OUT");
    }

    /* EP1 is declared and never driven: an interrupt IN must NAK forever. */
    {
        uint32_t k = 0u;
        int pid = 0;
        int i;
        int all_nak = 1;
        for (i = 0; i < 20; i++) {
            if (usbm_tok_in(h.addr, 1u, buf, &k, &pid) != USBM_R_NAK) all_nak = 0;
            usbm_pump(1);
        }
        check(all_nak, "EP1 IN must NAK forever — no case for token 0x21");
    }
}

/* ------------------------------------------------------------------ */
/* 3. Bulk echo across the packet boundary                              */
/* ------------------------------------------------------------------ */

static void t_bulk_echo_sizes(void)
{
    static const uint32_t sizes[] = { 1u, 8u, 31u, 32u, 33u, 64u, 256u,
                                      512u, 1024u, 4096u };
    static uint8_t out[4096], in[8192];
    unsigned s;

    for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        usbm_host_t h;
        uint32_t n = sizes[s];
        uint32_t got;

        cold_start(svc_echo);
        usbm_host_init(&h);
        if (!check(usbm_enumerate(&h, 9u) == 0, "enumerate before %u-byte echo",
                   n)) {
            continue;
        }

        fill_pattern(out, n, n * 7919u + 13u);
        memset(in, 0, sizeof in);
        got = echo_roundtrip(&h, out, n, in, sizeof in);

        check(got == n, "%u-byte echo returned %u bytes", n, got);
        check(got == n && memcmp(in, out, n) == 0,
              "%u-byte echo must come back byte-identical", n);
        check(h.dup_in == 0u,
              "%u-byte echo: %lu IN packets arrived with the wrong PID",
              n, h.dup_in);
        check(usbm_count_out_tog_bad() == 0u,
              "%u-byte echo: %lu OUT packets were reported RB_UIS_TOG_OK clear",
              n, usbm_count_out_tog_bad());
        check(usbm_count_fifo_ov() == 0u, "%u-byte echo: FIFO overrun", n);
    }
}

/* ------------------------------------------------------------------ */
/* 4. The R8_UEP2_CTRL toggle race (§3b)                                */
/* ------------------------------------------------------------------ */
/*
 * This is the reason the model advances RB_UEP_R_TOG / RB_UEP_T_TOG inside the
 * very register byte src/usb.c read-modify-writes.  A transaction is made to
 * complete at a chosen instruction boundary; if the store that follows writes
 * back the pre-transaction toggle, the endpoint's expected PID is one step
 * behind, the host's next packet is reported with RB_UIS_TOG_OK clear, the
 * TOK_OUT_EP2 case correctly refuses it — and the byte stream is short.
 *
 * The host flips its own toggle on the wire-level ACK, as a real host must,
 * so a rollback is unrecoverable and shows up as missing bytes.
 */

struct race_state {
    usbm_host_t   *h;
    const uint8_t *stream;
    uint32_t       total;
    uint32_t       sent;
    unsigned       injected;
};

static void race_send_cb(void *p)
{
    struct race_state *s = p;
    uint32_t chunk;

    if (s->sent >= s->total) return;
    chunk = s->total - s->sent;
    if (chunk > BL_USB_EP2_PKT) chunk = BL_USB_EP2_PKT;
    if (usbm_h_out_pkt(s->h, s->stream + s->sent, chunk) == USBM_R_ACK) {
        s->sent += chunk;
        s->injected++;
    }
}

/* Set by race_run() to the number of MMIO accesses that were REACHABLE by the
 * injection, i.e. sampled before the hook is cancelled.  Sizing the sweep off
 * the total for the whole function would include the trailing drain polls,
 * where nothing can fire, and silently under-sweep. */
static unsigned long g_race_span_all, g_race_span_uep2;

/* Stream `total' bytes with one packet injected at access index `nth' of the
 * address `addr' (0 = any).  Returns 1 if everything arrived in order.
 *
 * `echo' selects the workload.  RECEIVE-ONLY is the simple case, but in it the
 * only R8_UEP2_CTRL store that ever happens outside the RB_UIF_TRANSFER window
 * is a NAK->ACK transition, and while the endpoint sits at NAK no OUT can
 * complete underneath it — so receive-only traffic cannot, on its own, produce
 * the interleaving that matters.  The ECHO workload is the stage-4 shape:
 * tx_pump()'s "kick" runs from usb_pumps() with the flag clear AND with EP2 OUT
 * still at R_RES=ACK, which is precisely the second of the two sites the
 * stage-3 review named. */
static int race_run_mode(int echo, uintptr_t addr, unsigned nth, uint32_t total,
                         unsigned *out_injected, uint32_t *out_got)
{
    static uint8_t stream[512];
    static uint8_t back[1024];
    usbm_host_t h;
    struct race_state s;
    unsigned idle = 0u;
    uint32_t got = 0u;

    cold_start(echo ? svc_echo : svc_drain);
    usbm_host_init(&h);
    if (usbm_enumerate(&h, 11u) != 0) return -1;

    fill_pattern(stream, total, nth * 2654435761u + 1u + (uint32_t)echo);
    svc_reset();
    memset(back, 0, sizeof back);

    s.h = &h;
    s.stream = stream;
    s.total = total;
    s.sent = 0u;
    s.injected = 0u;

    usbm_access_count_reset();
    usbm_inject_at(addr, nth, race_send_cb, &s);

    while ((s.sent < total || (echo && got < total)) && idle < 3000u) {
        uint32_t chunk = total - s.sent;
        int progress = 0;

        if (s.sent < total) {
            if (chunk > BL_USB_EP2_PKT) chunk = BL_USB_EP2_PKT;
            if (usbm_h_out_pkt(&h, stream + s.sent, chunk) == USBM_R_ACK) {
                s.sent += chunk;
                progress = 1;
            }
        }
        /* Three polls per packet, not one.  The first services the transfer,
         * during which RB_UC_INT_BUSY holds the SIE off and nothing can be
         * injected; the other two take bl_usb_poll()'s no-transfer path, where
         * usb_pumps() and bl_usb_rx() do their read-modify-writes with the flag
         * clear and the endpoint live.  That is the exposed window, and
         * sweeping only one poll per packet barely reaches it. */
        usbm_pump(3);

        if (echo) {
            uint8_t pkt[64];
            uint32_t k = 0u;
            if (usbm_h_in_pkt(&h, pkt, &k) == USBM_R_ACK && k != 0u) {
                if (got + k <= sizeof back) memcpy(back + got, pkt, k);
                got += k;
                progress = 1;
            }
            usbm_pump(1);
        }
        idle = progress ? 0u : idle + 1u;
    }
    g_race_span_all = usbm_access_count();
    g_race_span_uep2 = usbm_access_count_at(USBM_A_UEP2_CTRL);
    usbm_inject_cancel();
    usbm_pump(16);

    if (out_injected != NULL) *out_injected = s.injected;
    if (out_got != NULL) *out_got = echo ? got : g_sinkn;

    if (s.sent != total) return 0;
    if (echo) {
        if (got != total) return 0;
        return memcmp(back, stream, total) == 0;
    }
    if (g_sinkn != total) return 0;
    return memcmp(g_sink, stream, total) == 0;
}

static int race_run(uintptr_t addr, unsigned nth, uint32_t total,
                    unsigned *out_injected, uint32_t *out_got)
{
    return race_run_mode(0, addr, nth, total, out_injected, out_got);
}

/* ---- the positive control -------------------------------------------- */
/*
 * Before believing that the sweep below proves anything, prove the sweep can
 * FAIL.  The same injection is applied twice: once to a bare ldrb/strb pair —
 * the shape the shipping application uses at all six of its own sites — and
 * once to src/usb.c's guarded uep2_ctrl_rmw().  The first must lose a packet
 * and the second must not.  If both survive, the model is not modelling the
 * hardware-managed toggle and every "no data lost" result here is worthless.
 */

struct ctl_state {
    usbm_host_t   *h;
    const uint8_t *pkt;
    int            acked;
};

static void ctl_send_cb(void *p)
{
    struct ctl_state *s = p;
    s->acked = (usbm_h_out_pkt(s->h, s->pkt, 32u) == USBM_R_ACK);
}

/* Returns the number of bytes that made it through, and reports the endpoint's
 * RB_UEP_R_TOG immediately after the read-modify-write. */
static uint32_t rmw_control_run(int guarded, int *tog_after,
                                unsigned long *tog_bad)
{
    usbm_host_t h;
    static uint8_t a[32], b[32];
    struct ctl_state s;

    cold_start(svc_drain);
    usbm_host_init(&h);
    if (usbm_enumerate(&h, 21u) != 0) return 0xFFFFFFFFu;
    svc_reset();

    fill_pattern(a, sizeof a, 0xAAAAu);
    fill_pattern(b, sizeof b, 0xBBBBu);

    s.h = &h;
    s.pkt = a;
    s.acked = 0;

    /* Access #0 to R8_UEP2_CTRL is the read, #1 is the store. */
    usbm_inject_at(USBM_A_UEP2_CTRL, 1u, ctl_send_cb, &s);
    if (guarded) {
        usbm_selftest_guarded_rmw(0x0Cu, 0x00u);   /* R_RES <- ACK */
    } else {
        usbm_selftest_naive_rmw(0x0Cu, 0x00u);
    }
    usbm_inject_cancel();

    if (!s.acked) return 0xFFFFFFFEu;              /* injection never landed */
    *tog_after = usbm_ep2_r_tog();

    usbm_pump(6);
    (void)usbm_h_out_pkt(&h, b, 32u);              /* the next in-sequence one */
    usbm_pump(6);

    *tog_bad = usbm_count_out_tog_bad();
    return g_sinkn;
}

static void t_rmw_positive_control(void)
{
    int tog_naive = -1, tog_guarded = -1;
    unsigned long bad_naive = 0u, bad_guarded = 0u;
    uint32_t n_naive, n_guarded;

    n_naive = rmw_control_run(0, &tog_naive, &bad_naive);
    n_guarded = rmw_control_run(1, &tog_guarded, &bad_guarded);

    check(n_naive != 0xFFFFFFFEu && n_guarded != 0xFFFFFFFEu,
          "the injected transaction must actually complete in both runs");

    /* The control: an unguarded read-modify-write MUST lose the packet after
     * the one that raced, because the write-back rolled RB_UEP_R_TOG back and
     * the host — which cannot see RB_UIS_TOG_OK — has already advanced. */
    check(tog_naive == 0,
          "unguarded RMW must write back the stale RB_UEP_R_TOG (got %d)",
          tog_naive);
    check(bad_naive >= 1u,
          "unguarded RMW must produce a RB_UIS_TOG_OK-clear packet (got %lu)",
          bad_naive);
    check(n_naive == 32u,
          "unguarded RMW must lose the following packet: 32 of 64 bytes "
          "expected, got %u", n_naive);

    /* And src/usb.c's guarded helper must not. */
    check(tog_guarded == 1,
          "uep2_ctrl_rmw() must repair RB_UEP_R_TOG to the post-transaction "
          "value (got %d)", tog_guarded);
    check(bad_guarded == 0u,
          "uep2_ctrl_rmw() must produce no mis-toggled packets (got %lu)",
          bad_guarded);
    check(n_guarded == 64u,
          "uep2_ctrl_rmw() must deliver both packets: 64 bytes expected, got %u",
          n_guarded);
}

/* The same control on the TRANSMIT side, which reaches uep2_ctrl_rmw()'s
 * `fix = RB_UEP_T_TOG' repair arm — nothing else in this suite does so
 * deterministically.  Here the injected transaction is a bulk IN: the host
 * takes the armed packet, the SIE advances RB_UEP_T_TOG, and a stale
 * write-back sends the NEXT packet out with the PID the host has already
 * consumed, so the host discards it as a retransmission and 32 bytes vanish. */

struct ctl_in_state {
    usbm_host_t *h;
    uint8_t      buf[64];
    uint32_t     n;
    int          acked;
};

static void ctl_in_cb(void *p)
{
    struct ctl_in_state *s = p;
    s->acked = (usbm_h_in_pkt(s->h, s->buf, &s->n) == USBM_R_ACK);
}

static uint32_t rmw_tx_control_run(int guarded, int *tog_after,
                                   unsigned long *dupes, uint8_t *back)
{
    usbm_host_t h;
    static uint8_t src[64];
    struct ctl_in_state s;
    uint32_t got;

    cold_start(svc_poll);
    usbm_host_init(&h);
    if (usbm_enumerate(&h, 22u) != 0) return 0xFFFFFFFFu;

    fill_pattern(src, sizeof src, 0xC0DEu);
    if (bl_usb_tx(src, sizeof src) != sizeof src) return 0xFFFFFFFDu;
    usbm_pump(1);
    if (!usbm_ep2_in_armed()) return 0xFFFFFFFDu;

    s.h = &h;
    s.n = 0u;
    s.acked = 0;

    usbm_inject_at(USBM_A_UEP2_CTRL, 1u, ctl_in_cb, &s);
    if (guarded) {
        usbm_selftest_guarded_rmw(0x03u, 0x00u);   /* T_RES <- ACK */
    } else {
        usbm_selftest_naive_rmw(0x03u, 0x00u);
    }
    usbm_inject_cancel();

    if (!s.acked || s.n != 32u) return 0xFFFFFFFEu;
    *tog_after = usbm_ep2_t_tog();

    memcpy(back, s.buf, 32);
    got = 32u + usbm_h_bulk_in(&h, back + 32, 32u);
    *dupes = h.dup_in;
    return got;
}

static void t_rmw_positive_control_tx(void)
{
    static uint8_t back_n[128], back_g[128], expect[64];
    int tog_n = -1, tog_g = -1;
    unsigned long dup_n = 0u, dup_g = 0u;
    uint32_t got_n, got_g;

    memset(back_n, 0, sizeof back_n);
    memset(back_g, 0, sizeof back_g);
    got_n = rmw_tx_control_run(0, &tog_n, &dup_n, back_n);
    got_g = rmw_tx_control_run(1, &tog_g, &dup_g, back_g);
    fill_pattern(expect, sizeof expect, 0xC0DEu);

    check(got_n < 0xFFFFFFF0u && got_g < 0xFFFFFFF0u,
          "both transmit-side control runs must set up (%u / %u)", got_n, got_g);

    check(tog_n == 0,
          "unguarded RMW must write back the stale RB_UEP_T_TOG (got %d)",
          tog_n);
    check(dup_n >= 1u,
          "the next IN must then go out with a PID the host has already "
          "consumed (%lu discarded)", dup_n);
    check(got_n == 32u,
          "unguarded RMW must lose the second transmit packet: 32 bytes "
          "expected, got %u", got_n);

    check(tog_g == 1,
          "uep2_ctrl_rmw() must repair RB_UEP_T_TOG (got %d)", tog_g);
    check(dup_g == 0u,
          "uep2_ctrl_rmw() must leave no duplicated IN packets (%lu)", dup_g);
    check(got_g == 64u && memcmp(back_g, expect, 64) == 0,
          "uep2_ctrl_rmw() must deliver both transmit packets intact (got %u)",
          got_g);
}

static void t_toggle_race(void)
{
    unsigned k;
    unsigned long span, uspan;
    unsigned bad = 0u, fired = 0u, productive = 0u;

    /* A baseline pass with the injection out of reach, to measure how many
     * register accesses one streaming run makes. */
    (void)race_run(0u, 100000u, 256u, NULL, NULL);
    span = g_race_span_all;
    uspan = g_race_span_uep2;
    check(span > 100u, "the streaming phase must make enough MMIO accesses to "
                       "sweep (%lu)", span);
    check(uspan > 16u, "the streaming phase must touch R8_UEP2_CTRL enough "
                       "times to sweep (%lu)", uspan);
    if (span > 700u) span = 700u;
    if (uspan > 200u) uspan = 200u;

    /* Sweep 1: targeted.  Every access to R8_UEP2_CTRL, which is where the
     * read-modify-write pairs are.  This is the sweep that would catch an
     * unguarded ldrb/strb. */
    for (k = 0; k < (unsigned)uspan; k++) {
        unsigned inj = 0u;
        uint32_t got = 0u;
        int ok = race_run(USBM_A_UEP2_CTRL, k, 256u, &inj, &got);
        if (usbm_inject_fired()) fired++;
        if (inj != 0u) productive++;
        if (ok != 1) {
            bad++;
            if (bad == 1u) {
                check(0, "toggle race at R8_UEP2_CTRL access #%u: %u/256 bytes "
                         "delivered (injected %u), TOG_OK-clear packets %lu",
                      k, got, inj, usbm_count_out_tog_bad());
            }
        }
    }
    check(bad == 0u,
          "%u of %lu injection points at R8_UEP2_CTRL lost or duplicated data",
          bad, uspan);
    check(fired == (unsigned)uspan,
          "only %u of %lu targeted injections fired", fired, uspan);
    check(productive >= (unsigned)uspan / 4u,
          "only %u of %lu targeted injections actually delivered a packet — "
          "the sweep is not reaching a live endpoint", productive, uspan);

    /* Sweep 2: broad.  Any MMIO access at all, so the completion can land
     * between any two instructions the compiler emitted, not only inside
     * uep2_ctrl_rmw(). */
    bad = 0u;
    fired = 0u;
    productive = 0u;
    for (k = 0; k < (unsigned)span; k++) {
        unsigned inj = 0u;
        uint32_t got = 0u;
        int ok = race_run(0u, k, 256u, &inj, &got);
        if (usbm_inject_fired()) fired++;
        if (inj != 0u) productive++;
        if (ok != 1) {
            bad++;
            if (bad == 1u) {
                check(0, "transaction completing at MMIO access #%u lost data: "
                         "%u/256 bytes (injected %u)", k, got, inj);
            }
        }
    }
    check(bad == 0u, "%u of %lu whole-poll injection points lost data",
          bad, span);
    check(fired == (unsigned)span, "only %u of %lu broad injections fired",
          fired, span);
    check(productive >= (unsigned)span / 4u,
          "only %u of %lu broad injections delivered a packet", productive,
          span);
    printf("  (receive-only: swept %lu targeted + %lu broad injection points; "
           "%u of the broad ones delivered a packet mid-instruction)\n",
           uspan, span, productive);

    /* Sweep 3: the same, but with the echo workload, so tx_pump()'s kick runs
     * outside the transfer window with EP2 OUT still open — the interleaving
     * that receive-only traffic structurally cannot produce.  This is the sweep
     * that fails if uep2_ctrl_rmw() loses its guard. */
    (void)race_run_mode(1, 0u, 100000u, 256u, NULL, NULL);
    span = g_race_span_all;
    uspan = g_race_span_uep2;
    check(span > 100u && uspan > 16u,
          "the echo workload must make enough MMIO accesses to sweep "
          "(%lu / %lu)", span, uspan);
    if (span > 900u) span = 900u;
    if (uspan > 260u) uspan = 260u;

    bad = 0u;
    productive = 0u;
    for (k = 0; k < (unsigned)uspan; k++) {
        unsigned inj = 0u;
        uint32_t got = 0u;
        int ok = race_run_mode(1, USBM_A_UEP2_CTRL, k, 256u, &inj, &got);
        if (inj != 0u) productive++;
        if (ok != 1) {
            bad++;
            if (bad == 1u) {
                check(0, "echo workload, R8_UEP2_CTRL access #%u: %u/256 bytes "
                         "returned (injected %u), TOG_OK-clear %lu, "
                         "duplicated IN %u",
                      k, got, inj, usbm_count_out_tog_bad(), 0u);
            }
        }
    }
    check(bad == 0u,
          "%u of %lu R8_UEP2_CTRL injection points under the echo workload "
          "lost or duplicated data", bad, uspan);

    {
        unsigned bad_broad = 0u;
        for (k = 0; k < (unsigned)span; k++) {
            uint32_t got = 0u;
            if (race_run_mode(1, 0u, k, 256u, NULL, &got) != 1) {
                bad_broad++;
                if (bad_broad == 1u) {
                    check(0, "echo workload, MMIO access #%u: %u/256 bytes "
                             "returned", k, got);
                }
            }
        }
        check(bad_broad == 0u,
              "%u of %lu whole-poll injection points under the echo workload "
              "lost data", bad_broad, span);
    }
    printf("  (echo: swept %lu targeted + %lu broad injection points; %u of "
           "the targeted ones delivered a packet mid-instruction)\n",
           uspan, span, productive);
}

/* ------------------------------------------------------------------ */
/* 5. Toggle mismatch — the host retransmits                            */
/* ------------------------------------------------------------------ */

static void t_toggle_mismatch(void)
{
    usbm_host_t h;
    uint8_t a[32], b[32];
    unsigned long bad_before;

    cold_start(svc_drain);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 4u) == 0, "enumerate for the retransmit case");
    svc_reset();

    fill_pattern(a, sizeof a, 0x1111u);
    fill_pattern(b, sizeof b, 0x2222u);

    check(usbm_ep2_r_tog() == 0, "EP2 expects DATA0 after a bus reset");
    check(usbm_h_out_pkt(&h, a, sizeof a) == USBM_R_ACK, "first OUT accepted");
    usbm_pump(2);
    check(usbm_ep2_r_tog() == 1, "RB_UEP_R_TOG must have advanced");

    /* The host did not see the ACK and sends the same packet again with the
     * same PID.  The SIE ACKs it on the wire; the firmware must not take it. */
    bad_before = usbm_count_out_tog_bad();
    check(usbm_h_out_pkt_retransmit(&h, a, sizeof a) == USBM_R_ACK,
          "a mis-toggled OUT is still ACKed on the wire");
    usbm_pump(2);
    check(usbm_count_out_tog_bad() == bad_before + 1u,
          "the retransmission must be reported with RB_UIS_TOG_OK clear");
    check(usbm_ep2_r_tog() == 1,
          "a mis-toggled packet must NOT advance RB_UEP_R_TOG");

    /* And the next in-sequence packet must still be taken. */
    check(usbm_h_out_pkt(&h, b, sizeof b) == USBM_R_ACK, "next OUT accepted");
    usbm_pump(4);

    check(g_sinkn == 64u, "expected 64 delivered bytes, got %u", g_sinkn);
    check(g_sinkn == 64u && memcmp(g_sink, a, 32) == 0 &&
          memcmp(g_sink + 32, b, 32) == 0,
          "a retransmission must be dropped, not duplicated, and must not cost "
          "the packet after it");

    /* The endpoint must be back open, not left NAKed by the unconditional NAK
     * in the TOK_OUT_EP2 case. */
    check(usbm_ep2_out_open(),
          "EP2 OUT must be re-opened after a mis-toggled packet");
}

/* ------------------------------------------------------------------ */
/* 6. Bus reset landing mid-transfer                                    */
/* ------------------------------------------------------------------ */

static void t_reset_mid_out(void)
{
    usbm_host_t h;
    static uint8_t stream[256], back[512];
    uint32_t sess;
    int i;

    cold_start(svc_drain);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 6u) == 0, "enumerate before the mid-OUT reset");
    svc_reset();

    fill_pattern(stream, sizeof stream, 0xBEEFu);

    /* Three packets in, with nothing consumed yet. */
    for (i = 0; i < 3; i++) {
        check(usbm_h_out_pkt(&h, stream + i * 32, 32u) == USBM_R_ACK,
              "packet %d before the reset", i);
        bl_usb_poll();       /* service the transfer, but do NOT drain */
    }
    check(usbm_ep2_r_tog() == 1, "three packets leave R_TOG at 1");

    sess = bl_usb_session();
    usbm_bus_reset();
    usbm_pump(4);

    check(bl_usb_session() == sess + 1u,
          "a bus reset must count as a new session");
    check(usbm_reg8(USBM_O_DEV_AD) == 0u, "address must return to 0");
    check(usbm_reg8(USBM_O_UEP2_CTRL) == 0x12u,
          "R8_UEP2_CTRL must be the absolute 0x12 — both toggles cleared, IN "
          "cancelled, OUT re-armed (got 0x%02X)",
          usbm_reg8(USBM_O_UEP2_CTRL));
    check(usbm_ep2_r_tog() == 0 && usbm_ep2_t_tog() == 0,
          "both EP2 toggles must be cleared by a bus reset");
    check(!usbm_ep2_in_armed(), "the IN direction must be disarmed");
    check(bl_usb_configured() == 0, "a bus reset un-configures the device");

    /* The staging is thrown away; that is the contract, and it is why
     * bl_usb_session() exists. */
    {
        uint8_t tmp[64];
        check(bl_usb_rx(tmp, sizeof tmp) == 0u,
              "the receive staging must be empty after a bus reset");
    }

    /* Recovery: re-enumerate and round-trip a fresh stream. */
    usbm_set_service(svc_echo);
    svc_reset();
    check(usbm_enumerate(&h, 6u) == 0, "re-enumeration after the reset");
    check(echo_roundtrip(&h, stream, 128u, back, sizeof back) == 128u &&
          memcmp(back, stream, 128) == 0,
          "the link must work normally after a mid-transfer bus reset");
}

static void t_reset_mid_in(void)
{
    usbm_host_t h;
    static uint8_t stream[256], back[512];
    uint8_t pkt[64];
    uint32_t n = 0u;
    uint32_t sess;

    cold_start(svc_echo);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 8u) == 0, "enumerate before the mid-IN reset");
    svc_reset();

    fill_pattern(stream, sizeof stream, 0xCAFEu);

    /* Push enough in to keep the transmit side busy, take exactly one packet
     * back, then reset with an IN packet still armed. */
    (void)usbm_h_bulk_out(&h, stream, 128u);
    usbm_pump(8);
    check(usbm_h_in_pkt(&h, pkt, &n) == USBM_R_ACK && n == 32u,
          "one IN packet before the reset (got %u bytes)", n);
    usbm_pump(1);
    check(usbm_ep2_in_armed(), "the transmit side must be armed mid-stream");

    sess = bl_usb_session();
    usbm_bus_reset();
    usbm_pump(4);

    check(bl_usb_session() == sess + 1u, "session counter after a mid-IN reset");
    check(!usbm_ep2_in_armed(),
          "the armed IN packet must be withdrawn by the bus reset");
    check(usbm_reg8(USBM_O_UEP2_T_LEN) == 0u,
          "R8_UEP2_T_LEN must be zeroed by the bus reset");

    svc_reset();
    check(usbm_enumerate(&h, 8u) == 0, "re-enumeration after the mid-IN reset");
    check(echo_roundtrip(&h, stream, 96u, back, sizeof back) == 96u &&
          memcmp(back, stream, 96) == 0,
          "the link must work normally after a mid-IN bus reset");
}

static void t_two_resets_in_a_row(void)
{
    usbm_host_t h;
    uint8_t buf[64];
    uint32_t n = 0u;

    /* Acceptance test: a normal host enumeration issues more
     * than one bus reset and the device must survive all of them. */
    cold_start(svc_poll);
    usbm_host_init(&h);

    usbm_bus_reset();
    usbm_bus_reset();
    usbm_bus_reset();
    usbm_pump(8);
    check(usbm_h_ctrl_in(&h, 0x80u, 6u, 0x0100u, 0u, 18u, buf, &n) == 0 &&
          n == 18u && memcmp(buf, k_dev_desc, 18) == 0,
          "three back-to-back bus resets must leave the device enumerable");

    /* A reset arriving together with a pending transfer: the transfer wins and
     * the reset is picked up on the next call.  Nothing may be lost. */
    {
        uint8_t setup[8] = { 0x80u, 6u, 0x00u, 0x01u, 0u, 0u, 18u, 0u };
        check(usbm_tok_setup(0u, setup) == USBM_R_ACK, "SETUP before the reset");
        usbm_bus_reset();
        usbm_pump(1);                       /* services the transfer */
        check((usbm_reg8(USBM_O_INT_FG) & USBM_UIF_BUS_RST) != 0u,
              "the bus-reset flag latches while a transfer is serviced first");
        usbm_pump(1);                       /* services the reset */
        check((usbm_reg8(USBM_O_INT_FG) & USBM_UIF_BUS_RST) == 0u,
              "the bus-reset flag must be cleared on the next poll");
        check(usbm_reg8(USBM_O_DEV_AD) == 0u, "and the reset must have applied");
    }
}

/* ------------------------------------------------------------------ */
/* 7. Session boundary without a bus reset (tty close/reopen)           */
/* ------------------------------------------------------------------ */

static void t_session_flush(void)
{
    usbm_host_t h;
    uint8_t a[32];
    uint8_t tmp[64];
    uint32_t sess;

    cold_start(svc_poll);           /* nothing consumes: bytes sit in staging */
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 12u) == 0, "enumerate before the session flush");

    fill_pattern(a, sizeof a, 0xF00Du);
    check(usbm_h_out_pkt(&h, a, sizeof a) == USBM_R_ACK, "one OUT before close");
    usbm_pump(2);
    check(usbm_ep2_r_tog() == 1, "R_TOG advanced by that packet");

    /* Also leave a packet armed on the transmit side. */
    check(bl_usb_tx(a, 16u) == 16u, "queue 16 bytes for the old session");
    usbm_pump(1);
    check(usbm_ep2_in_armed(), "an IN packet is armed for the old session");

    sess = bl_usb_session();

    /* The proxy for "a tty was opened": bmRequestType 0x40, bRequest 0xA1
     * CH341_REQ_SERIAL_INIT.  Every ch34x driver sends it before its first
     * bulk URB. */
    check(usbm_h_ctrl_out(&h, 0x40u, 0xA1u, 0x1312u, 0xB282u) == 0,
          "the 0x40/0xA1 vendor OUT must be answered with a ZLP, never a STALL");

    check(bl_usb_session() == sess + 1u,
          "CH341_REQ_SERIAL_INIT must count as a new session");
    check(bl_usb_rx(tmp, sizeof tmp) == 0u,
          "the previous session's received bytes must be discarded");
    check(!usbm_ep2_in_armed(),
          "the armed IN packet must be withdrawn — otherwise the new session's "
          "first bulk IN is answered with the old session's bytes");
    check(usbm_reg8(USBM_O_UEP2_T_LEN) == 0u, "and R8_UEP2_T_LEN zeroed");

    /* And the thing a bus reset does that a reopen must NOT: the toggles have
     * to survive, because the host did not restart them (§6b). */
    check(usbm_ep2_r_tog() == 1,
          "RB_UEP_R_TOG must SURVIVE a port reopen — clearing it would make the "
          "new session's first packet be discarded by one side or the other");
    check(usbm_ep2_out_open(), "EP2 OUT must be open again after the flush");

    /* The new session then works with the host's toggle where it left off. */
    {
        static uint8_t s2[64], b2[128];
        fill_pattern(s2, sizeof s2, 0x5A5Au);
        usbm_set_service(svc_echo);
        svc_reset();
        check(echo_roundtrip(&h, s2, 64u, b2, sizeof b2) == 64u &&
              memcmp(b2, s2, 64) == 0,
              "the new session must round-trip without resynchronising toggles");
    }

    /* Other 0x40 requests must NOT flush — only 0xA1 is the marker. */
    {
        uint32_t s3;
        usbm_set_service(svc_poll);
        s3 = bl_usb_session();
        check(usbm_h_ctrl_out(&h, 0x40u, 0x9Au, 0u, 0u) == 0,
              "0x40/0x9A must be accepted");
        check(usbm_h_ctrl_out(&h, 0x40u, 0xA4u, 0x00FFu, 0u) == 0,
              "0x40/0xA4 must be accepted");
        check(bl_usb_session() == s3,
              "only CH341_REQ_SERIAL_INIT is a session marker");
    }
}

/* ------------------------------------------------------------------ */
/* 8. Back pressure                                                     */
/* ------------------------------------------------------------------ */

static void t_buffer_pressure(void)
{
    usbm_host_t h;
    static uint8_t stream[1024];
    uint8_t pkt[32];
    unsigned accepted = 0u;
    unsigned naks = 0u;
    unsigned i;
    unsigned long out_before;

    cold_start(svc_poll);           /* the loop never drains */
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 14u) == 0, "enumerate before the pressure test");

    fill_pattern(stream, sizeof stream, 0x0BADu);

    /* Credit is granted only while a whole 64-byte hardware window fits in the
     * 512-byte staging, i.e. while rx_head <= 448.  Fourteen packets leave it
     * at exactly 448 (still open); the fifteenth closes it. */
    for (i = 0; i < 32u; i++) {
        usbm_res_t r = usbm_h_out_pkt(&h, stream + i * 32u, 32u);
        if (r == USBM_R_ACK) {
            accepted++;
        } else if (r == USBM_R_NAK) {
            naks++;
        } else {
            check(0, "unexpected response %d under back pressure", (int)r);
            break;
        }
        usbm_pump(2);
    }

    check(accepted == 15u,
          "exactly 15 packets (480 B) fit before credit closes, got %u",
          accepted);
    check(naks == 32u - 15u,
          "every packet after the fifteenth must be NAKed, got %u NAKs", naks);
    check(!usbm_ep2_out_open(),
          "EP2 OUT must be sitting at R_RES=NAK, not silently dropping");

    /* Hammering a closed endpoint must change nothing at all. */
    out_before = usbm_count_out_ep2();
    for (i = 0; i < 50u; i++) {
        check_at(__LINE__, usbm_h_out_pkt(&h, pkt, 32u) == USBM_R_NAK,
                 "a closed endpoint must keep NAKing");
        usbm_pump(1);
    }
    check(usbm_count_out_ep2() == out_before,
          "no packet may be accepted while the endpoint is NAKed");

    /* Now drain, and every byte must be there, in order. */
    usbm_set_service(svc_drain);
    svc_reset();
    usbm_pump(4);
    check(g_sinkn == 480u, "draining must yield 480 bytes, got %u", g_sinkn);
    check(g_sinkn == 480u && memcmp(g_sink, stream, 480) == 0,
          "back pressure must not corrupt or reorder the staged bytes");
    check(usbm_ep2_out_open(), "EP2 OUT must re-open once the staging drains");

    /* And the stream continues from where it stopped. */
    check(usbm_h_out_pkt(&h, stream + 480u, 32u) == USBM_R_ACK,
          "the stream resumes after the drain");
    usbm_pump(4);
    check(g_sinkn == 512u && memcmp(g_sink, stream, 512) == 0,
          "…with no gap and no duplication");
}

static void t_oversize_and_fifo_ov(void)
{
    usbm_host_t h;
    uint8_t big[96];
    uint8_t small[32];

    cold_start(svc_drain);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 15u) == 0, "enumerate before the oversize test");
    svc_reset();

    fill_pattern(big, sizeof big, 0x9999u);

    /* Larger than the 64-byte EP2 RX window: the SIE raises RB_UIF_FIFO_OV and
     * completes nothing.  bl_usb_poll() must clear the flag and carry on. */
    check(usbm_tok_out(h.addr, 2u, big, 96u, h.out_pid) == USBM_R_NAK,
          "an over-long OUT must not complete a transfer");
    check(usbm_count_fifo_ov() == 1u, "RB_UIF_FIFO_OV must be raised");
    check((usbm_reg8(USBM_O_INT_FG) & USBM_UIF_FIFO_OV) != 0u,
          "the FIFO_OV flag must latch");
    usbm_pump(2);
    check((usbm_reg8(USBM_O_INT_FG) & USBM_UIF_FIFO_OV) == 0u,
          "bl_usb_poll() must clear RB_UIF_FIFO_OV");
    check(g_sinkn == 0u, "nothing may be delivered from an over-long packet");

    /* A misbehaving SIE reporting more than the window holds: rx_deliver()'s
     * clamp is defence in depth and must hold. */
    fill_pattern(small, sizeof small, 0x4242u);
    usbm_force_next_rx_len(200u);
    check(usbm_h_out_pkt(&h, small, 32u) == USBM_R_ACK,
          "a normal packet with a lying R8_USB_RX_LEN");
    usbm_pump(4);
    check(g_sinkn == 64u,
          "rx_deliver() must clamp a 200-byte report to the 64-byte RX window, "
          "delivered %u", g_sinkn);
    check(g_sinkn >= 32u && memcmp(g_sink, small, 32) == 0,
          "the real 32 bytes must still be first");

    /* Still alive and in sequence afterwards. */
    check(usbm_h_out_pkt(&h, small, 32u) == USBM_R_ACK,
          "the endpoint survives the clamp");
    usbm_pump(4);
}

/* ------------------------------------------------------------------ */
/* 9. The host stops moving                                            */
/* ------------------------------------------------------------------ */

static void t_host_stops_and_resumes(void)
{
    usbm_host_t h;
    static uint8_t stream[256], back[512];
    uint32_t got;
    unsigned i;

    cold_start(svc_echo);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 16u) == 0, "enumerate before the idle test");
    svc_reset();

    fill_pattern(stream, sizeof stream, 0x7777u);

    /* Half the stream in, then the host goes away: no tokens at all while the
     * device keeps polling.  Nothing may be lost, duplicated or re-armed. */
    {
        uint32_t sess_before;
        uint8_t  ctrl_before;
        (void)usbm_h_bulk_out(&h, stream, 128u);
        usbm_pump(8);
        sess_before = bl_usb_session();
        ctrl_before = usbm_reg8(USBM_O_UEP2_CTRL);

        usbm_pump(2000);

        check(usbm_count_out_tog_bad() == 0u,
              "no spurious toggle errors while the host is away");
        check(bl_usb_session() == sess_before,
              "2000 idle polls must not invent a session boundary");
        check(usbm_reg8(USBM_O_UEP2_CTRL) == ctrl_before,
              "2000 idle polls must leave R8_UEP2_CTRL alone "
              "(0x%02X -> 0x%02X)", ctrl_before,
              usbm_reg8(USBM_O_UEP2_CTRL));
    }

    /* Resume: pull everything back, then send the rest. */
    got = 0u;
    for (i = 0; i < 400u && got < 128u; i++) {
        uint8_t pkt[64];
        uint32_t k = 0u;
        if (usbm_h_in_pkt(&h, pkt, &k) == USBM_R_ACK && k != 0u) {
            memcpy(back + got, pkt, k);
            got += k;
        }
        usbm_pump(1);
    }
    check(got == 128u, "the first half must survive the idle period (%u)", got);
    check(got == 128u && memcmp(back, stream, 128) == 0,
          "…byte-identical, in order");
    check(h.dup_in == 0u, "no IN packet may be delivered twice (%lu were)",
          h.dup_in);

    got = echo_roundtrip(&h, stream + 128, 128u, back, sizeof back);
    check(got == 128u && memcmp(back, stream + 128, 128) == 0,
          "the stream resumes cleanly after the host comes back");
}

static void t_tx_timeout_is_bounded(void)
{
    usbm_host_t h;
    static uint8_t big[600];
    uint32_t sent;
    unsigned long polls;

    cold_start(svc_poll);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 17u) == 0, "enumerate before the tx timeout test");

    fill_pattern(big, sizeof big, 0x3333u);

    /* The host never issues an IN, so the transmit staging can never drain.
     * bl_usb_tx() must return short rather than block forever — the whole
     * point of BL_USB_TX_TIMEOUT_MS.  The wall-clock watchdog is what turns a
     * regression here into a failure instead of a hung CI job. */
    polls = usbm_service_count();
    sent = bl_usb_tx(big, sizeof big);
    check(sent == 256u,
          "bl_usb_tx must queue exactly the 256-byte staging buffer and report "
          "it, got %u", sent);
    check(usbm_service_count() == polls,
          "bl_usb_tx pumps bl_usb_poll() directly, not through the harness");

    /* It must also not have discarded anything silently: what it says it took
     * is what comes out. */
    {
        uint32_t got;
        static uint8_t back[512];
        usbm_set_service(svc_poll);
        got = usbm_h_bulk_in(&h, back, 256u);
        check(got == 256u, "all 256 queued bytes must come back, got %u", got);
        check(got == 256u && memcmp(back, big, 256) == 0,
              "…and be the first 256 bytes queued, in order");
    }

    /* Drained, the endpoint must be PARKED AT NAK, not left at T_RES=ACK with
     * R8_UEP2_T_LEN = 0.  The difference is invisible to a byte-stream
     * comparison — a stream of zero-length packets keeps both toggles in step
     * and loses nothing — but it turns every bulk-IN the ch341 driver submits
     * into a completed zero-byte URB that it immediately resubmits, so the
     * link spins at full rate answering nothing.  tx_pump()'s else arm exists
     * to prevent exactly that. */
    {
        uint8_t pkt[64];
        uint32_t k = 0u;
        int pid = 0;
        int i;
        int all_nak = 1;

        check(!usbm_ep2_in_armed(),
              "a drained transmit side must sit at T_RES=NAK");
        check(usbm_reg8(USBM_O_UEP2_T_LEN) == 0u,
              "…with R8_UEP2_T_LEN zero");
        for (i = 0; i < 16; i++) {
            if (usbm_tok_in(h.addr, 2u, pkt, &k, &pid) != USBM_R_NAK) {
                all_nak = 0;
            }
            usbm_pump(1);
        }
        check(all_nak,
              "a drained EP2 IN must NAK, never answer with zero-length "
              "packets");
    }

    /* A second call must now succeed in full. */
    sent = bl_usb_tx(big + 256, 200u);
    check(sent == 200u, "bl_usb_tx must accept 200 bytes once drained, got %u",
          sent);
}

/* ------------------------------------------------------------------ */
/* 10. Suspend, and flag priority                                       */
/* ------------------------------------------------------------------ */

static void t_suspend_and_flags(void)
{
    usbm_host_t h;
    static uint8_t stream[64], back[128];
    uint32_t sess;

    cold_start(svc_echo);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 18u) == 0, "enumerate before the suspend test");
    svc_reset();

    fill_pattern(stream, sizeof stream, 0x2468u);

    /* 32 bytes are in flight — echoed into the transmit staging and armed, but
     * not yet read by the host — when the suspend arrives. */
    (void)usbm_h_bulk_out(&h, stream, 32u);
    usbm_pump(4);
    check(usbm_ep2_in_armed(), "an IN packet is armed when the suspend lands");

    sess = bl_usb_session();
    usbm_suspend();
    usbm_pump(2);
    check((usbm_reg8(USBM_O_INT_FG) & USBM_UIF_SUSPEND) == 0u,
          "RB_UIF_SUSPEND must be cleared by a poll");
    check(bl_usb_session() == sess,
          "a suspend must NOT cost an in-flight session");
    check(bl_usb_configured() == 1, "a suspend must not un-configure");
    check(usbm_ep2_in_armed(),
          "a suspend must not withdraw the armed IN packet");

    /* Resume is indistinguishable from suspend here, exactly as in the
     * application; the in-flight bytes have to survive both. */
    usbm_suspend();
    usbm_pump(2);
    memset(back, 0, sizeof back);
    check(usbm_h_bulk_in(&h, back, 32u) == 32u &&
          memcmp(back, stream, 32) == 0,
          "the bytes in flight across the suspend must come back intact");

    svc_reset();
    check(echo_roundtrip(&h, stream + 32, 64u, back, sizeof back) == 64u &&
          memcmp(back, stream + 32, 64) == 0,
          "the link must be intact across suspend/resume");

    /* A poll with nothing pending must be a no-op. */
    {
        uint8_t before = usbm_reg8(USBM_O_UEP2_CTRL);
        usbm_pump(50);
        check(usbm_reg8(USBM_O_UEP2_CTRL) == before,
              "an idle poll must not disturb R8_UEP2_CTRL (0x%02X -> 0x%02X)",
              before, usbm_reg8(USBM_O_UEP2_CTRL));
    }
}

/* ------------------------------------------------------------------ */
/* 11. Long soak                                                        */
/* ------------------------------------------------------------------ */

static void t_soak(void)
{
    usbm_host_t h;
    static uint8_t stream[8192], back[16384];
    unsigned round;

    cold_start(svc_echo);
    usbm_host_init(&h);
    check(usbm_enumerate(&h, 20u) == 0, "enumerate before the soak");

    for (round = 0; round < 8u; round++) {
        uint32_t n = 1u + (round * 997u) % 4000u;
        uint32_t got;

        svc_reset();
        fill_pattern(stream, n, 0x1000u + round);
        memset(back, 0, sizeof back);
        got = echo_roundtrip(&h, stream, n, back, sizeof back);
        if (!check_at(__LINE__, got == n && memcmp(back, stream, n) == 0,
                      "soak round %u (%u bytes) round-tripped %u bytes",
                      round, n, got)) {
            break;
        }
    }
    check(usbm_count_out_tog_bad() == 0u,
          "the soak saw %lu mis-toggled OUT packets",
          usbm_count_out_tog_bad());
    check(h.dup_in == 0u, "the soak saw %lu duplicated IN packets", h.dup_in);
    printf("  (%lu OUT packets, %lu IN packets, %lu service calls)\n",
           usbm_count_out_ep2(), usbm_count_in_ep2(), usbm_service_count());
}

/* ------------------------------------------------------------------ */

int main(void)
{
    usbm_watchdog(300u);

    group("bring-up");
    t_init_registers();

    group("enumeration");
    t_enumeration();
    t_vendor_replay();
    t_vendor_overread_note();
    t_stalls();

    group("bulk echo across the packet boundary");
    t_bulk_echo_sizes();

    group("the R8_UEP2_CTRL toggle race");
    t_rmw_positive_control();
    t_rmw_positive_control_tx();
    t_toggle_race();

    group("toggle mismatch");
    t_toggle_mismatch();

    group("bus reset mid-transfer");
    t_reset_mid_out();
    t_reset_mid_in();
    t_two_resets_in_a_row();

    group("session boundaries without a bus reset");
    t_session_flush();

    group("back pressure");
    t_buffer_pressure();
    t_oversize_and_fifo_ov();

    group("the host stops moving");
    t_host_stops_and_resumes();
    t_tx_timeout_is_bounded();

    group("suspend and flag priority");
    t_suspend_and_flags();

    group("soak");
    t_soak();

    /* Coverage the model deliberately does not claim:
     *   - usb_pll_power_on() is Thumb inline assembly for the safe-access
     *     window; it is compiled out here (BL_USB_PLL_POWER_ON 0) and cannot
     *     be exercised natively.
     *   - usb_echo_pump() is compiled out (BL_USB_ECHO 0, the stage-4 build);
     *     svc_echo() drives the same bl_usb_rx() -> bl_usb_tx() path.
     *   - The 16-bit R16_UEPn_DMA registers cannot hold a host pointer; only
     *     the low half is compared.
     *   - usb_delay_ms() is NOT executed here at all.  It is compiled out
     *     along with usb_pll_power_on(), its only remaining caller, by
     *     BL_USB_PLL_POWER_ON 0.  bl_usb_tx()'s timeout no longer uses it:
     *     the wait is measured on BL_USB_TIME_MS(), which under BL_HOST
     *     advances one millisecond per call.  So the SHAPE of the deadline is
     *     checked — it is bounded and it returns short — and its DURATION is
     *     not, and cannot be, checked here.  The substitute is conservative:
     *     the device polls far faster than 1 kHz, so it makes more attempts
     *     inside the same budget, never fewer.
     *   - The real bl_time_ms() accumulator is not linked into this suite.
     *     It has its own: host/test_timebase.c compiles src/timebase.c over a
     *     modelled SysTick whose clock the test advances explicitly. */
    printf("\n%d assertions, %d executions passed, %d failed, %d notes\n",
           g_nsites, g_pass, g_fail, g_warn);
    return g_fail ? 1 : 0;
}
