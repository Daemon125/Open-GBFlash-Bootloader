/* fuzz_proto.c — random and mutation fuzzing of the bl_proto framer.
 *
 *   ./build/fuzz_proto [iterations] [seed]
 *
 * Every iteration builds a byte stream, feeds it in random-sized bursts and
 * then asserts the invariants that must hold no matter what arrives on the
 * wire:
 *
 *   1. no crash, no out-of-bounds access. Two mechanisms, because
 *      AddressSanitizer does not run on this machine (see guard.h):
 *        - UBSan -fsanitize=undefined,local-bounds traps any index outside the
 *          fixed-size arrays inside bl_proto (payload[520], raw[531], hdr[7],
 *          tx[28], page[128], hdr_page[128]) — the whole overflow surface of
 *          the parser;
 *        - the bl_proto object and every input buffer are allocated flush
 *          against a PROT_NONE guard page, so a stray byte either side of
 *          either one is a SIGSEGV.
 *   2. nothing below 0x3E00 is ever erased or programmed, and the modelled
 *      bootloader image is unchanged.
 *   3. the flash driver contract is never violated (alignment, sector size,
 *      bounds, source not aliasing flash), and nothing is ever programmed over
 *      flash that was not erased first.
 *   4. the parser always finds its way back to hunting for the intro: after a
 *      flush of zero bytes long enough to satisfy any pending length twice
 *      over, plus one clean init frame, it must answer.
 *   5. any response emitted is a well-formed frame with an ODD payload length
 *      — an even one desynchronises both real hosts by a byte.
 *   6. the device never believes itself finalized without a committed
 *      boot-info record on the modelled flash.
 *
 * Each iteration also rotates the flash driver across the full vector, one
 * with no read-back op, and none at all; and half the iterations sprinkle
 * bl_proto_idle() through the stream at random points, so the entry point the
 * stage-3 receive loop must call is fuzzed rather than only unit-tested.
 *
 * A failing iteration prints its seed; rerun with `./build/fuzz_proto 1 <seed>`
 * to reproduce it exactly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "proto.h"
#include "dev_api.h"
#include "flash_stub.h"
#include "guard.h"

#define MAXIN   4096u
#define RXCAP   16384u

static uint64_t g_s;

static uint32_t rnd(void)
{
    /* xorshift64*, so a run is reproducible from its seed alone. */
    g_s ^= g_s >> 12; g_s ^= g_s << 25; g_s ^= g_s >> 27;
    return (uint32_t)((g_s * 2685821657736338717ull) >> 32);
}

static uint32_t rndn(uint32_t n) { return n ? rnd() % n : 0; }

static uint8_t *g_in;      /* guarded input buffer  */
static uint8_t *g_rx;      /* response scratch      */
static uint32_t g_fail;

static uint16_t be16r(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ---- stream builders --------------------------------------------------- */

/* Completely random bytes. */
static uint32_t gen_random(uint8_t *out, uint32_t cap)
{
    uint32_t n = 1u + rndn(cap - 1u);
    uint32_t i;
    for (i = 0; i < n; i++) out[i] = (uint8_t)rnd();
    return n;
}

/* Random bytes drawn from a small alphabet loaded with the marker bytes, so
 * intro/outro candidates and near-misses appear far more often than chance. */
static uint32_t gen_markerish(uint8_t *out, uint32_t cap)
{
    static const uint8_t alpha[] = { 0x48, 0x4A, 0x00, 0x01, 0x21, 0x23, 0x24,
                                     0xF1, 0xFF, 0x02, 0x08 };
    uint32_t n = 1u + rndn(cap - 1u);
    uint32_t i;
    for (i = 0; i < n; i++) out[i] = alpha[rndn(sizeof alpha)];
    return n;
}

/* A syntactically valid frame with a random command/seq/payload. */
static uint32_t gen_frame(uint8_t *out, uint32_t cap)
{
    static const uint16_t cmds[] = { 0x21, 0x23, 0x24, 0x00, 0x22, 0x25, 0xFFFF };
    uint16_t plen = (uint16_t)rndn(cap > 600u ? 560u : (uint16_t)(cap - 20u));
    uint16_t cmd = cmds[rndn(sizeof cmds / sizeof cmds[0])];
    uint8_t *pl = malloc(plen ? plen : 1u);
    uint32_t i, n;
    for (i = 0; i < plen; i++) pl[i] = (uint8_t)rnd();
    if (cmd == 0x24 && plen >= 6) {
        /* make it plausible often enough to reach the handler's guts */
        put_be16(pl, (uint16_t)(1u + rndn(4u)));
        put_be16(pl + 2, (uint16_t)(plen - 6u));
        if (rndn(2)) put_be16(pl + plen - 2u, bl_crc16(pl + 4, plen - 6u));
    }
    n = bl_frame_build(out, (uint8_t)rnd(), (uint16_t)rndn(70000u), cmd,
                       pl, plen);
    free(pl);
    return n;
}

/* A real, valid opening exchange followed by mutated continuations.
 *
 * Packet 1 is a WELL-FORMED boot-info page, not random bytes. With random
 * bytes there hdr_check() always failed on the "LFBG" tag and neither
 * commit_header() nor flash_verify() was ever reached by the fuzzer at all;
 * with a real page a mutation only has to survive as far as the length or CRC
 * comparisons to drive the commit path. */
static uint32_t gen_session(uint8_t *out, uint32_t cap)
{
    uint8_t chunk[512];
    uint8_t pl[520];
    uint32_t n = 0, k;
    uint16_t seq = 1;
    uint32_t i;
    uint32_t applen = 0;
    uint16_t appcrc = 0xFFFFu, hcrc;
    uint16_t whole = 0xFFFFu;
    uint8_t  hdrpage[512];
    uint32_t nchunks = 1u + rndn(3u);
    uint16_t clens[4];

    /* Decide the application chunks first, so the boot-info page can describe
     * them truthfully. */
    for (k = 1; k <= nchunks; k++) {
        clens[k] = (k == nchunks) ? (uint16_t)(1u + rndn(512u)) : 512u;
        applen += clens[k];
    }

    memset(hdrpage, 0xFF, sizeof hdrpage);
    hdrpage[2] = 'L'; hdrpage[3] = 'F'; hdrpage[4] = 'B'; hdrpage[5] = 'G';
    hdrpage[8]  = (uint8_t)applen;         hdrpage[9]  = (uint8_t)(applen >> 8);
    hdrpage[10] = (uint8_t)(applen >> 16); hdrpage[11] = (uint8_t)(applen >> 24);

    n += bl_frame_build(out + n, 0, seq++, BL_CMD_INIT, 0, 0);

    /* Emit packet 1 later — its CRC fields depend on the application bytes,
     * so generate those first into a scratch stream. */
    {
        uint8_t *app = malloc(applen ? applen : 1u);
        uint32_t off = 0;
        for (i = 0; i < applen; i++) app[i] = (uint8_t)rnd();
        /* SRAM-shaped initial SP, so flash_verify()'s gate can be passed. */
        if (applen >= 4u) {
            app[0] = 0x00; app[1] = 0x80; app[2] = 0x00; app[3] = 0x20;
        }
        /* And a Thumb reset vector inside the image, so app_gates() can be
         * passed too. Without it every unmutated session would stop at the
         * vector-table gate and the fuzzer would never reach commit_header()
         * again — the same coverage collapse the "LFBG" tag caused before
         * packet 1 was made well-formed. Sessions whose applen lands below
         * BL_APP_MIN_LEN are still refused, by design. */
        if (applen >= 8u) {
            app[4] = 0x09; app[5] = 0x40; app[6] = 0x00; app[7] = 0x00;
        }
        appcrc = bl_crc16(app, applen);
        hdrpage[6] = (uint8_t)appcrc; hdrpage[7] = (uint8_t)(appcrc >> 8);
        hcrc = bl_crc16(hdrpage, 12);
        hdrpage[12] = (uint8_t)hcrc; hdrpage[13] = (uint8_t)(hcrc >> 8);

        whole = bl_crc16_update(bl_crc16(hdrpage, 512), app, applen);

        /* packet 1: the boot-info page */
        put_be16(pl, 1u);
        put_be16(pl + 2, 512u);
        memcpy(pl + 4, hdrpage, 512);
        put_be16(pl + 4 + 512, bl_crc16(hdrpage, 512));
        if (n + 560u < cap)
            n += bl_frame_build(out + n, 0, seq++, BL_CMD_DATA, pl, 518u);

        for (k = 1; k <= nchunks && n + 560u < cap; k++) {
            uint16_t clen = clens[k];
            memcpy(chunk, app + off, clen);
            off += clen;
            put_be16(pl, (uint16_t)(k + 1u));
            put_be16(pl + 2, clen);
            memcpy(pl + 4, chunk, clen);
            put_be16(pl + 4 + clen, bl_crc16(chunk, clen));
            n += bl_frame_build(out + n, 0, seq++, BL_CMD_DATA, pl,
                                (uint16_t)(clen + 6u));
        }
        free(app);
    }

    if (n + 24u < cap) {
        uint8_t f4[4];
        /* Half the time the true whole-image CRC, so an unmutated session
         * really does commit; half the time a random one. */
        uint16_t c = rndn(2) ? whole : (uint16_t)rnd();
        put_be16(f4, c);
        put_be16(f4 + 2, (uint16_t)~c);
        n += bl_frame_build(out + n, 0, seq++, BL_CMD_FINALIZE, f4, 4);
    }
    return n;
}

/* Apply k random mutations: bit flips, byte stores, truncation, duplication. */
static uint32_t mutate(uint8_t *buf, uint32_t n, uint32_t cap)
{
    uint32_t k = rndn(6u);
    while (k--) {
        switch (rndn(5)) {
        case 0: if (n) buf[rndn(n)] ^= (uint8_t)(1u << rndn(8)); break;
        case 1: if (n) buf[rndn(n)] = (uint8_t)rnd();            break;
        case 2: if (n > 1) n = 1u + rndn(n - 1u);                break;
        case 3:
            if (n && n * 2u < cap) { memcpy(buf + n, buf, n); n *= 2u; }
            break;
        case 4:
            if (n + 4u < cap) {           /* splice in an intro or an outro */
                uint32_t at = rndn(n + 1u);
                memmove(buf + at + 4u, buf + at, n - at);
                if (rndn(2)) { buf[at]=0x48; buf[at+1]=0x48; buf[at+2]=0x4A; buf[at+3]=0x4A; }
                else         { buf[at]=0x4A; buf[at+1]=0x4A; buf[at+2]=0x48; buf[at+3]=0x48; }
                n += 4u;
            }
            break;
        }
    }
    return n;
}

/* ---- per-iteration invariant checks ------------------------------------ */

static int resp_frames_well_formed(const uint8_t *p, uint32_t n)
{
    uint32_t pos = 0;
    while (pos < n) {
        uint16_t plen;
        if (n - pos < 15u) return 0;
        if (!(p[pos] == 0x48 && p[pos+1] == 0x48 &&
              p[pos+2] == 0x4A && p[pos+3] == 0x4A)) return 0;
        plen = be16r(p + pos + 9);
        if ((uint32_t)plen + 15u > n - pos) return 0;
        if ((plen & 1u) == 0u) return 0;          /* would desync both hosts */
        if (!(p[pos+11+plen] == 0x4A && p[pos+12+plen] == 0x4A &&
              p[pos+13+plen] == 0x48 && p[pos+14+plen] == 0x48)) return 0;
        pos += 15u + plen;
    }
    return 1;
}

static void fail(uint64_t seed, uint32_t iter, const char *what)
{
    printf("  FAIL iteration %u (seed %llu): %s\n",
           iter, (unsigned long long)seed, what);
    g_fail++;
}

int main(int argc, char **argv)
{
    uint32_t iters = (argc > 1) ? (uint32_t)strtoul(argv[1], 0, 0) : 30000u;
    uint64_t seed0 = (argc > 2) ? strtoull(argv[2], 0, 0) : 0x5EEDC0DE1234ull;
    uint32_t iter;
    /* The recovery bound grew when the framer gained its rescan: a stale
     * length can consume up to 535 bytes before it fails, and the frame the
     * rescan then restarts can want up to 535 more. 1400 covers both with
     * room, and the invariant being tested is "it always comes back", not the
     * exact byte count. */
    uint8_t flush[1400];
    uint8_t initframe[32];
    uint32_t initlen;
    uint32_t total_bytes = 0, total_resp = 0;
    uint32_t idle_used = 0;

    setvbuf(stdout, 0, _IONBF, 0);
    g_in = gp_alloc_tail(MAXIN);
    g_rx = malloc(RXCAP);
    memset(flush, 0, sizeof flush);
    initlen = bl_frame_build(initframe, 0, 1, BL_CMD_INIT, 0, 0);

    printf("fuzzing bl_proto_feed: %u iterations, seed %llu\n",
           iters, (unsigned long long)seed0);

    for (iter = 0; iter < iters; iter++) {
        uint64_t seed = seed0 + iter;
        uint32_t n, fedpos = 0, resp = 0;
        int use_idle;
        g_s = seed ? seed : 1u;

        /* Rotate the flash driver: the full vector, one with no read-back op,
         * and none at all. The fuzzer used to exercise only the first, so the
         * degraded-driver paths saw no random input. */
        switch (rndn(8)) {
        case 0:  dev_reset_noread();  break;
        case 1:  dev_reset_nullops(); break;
        default: dev_reset();         break;
        }
        use_idle = (int)rndn(2);

        switch (rndn(4)) {
        case 0: n = gen_random(g_in, MAXIN);    break;
        case 1: n = gen_markerish(g_in, MAXIN); break;
        case 2: n = gen_frame(g_in, MAXIN);     break;
        default: n = gen_session(g_in, MAXIN);  break;
        }
        n = mutate(g_in, n, MAXIN);
        if (n > MAXIN) n = MAXIN;

        /* feed in random bursts; the buffer's last byte abuts a guard page, so
         * any read past the end faults here rather than being ignored */
        while (fedpos < n) {
            uint32_t take = 1u + rndn(97u);
            uint32_t got;
            if (take > n - fedpos) take = n - fedpos;
            got = dev_feed(g_in + fedpos, take, g_rx, RXCAP);
            if (got == 0xFFFFFFFFu) { fail(seed, iter, "response overflow"); break; }
            if (!resp_frames_well_formed(g_rx, got))
                fail(seed, iter, "malformed or even-payload_len response");
            resp += got;
            fedpos += take;
            /* Half the runs sprinkle in the idle notification the stage-3
             * loop will send, at arbitrary points. It must never produce a
             * malformed frame and never break any invariant below. */
            if (use_idle && rndn(4) == 0u) {
                uint32_t ig = dev_idle(g_rx, RXCAP);
                idle_used++;
                if (ig == 0xFFFFFFFFu) { fail(seed, iter, "idle overflow"); break; }
                if (!resp_frames_well_formed(g_rx, ig))
                    fail(seed, iter, "malformed response from bl_proto_idle");
                resp += ig;
            }
        }
        total_bytes += n;
        total_resp += resp;

        if (!dev_bootloader_intact())
            fail(seed, iter, "flash below 0x3E00 was modified");
        if (dev_flash_stat(FS_STAT_FLOOR_HITS))
            fail(seed, iter, "a write below 0x3E00 was attempted");
        if (dev_flash_stat(FS_STAT_VIOLATIONS))
            fail(seed, iter, "flash driver contract violated "
                             "(alignment / bounds / size)");
        /* A stream that programmed over flash that had not been erased first
         * would fail on silicon. The stub models it exactly (AND, then verify)
         * but nothing here was checking the counter, so such a stream would
         * have passed unnoticed. */
        if (dev_flash_stat(FS_STAT_VERIFY_FAILS))
            fail(seed, iter, "programmed over non-erased flash");
        /* The device must never claim a finalize it did not earn: the boot
         * decision reads the record, so a valid record over a random image is
         * the one outcome that would brick a user's device. */
        if (dev_finalized()) {
            uint8_t h[16];
            dev_flash_read(0x3E00u, h, 16);
            if (h[12] == 0xFF && h[13] == 0xFF)
                fail(seed, iter, "finalized with no committed boot-info record");
        }

        /* Must always come back to hunting and answer a clean init. */
        (void)dev_feed(flush, sizeof flush, g_rx, RXCAP);
        if (dev_proto_stat(DEV_PS_STATE) != 0)
            fail(seed, iter, "parser not back in the intro hunt after a flush");
        {
            uint32_t got = dev_feed(initframe, initlen, g_rx, RXCAP);
            if (got != 24u || g_rx[7] != 0x00 || g_rx[8] != 0x21)
                fail(seed, iter, "did not answer a clean init after recovery");
        }

        if (g_fail >= 10u) {
            printf("  stopping after %u failures\n", g_fail);
            break;
        }
    }

    printf("fuzz: %u iterations, %u bytes fed, %u response bytes, "
           "%u idle notifications, %u failures\n",
           iter, total_bytes, total_resp, idle_used, g_fail);
    gp_free(g_in, MAXIN);
    free(g_rx);
    return g_fail ? 1 : 0;
}
