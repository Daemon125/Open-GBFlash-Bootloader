/* proto.c — GBFlash CH579 update-mode bootloader: framer, CRC16, handlers.
 *
 * PORTABLE: no MMIO, no target headers, no libc. Compiles unchanged for
 * Cortex-M0 and natively on the host.
 *
 * Authority for every constant and every layout decision below:
 *   docs/PROTOCOL.md  (the byte-exact wire spec)
 *   docs/DESIGN.md §3 (flash) and §4 (the boot-info record)
 *
 * Safety invariants enforced here, in one place (region_ok()):
 *   - no address below BL_IMAGE_BASE (0x3E00) is ever erased or programmed;
 *     everything below that is this bootloader's own code;
 *   - no address at or beyond 0x3E00 + BL_IMAGE_MAX is ever touched;
 *   - every erase/program address is 512-byte sector aligned;
 *   - the source buffer handed to ops->program is always RAM-resident and
 *     4-byte aligned (see flash.h).
 *
 * And the marker trap (docs/DESIGN.md §4): the boot-info marker word is written
 * EXACTLY as the host sent it. Nothing is ever stamped over it. The stored
 * header CRC covers bytes 0x00..0x0B including the marker, so rewriting
 * 0xFFFF -> 0x5555 would invalidate the record the bootloader must later
 * validate — and would require setting flash bits, which is impossible
 * without an erase.
 */

#include "proto.h"

/* ------------------------------------------------------------------ */
/* Local memory helpers.                                               */
/*                                                                     */
/* The target links -nostdlib, so this translation unit must not emit  */
/* calls to memcpy/memset. A plain byte loop is NOT safe here: GCC's   */
/* loop-distribution pass recognises the idiom and rewrites it into a  */
/* libc call (verified — `arm-none-eabi-nm -u` showed U memcpy and     */
/* U memset with -Os). The `volatile` destination/source pointers      */
/* block that transformation without needing a build flag the Makefile */
/* owner would have to remember. Cost is ~5 k cycles per 512-byte      */
/* packet (~160 us at 32 MHz) against a ~4.9 ms flash write and a      */
/* ~100 ms host window — irrelevant.                                   */
/* Re-check with: arm-none-eabi-nm -u proto.o   (must print nothing).  */
/* ------------------------------------------------------------------ */

static void bl_zero(void *dst, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    while (n--) *d++ = 0;
}

static void bl_copy(void *dst, const void *src, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const volatile uint8_t *s = (const volatile uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void bl_fill(void *dst, uint8_t v, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    while (n--) *d++ = v;
}

/* ------------------------------------------------------------------ */
/* CRC-16/MODBUS, nibble table (init 0xFFFF, no final xor, no output   */
/* reflection). Identical to FlashGBX's FirmwareUpdater.CRC16, the     */
/* unlocker's crc16().                                                 */
/* ------------------------------------------------------------------ */

static const uint16_t bl_crc_tab[16] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400
};

uint16_t bl_crc16_update(uint16_t crc, const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) {
        uint8_t v = *b++;
        crc = (uint16_t)(bl_crc_tab[(v ^ crc) & 0x0F] ^ (crc >> 4));
        crc = (uint16_t)(bl_crc_tab[((v >> 4) ^ crc) & 0x0F] ^ (crc >> 4));
    }
    return crc;
}

uint16_t bl_crc16(const uint8_t *p, uint32_t n)
{
    return bl_crc16_update(0xFFFFu, p, n);
}

/* ------------------------------------------------------------------ */
/* Big-endian field helpers (the wire is BE everywhere; the boot-info  */
/* record on flash is LE — see the endianness trap in §4.8).           */
/* ------------------------------------------------------------------ */

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Frame builder                                                       */
/* ------------------------------------------------------------------ */

uint16_t bl_frame_build(uint8_t *out, uint8_t sender, uint16_t seq,
                        uint16_t cmd, const uint8_t *payload, uint16_t plen)
{
    uint16_t n = 0;
    uint16_t i;

    out[0] = 0x48; out[1] = 0x48; out[2] = 0x4A; out[3] = 0x4A;  /* intro */
    out[4] = sender;
    put_be16(out + 5, seq);
    put_be16(out + 7, cmd);
    put_be16(out + 9, plen);
    n = 11;
    for (i = 0; i < plen; i++) out[n++] = payload[i];
    out[n++] = 0x4A; out[n++] = 0x4A; out[n++] = 0x48; out[n++] = 0x48;
    /* Pad rule: applied to the WHOLE frame, after the outro, iff the frame
     * length is odd. The fixed part is 15 bytes, so this triggers exactly
     * when plen is even. Every response we build has an odd plen (9/3/1), so
     * no response is ever padded — which is what the hosts require, since
     * neither of them ever reads a pad byte. */
    if (n & 1u) out[n++] = 0x00;
    return n;
}

static int respond(bl_proto *st, const uint8_t *payload, uint16_t plen)
{
    /* seq and cmd are echoed verbatim; both hosts check both. */
    st->tx_len = bl_frame_build(st->tx, BL_SENDER_DEV, st->seq, st->cmd,
                                payload, plen);
    return (int)st->tx_len;
}

/* ------------------------------------------------------------------ */
/* Flash access, funnelled through bounds checks and the dry-run gate  */
/* ------------------------------------------------------------------ */

/* The single place that decides whether an address may be touched.
 * [addr, addr+len) must lie entirely inside the writable window
 * 0x3E00 .. BL_IMAGE_END, which proto.h statically asserts is inside
 * CodeFlash.
 *
 * The upper-bound test is an explicit `addr >= BL_IMAGE_END` BEFORE any
 * subtraction, exactly as flash.c's range_writable() does it: with only
 * `len > BL_IMAGE_END - addr`, any addr past the end wraps the unsigned
 * subtraction to ~4 GB and every length passes. This guard claims to be
 * independent of flash.c's, and the failure it would let through is the one
 * unrecoverable fault in the design (BusFault -> HardFault -> trampoline into
 * the erased application vector table -> spin with CodeFlash unlocked, H1
 * jumper only), so it must actually be independent.
 *
 * ZERO LENGTH IS NOT THIS FUNCTION'S BUSINESS. It answers "does
 * [addr, addr+len) lie inside the window", and the empty range at a legal
 * address does. Whether a zero-length OPERATION is meaningful is each
 * operation's own question: program_ok() rejects len 0 before it gets here,
 * fl_read() rejects it explicitly, and erase_ok() passes a constant sector. */
static int range_ok(uint32_t addr, uint32_t len)
{
    if (addr < BL_IMAGE_BASE)      return 0;   /* our own code            */
    if (addr >= BL_IMAGE_END)      return 0;   /* past the window; and it
                                                * is what makes the
                                                * subtraction below safe  */
    if (len > BL_IMAGE_END - addr) return 0;   /* runs off the end        */
    return 1;
}

/* An address that may be handed to erase_sector: whole sector, aligned. */
static int erase_ok(uint32_t addr)
{
    if (addr & (BL_SECTOR - 1u)) return 0;
    return range_ok(addr, BL_SECTOR);
}

/* An address/length that may be handed to program: word aligned, at most one
 * sector at a time, inside the window. */
static int program_ok(uint32_t addr, uint32_t len)
{
    if (addr & 3u) return 0;
    if (len == 0u || len > BL_SECTOR || (len & 3u)) return 0;
    return range_ok(addr, len);
}

static int fl_erase(bl_proto *st, uint32_t addr)
{
    if (!erase_ok(addr)) return -1;
#ifdef BL_DRY_RUN
    (void)st;
    return 0;
#else
    if (!st->ops || !st->ops->erase_sector) return -1;
    return st->ops->erase_sector(addr);
#endif
}

/* buf must be RAM-resident and 4-byte aligned; len a multiple of 4. */
static int fl_program(bl_proto *st, uint32_t addr, const void *buf, uint32_t len)
{
    if (!program_ok(addr, len)) return -1;
#ifdef BL_DRY_RUN
    (void)buf; (void)len;
    (void)st;
    return 0;
#else
    if (!st->ops || !st->ops->program) return -1;
    return st->ops->program(addr, buf, len);
#endif
}

/* Read-back verification. Optional: returns 1 = "verified", 0 = "failed",
 * and 1 (skipped) when no read op exists or we are in dry run. */
static int fl_readable(const bl_proto *st)
{
#ifdef BL_DRY_RUN
    (void)st;
    return 0;
#else
    return (st->ops && st->ops->read) ? 1 : 0;
#endif
}

static int fl_read(bl_proto *st, uint32_t addr, void *buf, uint32_t len)
{
#ifdef BL_DRY_RUN
    (void)st; (void)addr; (void)buf; (void)len;
    return -1;
#else
    /* Reads are harmless, but keep them inside the same window so a bug can
     * never hand the driver a wild address. len is capped at one sector
     * because every caller stages through st->page. The len == 0 test is here
     * rather than in range_ok(): a zero-length read is a caller bug, not an
     * address that may not be touched. */
    if (len == 0u || len > BL_SECTOR || !range_ok(addr, len)) return -1;
    if (!st->ops || !st->ops->read) return -1;
    return st->ops->read(addr, buf, len);
#endif
}

/* ------------------------------------------------------------------ */
/* Session state                                                       */
/* ------------------------------------------------------------------ */

static void session_reset(bl_proto *st)
{
    st->next_index    = 1u;
    st->last_len      = 0u;
    st->last_crc      = 0u;
    st->crc_img       = 0xFFFFu;
    st->crc_app       = 0xFFFFu;
    st->bytes         = 0u;
    st->erased_hdr    = 0u;
    st->short_seen    = 0u;
    st->have_hdr_page = 0u;
    st->finalized     = 0u;
    st->hdr_page_len  = 0u;
    st->app_head_len  = 0u;
    bl_zero(st->app_head, (uint32_t)sizeof st->app_head);
}

void bl_proto_reset(bl_proto *st)
{
    /* Preserve the bound flash driver across a reset: bl_proto_reset() may
     * legitimately be called again mid-session (e.g. after a USB reset) and
     * must not silently unbind it. The magic word makes that safe even when
     * the object was never initialised — an uninitialised stack object keeps
     * no garbage pointer. bl_proto_bind() may be called before or after. */
    const bl_flash_ops *ops = (st->magic == BL_PROTO_MAGIC) ? st->ops : 0;
    bl_zero(st, (uint32_t)sizeof(*st));
    st->magic = BL_PROTO_MAGIC;
    st->ops   = ops;
    st->state = BL_S_HUNT;
    session_reset(st);
}

void bl_proto_bind(bl_proto *st, const bl_flash_ops *ops)
{
    st->ops = ops;
}

int bl_proto_finalized(const bl_proto *st)
{
    return st->finalized ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 0x21 — initialise session                                           */
/* ------------------------------------------------------------------ */
/* Answered statelessly and unconditionally. FlashGBX and the unlocker both
 * send this TWICE with the same seq_no = 1 and throw the first response away;
 * getserial.py sends it once. A repeated seq_no = 1 is never an error.
 *
 * Payload is exactly 9 bytes (odd, so the frame is even and unpadded):
 *   [0]    UNKNOWN, never read by any host        -> 0x00
 *   [1:3]  hard gate, must be 0x0003              -> 0x0003
 *   [3:5]  program_size, parsed but unused        -> 0x3E00
 *   [5:7]  UNKNOWN, never read                    -> 0x0000
 *   [7:9]  page_size, drives the host's chunking  -> 0x0200
 */
static int h_init(bl_proto *st)
{
    uint8_t p[BL_RESP_INIT_LEN];

    p[0] = 0x00;
    put_be16(p + 1, BL_INIT_MARKER);
    put_be16(p + 3, BL_PROGRAM_SIZE);
    p[5] = 0x00; p[6] = 0x00;
    put_be16(p + 7, BL_PAGE_SIZE);

    /* Begin (or restart) a session. Both init frames arrive before any data
     * packet, so restarting here is a no-op in the normal flow; it exists so
     * a host that gave up mid-update can simply start over without a power
     * cycle. No flash is touched. */
    session_reset(st);

    return respond(st, p, BL_RESP_INIT_LEN);
}

/* ------------------------------------------------------------------ */
/* 0x24 — write firmware data packet                                   */
/* ------------------------------------------------------------------ */
/* Payload:  u16 BE index (1-based) | u16 BE chunk_len | chunk | u16 BE CRC16
 * Response: u16 BE index | u8 status (0x01 = written).
 *
 * There is no address on the wire: dest = 0x3E00 + (index - 1) * page_size.
 *
 * Ordering:
 *   - on packet 1, erase sector 31 (0x3E00) FIRST. From that instant there is
 *     no valid boot-info record, so any interruption lands back in update
 *     mode instead of booting a half-written application.
 *   - packet 1 (the boot-info page) is buffered in RAM and NOT programmed
 *     until finalize, so the record only becomes valid once the whole image
 *     is verified.
 *   - packets 2..N erase-then-program their own sector as they arrive. Never
 *     bulk-erase: 59 sectors is 83 ms typ / 142 ms worst case and blows
 *     FlashGBX's ~100 ms response window.
 */
static int nak_data(bl_proto *st, uint16_t index)
{
    uint8_t p[BL_RESP_DATA_LEN];
    st->n_nak++;
    put_be16(p, index);
    p[2] = BL_STATUS_FAIL;
    return respond(st, p, BL_RESP_DATA_LEN);
}

static int ack_data(bl_proto *st, uint16_t index)
{
    uint8_t p[BL_RESP_DATA_LEN];
    put_be16(p, index);
    p[2] = BL_STATUS_OK;
    return respond(st, p, BL_RESP_DATA_LEN);
}

static int h_data(bl_proto *st)
{
    const uint8_t *pl = st->payload;
    uint16_t index, clen, crc;
    const uint8_t *chunk;
    uint32_t off, dest, proglen;

    /* Too short to even carry an index — cannot form a meaningful ack. */
    if (st->plen < 6u) return BL_ERR_PAYLOAD;

    index = be16(pl);
    clen  = be16(pl + 2);

    if ((uint32_t)clen + 6u != (uint32_t)st->plen) return nak_data(st, index);
    if (clen == 0u || clen > BL_PAGE_SIZE)         return nak_data(st, index);

    chunk = pl + 4;
    crc   = be16(pl + 4 + clen);
    if (bl_crc16(chunk, clen) != crc) return nak_data(st, index);

    if (st->finalized) return nak_data(st, index);

    /* Retransmission of the packet we already accepted (our ack was lost).
     * Re-ack without rewriting: rewriting would double-count the streaming
     * CRC and corrupt the finalize comparison. */
    if (index != 0u && index == (uint16_t)(st->next_index - 1u)
        && clen == st->last_len && crc == st->last_crc) {
        return ack_data(st, index);
    }

    if (index != st->next_index) return nak_data(st, index);
    if (st->short_seen)          return nak_data(st, index); /* image ended */

    off = (uint32_t)(index - 1u) * (uint32_t)BL_PAGE_SIZE;
    if (off + clen > BL_IMAGE_MAX) return nak_data(st, index);

    dest = BL_IMAGE_BASE + off;
    /* dest is always sector aligned (0x3E00 + k*0x200); check anyway. */
    if (!erase_ok(dest)) return nak_data(st, index);

    /* Round the programmed length up to a word; pad with 0xFF so the tail of
     * a short final chunk matches the erased state. */
    proglen = ((uint32_t)clen + 3u) & ~3u;

    if (index == 1u) {
        /* The boot-info page must be a full sector: an image shorter than
         * 0x200 has no application at all. */
        if (clen != BL_HDR_PAGE_LEN) return nak_data(st, index);

        if (!st->erased_hdr) {
            if (fl_erase(st, BL_IMAGE_BASE) != 0) return nak_data(st, index);
            st->erased_hdr = 1u;
        }
        bl_fill(st->hdr_page, 0xFF, BL_HDR_PAGE_LEN);
        bl_copy(st->hdr_page, chunk, clen);
        st->hdr_page_len  = clen;
        st->have_hdr_page = 1u;
        /* Deliberately NOT programmed yet — see the ordering note above. */
    } else {
        /* Defence in depth, not a reachable "out of order" case: index ==
         * next_index with index != 1 implies index 1 was already accepted.
         * Kept because it stops a future edit to the index bookkeeping from
         * programming application sectors with no boot-info page buffered. */
        if (!st->have_hdr_page) return nak_data(st, index);

        bl_fill(st->page, 0xFF, BL_PAGE_SIZE);
        bl_copy(st->page, chunk, clen);

        if (fl_erase(st, dest) != 0)                       return nak_data(st, index);
        if (fl_program(st, dest, st->page, proglen) != 0)  return nak_data(st, index);

        /* Read-back verification of what we just wrote. Cheap, and it turns a
         * silent flash failure into a NAK the host will retry. */
        if (fl_readable(st)) {
            bl_fill(st->page, 0x00, BL_PAGE_SIZE);
            if (fl_read(st, dest, st->page, clen) != 0)   return nak_data(st, index);
            if (bl_crc16((const uint8_t *)st->page, clen) != crc)
                return nak_data(st, index);
        }
    }

    /* Streaming CRCs. Chunks arrive strictly in order (enforced above), so
     * both can be folded incrementally and finalize is a compare rather than
     * a 30 KB re-read. crc_app covers image bytes from 0x200 on, i.e. exactly
     * what the boot-info record's payload CRC field describes. */
    st->crc_img = bl_crc16_update(st->crc_img, chunk, clen);
    if (off + clen > BL_HDR_PAGE_LEN) {
        uint32_t skip = (off >= BL_HDR_PAGE_LEN) ? 0u : (BL_HDR_PAGE_LEN - off);
        st->crc_app = bl_crc16_update(st->crc_app, chunk + skip, clen - skip);
    }

    /* Capture the application's first two vector words for app_gates(). Packet
     * 1 is forced to be exactly BL_HDR_PAGE_LEN (== BL_PAGE_SIZE, statically
     * asserted), so image offset 0x200 can only arrive at the head of packet 2.
     * A short packet 2 leaves the capture short; app_gates() checks, because a
     * partial capture must never be read as a valid vector table. */
    if (off == BL_HDR_PAGE_LEN) {
        uint32_t nh = (clen < BL_APP_HEAD_LEN) ? (uint32_t)clen
                                               : (uint32_t)BL_APP_HEAD_LEN;
        bl_copy(st->app_head, chunk, nh);
        st->app_head_len = (uint8_t)nh;
    }

    st->bytes     += clen;
    st->last_len   = clen;
    st->last_crc   = crc;
    st->next_index = (uint16_t)(index + 1u);
    if (clen < BL_PAGE_SIZE) st->short_seen = 1u;

    return ack_data(st, index);
}

/* ------------------------------------------------------------------ */
/* 0x23 — finalise                                                     */
/* ------------------------------------------------------------------ */
/* Payload:  u16 BE CRC16(whole image) | u16 BE bitwise inverse.
 * Response: 1 byte, 0x01 = success.
 */

/* Validate the buffered boot-info record exactly as it will be validated at
 * boot time — same fields, same CRC spans — so we never commit a record the
 * bootloader would afterwards reject. NOTHING is stamped or modified.
 *
 * THE FIELDS INSPECTED HERE ARE BL_WRITER_GATES (proto.h), WHICH MUST EQUAL
 * boot.h's BL_VALIDATOR_GATES. Both headers assert it, in whichever order they
 * are included, so src/boot.c and host/check_headers.c both fail the build on a
 * divergence. Do not add a test here without adding it to bl_app_valid() and
 * setting its bit in both masks — a writer stricter than the validator answers
 * 0x00 FAIL at finalize, AFTER packet 1 has erased sector 31, over an image the
 * boot decision would have accepted.
 *
 * The marker at 0x00 is deliberately not gated: see boot.c NOTE 1a. The wire
 * value is written verbatim and never stamped over (§2.5 / §4.11 Q1). */
static int hdr_check(bl_proto *st)
{
    const uint8_t *h = (const uint8_t *)st->hdr_page;
    uint16_t hcrc;
    uint32_t applen;

#if (BL_WRITER_GATES & BL_GATE_MARKER)
    /* NOT COMPILED TODAY — the bit is clear in both gate sets. Setting
     * BL_GATE_MARKER in proto.h alone fails the build; setting it in both
     * headers restores the whitelist here and in bl_app_valid() together. */
    {
        uint16_t marker = le16(h + 0);
        if (marker != 0xFFFFu && marker != 0x5555u) return 0;
    }
#endif

    if (h[2] != 'L' || h[3] != 'F' || h[4] != 'B' || h[5] != 'G') return 0;

    applen = le32(h + 8);
    /* BOTH ends of the window bl_app_valid() enforces. The lower bound is not
     * cosmetic: without it a host could install a 4-byte "application" with
     * self-consistent CRCs, be answered 0x01 SUCCESS, and then watch the boot
     * decision refuse it forever — the writer must be neither stricter nor
     * looser than the validator (proto.h, BL_APP_MIN_LEN / BL_APP_MAX_LEN;
     * host/check_headers fails the build if either drifts from boot.h). */
    if (applen < BL_APP_MIN_LEN || applen > BL_APP_MAX_LEN) return 0;
    if (applen != st->bytes - BL_HDR_PAGE_LEN)   return 0;

    hcrc = le16(h + 12);
    if (bl_crc16(h, 12) != hcrc) return 0;

    /* The record's payload CRC must match what actually streamed past us. */
    if (le16(h + 6) != st->crc_app) return 0;

    return 1;
}

/* bl_app_valid()'s two vector-table gates — step 4 (initial SP looks like SRAM)
 * and step 4b (reset vector is a Thumb pointer inside the image) — applied to
 * the bytes that streamed past us rather than to flash, so they hold with no
 * read op injected and under BL_DRY_RUN. h_final() must never answer 0x01
 * SUCCESS over an image the boot decision will then permanently refuse; step 4b
 * in particular catches an application linked for base 0x0000, the layout a
 * bootloader-less GBFlash ships with.
 *
 * flash_verify() below re-tests the SP against what is actually in flash —
 * deliberate duplication across two independent sources (wire vs silicon).
 *
 * KEEP IN LOCKSTEP WITH boot.c:bl_app_valid(). The constants live in proto.h
 * (BL_APP_BASE, BL_SP_MASK, BL_SP_VALUE) and host/check_headers compares the
 * shared ones against boot.h. */
static int app_gates(const bl_proto *st)
{
    const uint8_t *h = (const uint8_t *)st->hdr_page;
    uint32_t applen = le32(h + 8);
    uint32_t sp, pc;

    /* A short capture means the image never carried a full vector-table head;
     * hdr_check()'s BL_APP_MIN_LEN test has already refused that, so this is
     * the guard that keeps a partial capture from ever being read as a valid
     * one should that test ever be loosened. */
    if (st->app_head_len < BL_APP_HEAD_LEN) return 0;

    sp = le32(st->app_head);
    if ((sp & BL_SP_MASK) != BL_SP_VALUE) return 0;

    pc = le32(st->app_head + 4);
    if ((pc & 1u) == 0u) return 0;                    /* Thumb bit clear   */
    if ((pc & ~1u) < BL_APP_BASE) return 0;           /* below the image   */
    if ((pc & ~1u) >= BL_APP_BASE + applen) return 0; /* past its end      */

    return 1;
}

/* Verify the application region as actually stored in flash, plus the
 * "initial SP looks like SRAM" test the boot decision applies. Skipped in dry
 * run and when no read op was injected. */
static int flash_verify(bl_proto *st)
{
    const uint8_t *h = (const uint8_t *)st->hdr_page;
    uint32_t applen = le32(h + 8);
    uint32_t addr   = BL_APP_BASE;
    uint32_t left   = applen;
    uint16_t crc    = 0xFFFFu;
    uint32_t sp;

    if (!fl_readable(st)) return 1;   /* nothing to verify against */

    if (fl_read(st, BL_APP_BASE, st->page, 4u) != 0) return 0;
    sp = le32((const uint8_t *)st->page);
    if ((sp & BL_SP_MASK) != BL_SP_VALUE) return 0;

    while (left) {
        uint32_t n = (left > BL_PAGE_SIZE) ? BL_PAGE_SIZE : left;
        if (fl_read(st, addr, st->page, n) != 0) return 0;
        crc = bl_crc16_update(crc, st->page, n);
        addr += n;
        left -= n;
    }
    return (crc == le16(h + 6)) ? 1 : 0;
}

/* Program the boot-info record LAST and in a fixed word order, so the commit
 * is atomic in a single 27 us word write:
 *
 *   words 0x10..0x1FF   the erased tail (all 0xFF in every real image)
 *   word  0x00          marker + "LF"      record still invalid
 *   word  0x04          "BG" + payload CRC record still invalid
 *   word  0x08          length             record still invalid
 *   word  0x0C          header CRC         <-- VALID from this instant
 *
 * Bytes are written verbatim; the marker word is whatever the host sent.
 */
static int commit_header(bl_proto *st)
{
    const uint32_t *w = st->hdr_page;
    const uint8_t  *a = (const uint8_t *)st->hdr_page;
    uint32_t base = BL_IMAGE_BASE;

    if (!erase_ok(base)) return 0;

    /* Tail first: 0x10 .. 0x1FF. */
    if (fl_program(st, base + 0x10u, &w[4], BL_HDR_PAGE_LEN - 0x10u) != 0) return 0;
    /* Then the record body, commit word withheld. */
    if (fl_program(st, base + 0x00u, &w[0], 4u) != 0) return 0;
    if (fl_program(st, base + 0x04u, &w[1], 4u) != 0) return 0;
    if (fl_program(st, base + 0x08u, &w[2], 4u) != 0) return 0;

    /* Read back and compare EVERYTHING EXCEPT the commit word, while the
     * record is still invalid. Order matters: a verification that ran after
     * the commit word could only report a failure over a record that was
     * already live, i.e. the device would say "update failed" while the boot
     * decision was already accepting the new application. Verifying first
     * means any read-back problem in the 508 bytes that carry the actual
     * content leaves the device safely in update mode. */
    if (fl_readable(st)) {
        const uint8_t *b = (const uint8_t *)st->page;
        uint32_t i;
        if (fl_read(st, base, st->page, BL_HDR_PAGE_LEN) != 0) return 0;
        for (i = 0; i < BL_HDR_PAGE_LEN; i++) {
            if (i >= 0x0Cu && i < 0x10u) {
                /* Must still be erased: writing it is the commit. */
                if (b[i] != 0xFFu) return 0;
            } else if (a[i] != b[i]) {
                return 0;
            }
        }
    }

    /* The commit. From the instant this word lands, the record is valid.
     * (FlashGBX reports success on a failed finalize anyway, but the unlocker
     * does not, and the device must not lie.) */
    if (fl_program(st, base + 0x0Cu, &w[3], 4u) != 0) return 0;

    /* The one check that cannot precede what it checks. If THIS read-back
     * fails, the record is already live and the device still answers 0x00 —
     * "failed" over a good image. That asymmetry is deliberate and bounded:
     * flash_verify() has just read the whole application back and matched its
     * CRC word for word, so the image the live record describes is correct.
     * The user's retry simply reinstalls it. The alternative — no check at all
     * — would let a failed commit word be reported as success. */
    if (fl_readable(st)) {
        const uint8_t *b = (const uint8_t *)st->page;
        uint32_t i;
        if (fl_read(st, base + 0x0Cu, st->page, 4u) != 0) return 0;
        for (i = 0; i < 4u; i++) if (a[0x0Cu + i] != b[i]) return 0;
    }
    return 1;
}

static int h_final(bl_proto *st)
{
    uint8_t p[BL_RESP_FINAL_LEN];
    uint16_t crc, inv;
    int ok = 1;

    if (st->plen < 4u) return BL_ERR_PAYLOAD;

    crc = be16(st->payload);
    inv = be16(st->payload + 2);

    if ((uint16_t)(crc ^ inv) != 0xFFFFu) ok = 0;

    /* Idempotent re-finalise: if the same image was already committed, say so
     * again rather than failing (FlashGBX retries are not drained). */
    if (ok && st->finalized && crc == st->crc_img) {
        p[0] = BL_STATUS_OK;
        return respond(st, p, BL_RESP_FINAL_LEN);
    }

    if (ok && st->finalized)                                    ok = 0;
    if (ok && (!st->have_hdr_page || st->next_index < 3u))      ok = 0;
    /* The next two are defence in depth, not reachable states: next_index >= 3
     * already implies bytes > BL_HDR_PAGE_LEN, and h_data() NAKs any packet
     * reaching past BL_IMAGE_MAX. Kept because they bound what commit_header()
     * is about to make bootable — but no test covers them, so do not "simplify"
     * the index bookkeeping on the assumption that they will catch it. */
    if (ok && st->bytes <= BL_HDR_PAGE_LEN)                     ok = 0;
    if (ok && st->bytes > BL_IMAGE_MAX)                         ok = 0;
    if (ok && st->crc_img != crc)                               ok = 0;
    if (ok && !hdr_check(st))                                   ok = 0;
    /* The boot decision's own vector-table gates, so SUCCESS here can never
     * describe an image bl_app_valid() would refuse. Runs before
     * flash_verify() because it is free and needs no read op. */
    if (ok && !app_gates(st))                                   ok = 0;
    if (ok && !flash_verify(st))                                ok = 0;
    if (ok && !commit_header(st))                               ok = 0;

    if (ok) st->finalized = 1u;
    else    st->n_nak++;

    p[0] = ok ? BL_STATUS_OK : BL_STATUS_FAIL;
    return respond(st, p, BL_RESP_FINAL_LEN);
}

/* ------------------------------------------------------------------ */
/* Framer                                                              */
/* ------------------------------------------------------------------ */

static int dispatch(bl_proto *st)
{
    switch (st->cmd) {
    case BL_CMD_INIT:     return h_init(st);
    case BL_CMD_DATA:     return h_data(st);
    case BL_CMD_FINALIZE: return h_final(st);
    default:              return BL_ERR_UNKNOWN_CMD;   /* silence, no NAK */
    }
}

/* Is the header we just parsed one a real host could have sent?
 *
 * All three host implementations emit exactly three commands, each with a
 * fixed or tightly bounded payload_len (docs/PROTOCOL.md, "Commands"):
 *
 *   0x21 init      payload_len == 0
 *   0x23 finalize  payload_len == 4        (crc || ~crc)
 *   0x24 data      payload_len == chunk_len + 6, chunk_len >= 1
 *
 * Rejecting an impossible header HERE rather than after swallowing plen more
 * bytes is what keeps a truncated write from eating the frames behind it. The
 * motivating case: a 16-byte init frame cut off after 10 bytes leaves hdr[]
 * one byte short, so the NEXT frame's leading 0x48 becomes the low half of
 * payload_len — cmd is still 0x21 but payload_len is now 0x0048, and the
 * framer would consume 72 + 4 more bytes, i.e. five whole init frames. Both
 * hosts send only two and then report "no device found".
 *
 * Unknown commands are deliberately NOT length-checked: the module's
 * documented behaviour is to consume them whole and stay silent, and a future
 * command's payload shape is not ours to guess. The BL_MAX_PAYLOAD bound still
 * applies to every command.
 *
 * The 0x24 lower bound is 6, not 7, so a chunk_len of 0 still reaches h_data()
 * and is answered with a NAK rather than silence; likewise the upper bound is
 * left at BL_MAX_PAYLOAD so an over-long chunk_len is NAKed by the handler
 * (which owns the page_size policy) instead of being ignored here.
 */
static int hdr_plausible(const bl_proto *st)
{
    switch (st->cmd) {
    case BL_CMD_INIT:     return st->plen == 0u;
    case BL_CMD_FINALIZE: return st->plen == 4u;
    case BL_CMD_DATA:     return st->plen >= 6u;
    default:              return 1;
    }
}

/* Advance the framer by one byte while inside a frame (i.e. not hunting).
 *
 * Returns BL_FEED_MORE, a positive transmit length, or a negative BL_ERR_*.
 * *bad is set only for a FRAMING failure — a header that cannot be real or an
 * outro that did not match — meaning the caller must resynchronise. A negative
 * return with *bad == 0 (BL_ERR_PAYLOAD, BL_ERR_UNKNOWN_CMD) is a well-framed
 * frame the handlers declined; the framer is already back in the hunt.
 */
static int feed_framed(bl_proto *st, uint8_t byte, int *bad)
{
    static const uint8_t outro[4] = { 0x4A, 0x4A, 0x48, 0x48 };

    switch (st->state) {

    case BL_S_HDR:
        st->hdr[st->got++] = byte;
        if (st->got < 7u) return BL_FEED_MORE;
        st->sender = st->hdr[0];
        st->seq    = be16(st->hdr + 1);
        st->cmd    = be16(st->hdr + 3);
        st->plen   = be16(st->hdr + 5);
        st->got    = 0;
        /* HARD bound: never let a declared length exceed the buffer. */
        if (st->plen > BL_MAX_PAYLOAD) { *bad = 1; return BL_ERR_LEN; }
        if (!hdr_plausible(st))        { *bad = 1; return BL_ERR_HDR; }
        st->state = st->plen ? BL_S_PAYLOAD : BL_S_OUTRO;
        return BL_FEED_MORE;

    case BL_S_PAYLOAD:
        st->payload[st->got++] = byte;
        if (st->got < st->plen) return BL_FEED_MORE;
        st->got   = 0;
        st->state = BL_S_OUTRO;
        return BL_FEED_MORE;

    case BL_S_OUTRO:
    default:
        if (byte != outro[st->got]) { *bad = 1; return BL_ERR_OUTRO; }
        if (++st->got < 4u) return BL_FEED_MORE;

        /* Complete, well-formed frame. Return to hunting first: the trailing
         * pad byte (present whenever plen is even, i.e. on every host frame)
         * is then swallowed harmlessly by the hunt window. We deliberately do
         * NOT consume a pad byte explicitly — a host that omitted it would
         * cost us the first byte of the next intro. */
        st->state   = BL_S_HUNT;
        st->sr      = 0;
        st->got     = 0;
        st->raw_len = 0;
        return dispatch(st);
    }
}

/* One byte, no resynchronisation. */
static int feed_one(bl_proto *st, uint8_t byte, int *bad)
{
    if (st->state == BL_S_HUNT) {
        /* Hunt for the intro with a rolling 4-byte window. This is what makes
         * the stray 0xF1 / 0x01 trigger bytes FlashGBX emits before (and
         * again after) entering update mode harmless, and what absorbs the
         * pad byte trailing every host frame. Overlapping candidates are
         * handled correctly by the shift register. */
        st->sr = (st->sr << 8) | byte;
        if (st->sr == BL_INTRO) {
            st->state   = BL_S_HDR;
            st->got     = 0;
            st->raw_len = 0;
        }
        return BL_FEED_MORE;
    }
    /* Keep the frame's post-intro bytes so a framing failure can rescan them
     * instead of throwing them away. The buffer is sized for the largest frame
     * the framer will accept, so a well-formed frame never truncates. */
    if (st->raw_len < BL_RAW_MAX) st->raw[st->raw_len++] = byte;
    return feed_framed(st, byte, bad);
}

/* A frame failed its framing checks. The bytes it consumed are NOT discarded:
 * a stale length can have swallowed whole valid frames behind it, and simply
 * returning to the hunt loses them. Instead, rescan the buffered post-intro
 * bytes for the LAST intro marker and restart the frame there.
 *
 * "Last", not "first", for two reasons. It is the frame the host most recently
 * sent, so it is the one whose answer the host is still waiting for; and
 * because no intro can follow it, replaying from it can complete at most ONE
 * frame — which keeps bl_proto_feed()'s "at most one response per byte"
 * contract intact.
 *
 * When there is no intro in the buffer at all, seed the hunt window with the
 * last three bytes so an intro straddling this frame and the next is still
 * found. (No intro can straddle the discarded intro itself: 48 48 4A 4A has no
 * proper suffix that starts 0x48 0x48.)
 *
 * The replay is done in place. The write index trails the read index by at
 * least four bytes for the whole loop, so no byte is overwritten before it is
 * read.
 */
static int frame_drop(bl_proto *st, int err)
{
    int rc = err;

    for (;;) {
        uint16_t n = st->raw_len;
        uint16_t i, src;
        int at = -1;
        int bad = 0;
        int resp = 0;

        st->n_resync++;

        for (i = 0; (uint32_t)i + 4u <= (uint32_t)n; i++) {
            if (st->raw[i] == 0x48u && st->raw[i + 1u] == 0x48u
             && st->raw[i + 2u] == 0x4Au && st->raw[i + 3u] == 0x4Au) {
                at = (int)i;
            }
        }

        st->state = BL_S_HUNT;
        st->got   = 0;
        st->sr    = 0;

        if (at < 0) {
            uint16_t keep = (n < 3u) ? n : 3u;
            for (i = (uint16_t)(n - keep); i < n; i++)
                st->sr = (st->sr << 8) | st->raw[i];
            st->raw_len = 0;
            return rc;
        }

        st->state   = BL_S_HDR;
        st->got     = 0;
        st->raw_len = 0;

        for (src = (uint16_t)((uint32_t)at + 4u); src < n; src++) {
            uint8_t b = st->raw[src];
            int r;
            bad = 0;
            r = feed_one(st, b, &bad);
            if (bad) { rc = r; break; }
            if (r > 0) resp = r;
        }
        if (bad) continue;   /* rescan what the replay itself consumed */

        return resp ? resp : rc;
    }
}

int bl_proto_feed(bl_proto *st, uint8_t byte)
{
    int bad = 0;
    int r = feed_one(st, byte, &bad);
    if (bad) return frame_drop(st, r);
    return r;
}

/* "Nothing has arrived for a while" — abandon the frame in progress.
 *
 * The rescan in frame_drop() cannot help with one shape of failure, because
 * that shape never produces a failure at all: a host killed part-way through a
 * DATA frame leaves a completely legal header with hundreds of payload bytes
 * still outstanding. The framer is then correct to keep waiting, and it
 * swallows whatever arrives next until the count is met. Two init frames are
 * 32 bytes; up to 522 bytes can be outstanding, so the user's reconnect can be
 * eaten whole, several times over. Nothing on the wire distinguishes this from a
 * slow host — only time does.
 *
 * The receive loop is fully polled, so it can see an idle line and call this.
 * Anything above ~10 ms and below the 100 ms the hosts themselves leave between
 * frames works; 50 ms is the recommendation. At 2 Mbaud no legitimate mid-frame
 * gap comes close.
 *
 * Returns like bl_proto_feed(): >0 means transmit st->tx[0..n). It can answer,
 * because the bytes the abandoned frame swallowed are rescanned for a complete
 * frame exactly as a framing failure would be.
 */
int bl_proto_idle(bl_proto *st)
{
    if (st->state == BL_S_HUNT) {
        /* Not mid-frame. Clear the hunt window anyway so a stale partial
         * marker cannot combine with the next burst. */
        st->sr = 0;
        return BL_FEED_MORE;
    }
    return frame_drop(st, BL_ERR_IDLE);
}

void bl_proto_feed_buf(bl_proto *st, const uint8_t *buf, uint32_t n,
                       bl_tx_fn tx, void *user)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int r = bl_proto_feed(st, buf[i]);
        if (r > 0 && tx) tx(st->tx, st->tx_len, user);
    }
}

/* ------------------------------------------------------------------ */
/* Self-test (host builds only)                                        */
/* ------------------------------------------------------------------ */

#if BL_SELFTEST

int bl_proto_selftest(void)
{
    static const uint8_t s123[9] = { '1','2','3','4','5','6','7','8','9' };
    static const uint8_t lfbg[4] = { 'L','F','B','G' };
    uint8_t buf[512];
    uint8_t frame[32];
    unsigned i;

    if (bl_crc16((const uint8_t *)"", 0) != 0xFFFFu) return 1;

    buf[0] = 0x00;
    if (bl_crc16(buf, 1) != 0x40BFu) return 2;

    buf[0] = 0xFF;
    if (bl_crc16(buf, 1) != 0x00FFu) return 3;

    if (bl_crc16(s123, 9) != 0x4B37u) return 4;   /* MODBUS check value */

    if (bl_crc16(lfbg, 4) != 0xF387u) return 5;

    for (i = 0; i < 256; i++) buf[i] = (uint8_t)i;
    if (bl_crc16(buf, 256) != 0xDE6Cu) return 6;

    for (i = 0; i < 511; i++) buf[i] = 0xAA;
    if (bl_crc16(buf, 511) != 0x3F2Eu) return 7;

    /* Incremental folding must equal the one-shot CRC. */
    {
        uint16_t a = bl_crc16(s123, 9);
        uint16_t b = bl_crc16_update(bl_crc16_update(0xFFFFu, s123, 4),
                                     s123 + 4, 5);
        if (a != b) return 8;
    }

    /* The literal 0x21 init frame, as docs/PROTOCOL.md spells it out. */
    {
        static const uint8_t want[16] = {
            0x48,0x48,0x4a,0x4a, 0x00, 0x00,0x01, 0x00,0x21, 0x00,0x00,
            0x4a,0x4a,0x48,0x48, 0x00
        };
        uint16_t n = bl_frame_build(frame, 0x00, 1, BL_CMD_INIT, 0, 0);
        if (n != 16u) return 9;
        for (i = 0; i < 16; i++) if (frame[i] != want[i]) return 10;
    }

    /* Every response payload length must be odd, or the hosts desync. */
    if (!(BL_RESP_INIT_LEN & 1u))  return 11;
    if (!(BL_RESP_DATA_LEN & 1u))  return 12;
    if (!(BL_RESP_FINAL_LEN & 1u)) return 13;

    /* Response frames must therefore be even and unpadded. */
    {
        uint8_t p[BL_RESP_INIT_LEN];
        uint16_t n;
        for (i = 0; i < BL_RESP_INIT_LEN; i++) p[i] = 0;
        n = bl_frame_build(frame, 0, 1, BL_CMD_INIT, p, BL_RESP_INIT_LEN);
        if (n != 24u) return 14;
    }

    /* The bounds checks must refuse everything below 0x3E00. */
    if (erase_ok(0x00000000u))                 return 15;
    if (erase_ok(0x00003C00u))                 return 16;
    if (erase_ok(0x00003DFFu))                 return 17;
    if (!erase_ok(BL_IMAGE_BASE))              return 18;
    if (!erase_ok(BL_APP_BASE))                return 19;
    if (erase_ok(BL_IMAGE_BASE + BL_IMAGE_MAX))return 20;
    if (erase_ok(BL_APP_BASE + 1u))            return 21;  /* unaligned */
    if (program_ok(0x00003DFCu, 4u))           return 22;
    if (program_ok(BL_IMAGE_BASE - 4u, 8u))    return 23;
    if (!program_ok(BL_IMAGE_BASE + 0x10u, BL_HDR_PAGE_LEN - 0x10u)) return 24;
    if (program_ok(BL_IMAGE_BASE + BL_IMAGE_MAX - 4u, 8u)) return 25;
    if (program_ok(BL_APP_BASE, 3u))           return 26;  /* not a word    */
    if (program_ok(BL_APP_BASE + 2u, 4u))      return 27;  /* misaligned    */

    /* Unsigned-underflow regression: without the explicit `addr >=
     * BL_IMAGE_END` test, the subtraction in range_ok() wraps to ~4 GB and
     * every length passes. These are the addresses that demonstrate it. */
    if (range_ok(0xFFFFFFFFu, 1u))             return 28;
    if (range_ok(0xFFFFFFFCu, 4u))             return 29;
    if (range_ok(BL_IMAGE_END, 1u))            return 30;
    if (range_ok(BL_IMAGE_END, 0x10000u))      return 31;
    if (range_ok(BL_FLASH_END, 4u))            return 32;
    if (range_ok(0x20000000u, 4u))             return 33;  /* SRAM          */
    if (range_ok(0x40001800u, 4u))             return 34;  /* flash ctrl    */
    if (range_ok(BL_IMAGE_END - 1u, 2u))       return 35;  /* straddles end */
    if (!range_ok(BL_IMAGE_END - 1u, 1u))      return 36;  /* last byte     */
    if (program_ok(0xFFFFFFFCu, 4u))           return 37;
    if (erase_ok(0xFFFFFE00u))                 return 38;

    /* Zero length. range_ok() answers the RANGE question only, so the empty
     * range at a legal address is inside the window and the one at an illegal
     * address is not; refusing a zero-length OPERATION is each operation's own
     * job. See the note on range_ok(). */
    if (!range_ok(BL_IMAGE_BASE, 0u))           return 39;
    if (!range_ok(BL_IMAGE_END - 1u, 0u))       return 40;
    if (range_ok(BL_IMAGE_BASE - 1u, 0u))       return 41;
    if (range_ok(BL_IMAGE_END, 0u))             return 42;
    if (program_ok(BL_APP_BASE, 0u))            return 43;

    return 0;
}

int bl_proto_range_probe(uint32_t addr, uint32_t len)
{
    return range_ok(addr, len);
}

int bl_proto_selftest_image(const uint8_t *fw, uint32_t n)
{
    if (n <= BL_HDR_PAGE_LEN) return 1;
    if (bl_crc16(fw + BL_HDR_PAGE_LEN, n - BL_HDR_PAGE_LEN) != le16(fw + 6))
        return 2;
    if (bl_crc16(fw, 12) != le16(fw + 12)) return 3;
    if (le32(fw + 8) != n - BL_HDR_PAGE_LEN) return 4;
    return 0;
}

#endif /* BL_SELFTEST */
