/* proto.h — GBFlash CH579 update-mode bootloader: wire protocol layer.
 *
 * PORTABLE. This header and src/proto.c contain no MMIO, no target headers and
 * no target-specific types. They compile unchanged for Cortex-M0 and natively
 * on the host, so the framer and the handlers can be tested byte-for-byte
 * against real updater output with zero hardware.
 *
 * All flash access goes through an injected `bl_flash_ops`; the host harness
 * substitutes a RAM-backed stub.
 *
 * Authority: docs/PROTOCOL.md, which is the byte-exact spec.
 *
 * Build options
 *   BL_DRY_RUN   parse, CRC-check and ack exactly as normal, but never call a
 *                flash op (stage 5 of the plan). Read-back verification is
 *                skipped too, because nothing was written; the image CRC and
 *                header checks still run in full.
 *   BL_HOST      host build; enables bl_proto_selftest().
 *   BL_SELFTEST  force the self-test in or out (defaults to 1 under BL_HOST).
 */

#ifndef BL_PROTO_H
#define BL_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Memory map — flash addresses. See docs/DESIGN.md §3 and §4.         */
/* ------------------------------------------------------------------ */

/* The bootloader occupies 0x0000..0x3DFF (sectors 0..30). Nothing in this
 * module may ever name an address below BL_IMAGE_BASE, which is both where
 * fw.bin byte 0 lands and the first byte the updater may touch. */
#define BL_IMAGE_BASE       0x00003E00u  /* fw.bin byte 0 lands here          */
#define BL_APP_BASE         0x00004000u  /* fw.bin byte 0x200 = app vectors   */
#define BL_HDR_PAGE_LEN     0x00000200u  /* boot-info page = one sector       */
#define BL_SECTOR           0x00000200u  /* CH579 erase granularity, verified */
#define BL_FLASH_END        0x0003E800u  /* end of CodeFlash (250 KB)         */

/* Largest application payload (fw.bin length minus its 0x200 header page)
 * this bootloader will accept.
 *
 * It MUST stay identical to boot.h's BL_APP_MAX_LEN, which is what
 * bl_app_valid() enforces, and to bl_config.h's BL_APP_MAX_SIZE.
 *
 * THE UPDATER MUST NEVER BE STRICTER THAN THE VALIDATOR.  h_data() erases the
 * boot-info sector on packet 1 BEFORE any total-size limit applies, so a
 * tighter ceiling here means an image above it erases the header, writes part
 * of the app, then gets NAKed forever — no valid application, stuck in update
 * mode, unable to accept the very firmware being installed.
 *
 * Written as a literal rather than derived because proto.c is the portable
 * module and includes only <stdint.h>, so it can be compiled natively for the
 * host harness.  host/check_headers compiles proto.h, boot.h and bl_config.h
 * side by side and FAILS if they disagree - keep them in step. */
#ifndef BL_APP_MAX_LEN
#define BL_APP_MAX_LEN      0x3A800u
#endif

/* Smallest application payload this bootloader will accept.
 *
 * It MUST stay identical to boot.h's BL_APP_MIN_LEN, which is what
 * bl_app_valid() enforces: an application must at minimum carry a full
 * 36-entry vector table (36 * 4 = 0x90).
 *
 * The mirror of BL_APP_MAX_LEN: the updater must be neither STRICTER nor LOOSER
 * than the validator.  Without this bound a host could install a 4-byte
 * "application" with self-consistent CRCs, be answered 0x01 SUCCESS, and then
 * have bl_app_valid() refuse to boot it forever — the device sitting in update
 * mode reporting a successful update.
 *
 * Spelled exactly as boot.h spells it (`0x90u`) so that a translation unit
 * including both headers sees an identical replacement list, which C permits
 * without a diagnostic; if either value is ever edited, that same TU stops
 * compiling cleanly AND host/check_headers fails.  Written as a literal, not
 * derived, for the same portability reason as BL_APP_MAX_LEN. */
#ifndef BL_APP_MIN_LEN
#define BL_APP_MIN_LEN      0x90u
#endif

/* Total accepted fw.bin size = header page + app payload:
 * 0x200 + 0x3A800 = 0x3AA00.  The governing limit is CodeFlash itself,
 * 0x4000 + 0x3A800 = 0x3E800. */
#define BL_IMAGE_MAX        (BL_HDR_PAGE_LEN + BL_APP_MAX_LEN)

/* One past the last byte the updater may ever touch.  Today this is exactly
 * BL_FLASH_END; the static assert below is what keeps it that way or tighter,
 * so range_ok() can bound everything with this single constant. */
#define BL_IMAGE_END        (BL_IMAGE_BASE + BL_IMAGE_MAX)

/* The application's first two vector-table words, captured off the wire as the
 * image streams past so that finalize can apply the boot decision's gates
 * without needing a flash read-back op:
 *   image 0x200 / flash 0x4000  initial SP
 *   image 0x204 / flash 0x4004  reset vector (Thumb pointer into the image)
 * See app_gates() in proto.c and bl_app_valid() steps 4 and 4b in boot.c. */
#define BL_APP_HEAD_LEN     8u

/* The "initial SP looks like SRAM" test, byte for byte as boot.c step 4 does
 * it: (sp & BL_SP_MASK) == BL_SP_VALUE.  bl_app_valid() uses these same macros
 * rather than literals, so writer and validator cannot drift apart; boot.h
 * carries an #ifndef-guarded copy with an identical replacement list, because
 * this header is portable and cannot include it. */
#define BL_SP_MASK          0x2FFE0000u
#define BL_SP_VALUE         0x20000000u

/* ------------------------------------------------------------------ */
/* THE GATE SET — see the long note in boot.h, which owns the story.   */
/*                                                                     */
/* This is the WRITER's half: the set of boot-info fields hdr_check()  */
/* and app_gates() inspect before finalize may answer 0x01 SUCCESS. It */
/* must equal boot.h's BL_VALIDATOR_GATES, the set bl_app_valid()      */
/* inspects before the device will boot the image — a writer that is   */
/* STRICTER refuses a legitimate update after packet 1 has already     */
/* erased the boot-info sector, and a writer that is LOOSER answers    */
/* SUCCESS for an image that will never boot. Both have happened.      */
/*                                                                     */
/* BL_GATE_MARKER is CLEAR on both sides: §2.5's record CRC covers     */
/* bytes 0x00..0x0B including the marker, so the marker is already     */
/* authenticated and an extra whitelist of two literal values can only */
/* reject an otherwise-perfect future image. hdr_check() compiles the  */
/* whitelist from this bit, so turning it on here without also turning */
/* it on in boot.h fails the build.                                    */
/*                                                                     */
/* Spelled identically to boot.h on purpose — see there for which half of the
 * tripwire catches a divergence in the SET (the _Static_asserts below and in
 * boot.h) and which catches one in the bit VALUES (check_headers.c at run
 * time, plus -Wmacro-redefined). */
#define BL_GATE_MARKER          0x01u   /* marker at 0x00 whitelisted         */
#define BL_GATE_TAG             0x02u   /* "LFBG" at 0x02                     */
#define BL_GATE_HDR_CRC         0x04u   /* record CRC at 0x0C over 0x00..0x0B */
#define BL_GATE_APPLEN          0x08u   /* BL_APP_MIN_LEN..BL_APP_MAX_LEN     */
#define BL_GATE_APP_CRC         0x10u   /* payload CRC at 0x06                */
#define BL_GATE_SP              0x20u   /* initial SP looks like SRAM         */
#define BL_GATE_RESETVEC        0x40u   /* reset vector Thumb + inside image  */

#define BL_WRITER_GATES     (BL_GATE_TAG | BL_GATE_HDR_CRC | \
                             BL_GATE_APPLEN | BL_GATE_APP_CRC | \
                             BL_GATE_SP | BL_GATE_RESETVEC)

/* Fires when proto.h is the SECOND of the pair to be included — src/boot.c.
 * boot.h carries the mirror image for the other order, which is what
 * host/check_headers.c sees. */
#ifdef BL_VALIDATOR_GATES
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(BL_WRITER_GATES == BL_VALIDATOR_GATES,
               "the updater and the boot decision must gate the same fields "
               "of the boot-info record (proto.h BL_WRITER_GATES vs boot.h "
               "BL_VALIDATOR_GATES)");
#elif defined(__GNUC__)
__extension__ _Static_assert(BL_WRITER_GATES == BL_VALIDATOR_GATES,
               "the updater and the boot decision must gate the same fields "
               "of the boot-info record");
#endif
#endif

/* ------------------------------------------------------------------ */
/* Wire constants                                                      */
/* ------------------------------------------------------------------ */

/* The two frame markers, as the 32-bit values a rolling window compares
 * against. src/proto.c also spells them out byte by byte where it emits a
 * frame and where it matches the trailing marker one byte at a time; these are
 * the named definitions those bytes serialise. BL_OUTRO has no referencing
 * code for that reason — it is not dead, it is the other half of the pair. */
#define BL_INTRO            0x48484A4Au   /* "HHJJ" big-endian */
#define BL_OUTRO            0x4A4A4848u   /* "JJHH" big-endian */

#define BL_CMD_INIT         0x0021u
#define BL_CMD_FINALIZE     0x0023u
#define BL_CMD_DATA         0x0024u

/* Values emitted in the 0x21 response. §4.5: 0x0003 is a hard gate, the two
 * UNKNOWN fields are emitted as zero, page_size 0x0200 is forced by the
 * ~100 ms FlashGBX response window (one 512-byte sector per data packet). */
#define BL_PAGE_SIZE        0x0200u
/* program_size, emitted at [3:5]. Every host parses this field and then never
 * references it — confirmed by grep across both FlashGBX trees (assigned at
 * hw_GBFlash.py:378, used nowhere) and both other host scripts. No host checks
 * len(fw) <= program_size. The stock bootloader's value is therefore
 * unknowable without a live capture and, more to the point, cannot change host
 * behaviour. It is NOT an image-size gate — that is BL_APP_MAX_LEN, enforced
 * in hdr_check(). Do not "fix" this, and do not spend hardware time on it. The
 * same applies to the zeros emitted at [0] and [5:7]. */
#define BL_PROGRAM_SIZE     0x3E00u
#define BL_INIT_MARKER      0x0003u
#define BL_SENDER_DEV       0x00u         /* never inspected by any host */

/* Response payload lengths. Every one MUST be odd: the hosts never read the
 * pad byte, so an even payload length desyncs them by one byte on the next
 * frame (docs/PROTOCOL.md, "Framing"). */
#define BL_RESP_INIT_LEN    9u
#define BL_RESP_DATA_LEN    3u
#define BL_RESP_FINAL_LEN   1u

#define BL_STATUS_OK        0x01u
#define BL_STATUS_FAIL      0x00u

/* Buffers. Largest inbound payload is a data packet: 2 + 2 + 512 + 2 = 518. */
#define BL_MAX_PAYLOAD      520u
/* Largest outbound frame: 11 + 9 + 4 (+1 pad, never used) = 25. */
#define BL_MAX_TX           28u

/* Post-intro bytes of the largest frame the framer will accept: 7 header +
 * BL_MAX_PAYLOAD + 4 outro. The framer keeps these so a framing failure can
 * rescan them for a later intro instead of discarding whole frames — see
 * frame_drop() in proto.c. */
#define BL_RAW_MAX          (7u + BL_MAX_PAYLOAD + 4u)

/* The advertised page_size is what every host chunks by, so a data packet is
 * always page_size + 6 bytes of payload (index, chunk_len, chunk, CRC). If
 * BL_PAGE_SIZE were ever raised past this bound the framer would silently drop
 * every data frame at the BL_MAX_PAYLOAD gate and the device would look dead:
 * FlashGBX reports "No response from device", the unlocker raises, getserial
 * dereferences None. 0x0400 is an equally plausible page_size for the stock
 * bootloader, so this is a live temptation for a future editor. Fail the build
 * instead. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(BL_PAGE_SIZE + 6u <= BL_MAX_PAYLOAD,
               "BL_PAGE_SIZE + 6 must fit in BL_MAX_PAYLOAD");
_Static_assert(BL_RAW_MAX >= 7u + BL_MAX_PAYLOAD + 4u,
               "BL_RAW_MAX must hold the largest accepted frame");
/* range_ok() bounds everything with BL_IMAGE_END alone. That is only safe
 * while the window stays inside CodeFlash. */
_Static_assert(BL_IMAGE_END <= BL_FLASH_END,
               "the writable window must not extend past CodeFlash");
_Static_assert(BL_APP_MIN_LEN <= BL_APP_MAX_LEN,
               "the application length window must not be empty");
/* h_data() derives the image offset as (index - 1) * BL_PAGE_SIZE and forces
 * packet 1 to be exactly BL_HDR_PAGE_LEN, so image offset 0x200 — where the
 * application's vector table starts — can only ever arrive in packet 2. The
 * app-head capture in h_data() relies on that. */
_Static_assert(BL_HDR_PAGE_LEN == BL_PAGE_SIZE,
               "the boot-info page must be exactly one wire page");
/* An application long enough to be accepted always carried its first two
 * vector words past us, so app_gates() can insist on a full capture. */
_Static_assert(BL_APP_MIN_LEN >= BL_APP_HEAD_LEN,
               "BL_APP_MIN_LEN must cover the captured vector words");
#elif defined(__GNUC__)
__extension__ _Static_assert(BL_PAGE_SIZE + 6u <= BL_MAX_PAYLOAD,
               "BL_PAGE_SIZE + 6 must fit in BL_MAX_PAYLOAD");
__extension__ _Static_assert(BL_IMAGE_END <= BL_FLASH_END,
               "the writable window must not extend past CodeFlash");
#endif

/* ------------------------------------------------------------------ */
/* Injected flash interface                                            */
/* ------------------------------------------------------------------ */

/* Guarded so flash.h may declare the same type without clashing. */
#ifndef BL_FLASH_OPS_DEFINED
#define BL_FLASH_OPS_DEFINED
typedef struct {
    /* Erase one 512-byte sector. addr is sector-aligned. 0 = success. */
    int (*erase_sector)(uint32_t addr);
    /* Program len bytes at addr. addr and len are 4-byte multiples, buf is
     * 4-byte aligned and always RAM-resident (never a flash pointer — see
     * docs/DESIGN.md §3). 0 = success. */
    int (*program)(uint32_t addr, const void *buf, uint32_t len);
    /* OPTIONAL (may be NULL): copy len bytes of flash at addr into buf, for
     * read-back verification. On the target this is a plain memcpy from the
     * address; the host stub reads its RAM array. */
    int (*read)(uint32_t addr, void *buf, uint32_t len);
} bl_flash_ops;
#endif

/* ------------------------------------------------------------------ */
/* CRC16 — plain CRC-16/MODBUS (reflected poly 0xA001, init 0xFFFF,     */
/* no final xor, no output reflection), nibble-table form.             */
/*                                                                     */
/* Known-answer vectors (the same table as docs/PROTOCOL.md, and       */
/* re-verified by bl_proto_selftest):                                  */
/*   ""              -> 0xFFFF     "\x00"        -> 0x40BF             */
/*   "\xFF"          -> 0x00FF     "123456789"   -> 0x4B37             */
/*   "LFBG"          -> 0xF387     0x00..0xFF    -> 0xDE6C             */
/*   0xAA x 511      -> 0x3F2E                                         */
/* ------------------------------------------------------------------ */

uint16_t bl_crc16(const uint8_t *p, uint32_t n);
uint16_t bl_crc16_update(uint16_t crc, const void *p, uint32_t n);

/* ------------------------------------------------------------------ */
/* Framer                                                              */
/* ------------------------------------------------------------------ */

/* bl_proto_feed() return values. >0 means "transmit st->tx[0..n)". */
#define BL_FEED_MORE         0    /* byte consumed, frame incomplete       */
#define BL_ERR_LEN          (-1)  /* payload length exceeds the buffer     */
#define BL_ERR_OUTRO        (-2)  /* outro mismatch; resynchronised        */
#define BL_ERR_PAYLOAD      (-3)  /* frame malformed; no reply sent        */
#define BL_ERR_UNKNOWN_CMD  (-4)  /* command not 0x21/0x23/0x24; ignored   */
#define BL_ERR_HDR          (-5)  /* header no real host could send;
                                   * resynchronised (see hdr_plausible)    */
#define BL_ERR_IDLE         (-6)  /* frame abandoned by bl_proto_idle()    */

enum {
    BL_S_HUNT = 0,   /* scanning for the 4-byte intro marker */
    BL_S_HDR,        /* collecting sender/seq/cmd/len        */
    BL_S_PAYLOAD,
    BL_S_OUTRO
};

#define BL_PROTO_MAGIC 0x50524F54u   /* "PROT" — marks an initialised object */

typedef struct {
    uint32_t magic;              /* BL_PROTO_MAGIC once reset has run     */

    /* --- framer --- */
    uint32_t sr;                 /* rolling window, intro hunt            */
    uint8_t  state;
    /* The frame's sender byte. Written by the framer and read by nothing: no
     * host inspects it (see BL_SENDER_DEV) and neither does this module, so it
     * is a redundant copy of hdr[0]. Deliberately retained — dropping it saves
     * 4 bytes and changes an image proven on hardware. */
    uint8_t  sender;
    uint16_t seq;
    uint16_t cmd;
    uint16_t plen;
    uint16_t got;
    uint8_t  hdr[7];
    uint8_t  payload[BL_MAX_PAYLOAD];
    /* Every post-intro byte of the frame currently being assembled. Used only
     * on a framing failure, to re-find a later intro among the bytes a stale
     * length swallowed. Reset whenever a frame starts or completes. */
    uint16_t raw_len;
    uint8_t  raw[BL_RAW_MAX];

    /* --- response --- */
    uint8_t  tx[BL_MAX_TX];
    uint16_t tx_len;

    /* --- session --- */
    const bl_flash_ops *ops;
    uint16_t next_index;         /* expected packet index, 1-based        */
    uint16_t last_len;           /* last accepted chunk (retransmit ack)  */
    uint16_t last_crc;
    uint16_t crc_img;            /* running CRC16 of the whole image      */
    uint16_t crc_app;            /* running CRC16 of image bytes >= 0x200 */
    uint32_t bytes;              /* image bytes accepted so far           */
    uint8_t  erased_hdr;         /* sector 31 erased => no valid boot-info*/
    uint8_t  short_seen;         /* a short chunk ended the image         */
    uint8_t  have_hdr_page;
    uint8_t  finalized;
    uint16_t hdr_page_len;

    /* Image bytes 0x200..0x207 — the application's initial SP and reset
     * vector — captured as they stream past. finalize applies exactly the
     * gates bl_app_valid() applies to these two words, so the device can never
     * answer SUCCESS for an image the boot decision will afterwards refuse.
     * Captured from the wire rather than read back from flash so the gates
     * hold identically with no read op and under BL_DRY_RUN. */
    uint8_t  app_head[BL_APP_HEAD_LEN];
    uint8_t  app_head_len;

    /* --- counters, for the LED / diagnostics --- */
    uint32_t n_resync;
    uint32_t n_nak;

    /* 4-byte aligned scratch. The boot-info page is held in RAM for the
     * whole session and programmed last, at finalize; `page` is the
     * per-packet staging buffer handed to ops->program (guaranteed aligned
     * and RAM-resident). */
    uint32_t hdr_page[BL_HDR_PAGE_LEN / 4];
    uint32_t page[BL_PAGE_SIZE / 4];
} bl_proto;

/* Reset the framer and the session. Safe on an uninitialised object. A flash
 * driver bound with bl_proto_bind() survives a reset. */
void bl_proto_reset(bl_proto *st);
/* Inject the flash driver. May be called before or after bl_proto_reset().
 * Pass NULL (or build with BL_DRY_RUN) for a parse-and-ack-only bootloader. */
void bl_proto_bind(bl_proto *st, const bl_flash_ops *ops);

/* Feed one received byte. Returns BL_FEED_MORE, a negative BL_ERR_*, or the
 * number of bytes to transmit from st->tx. Never overflows: any length or
 * marker violation drops the frame and returns to hunting for the intro.
 *
 * A framing failure does not discard the bytes the bad frame consumed: they
 * are rescanned for the last intro marker and the frame is restarted there, so
 * a truncated write cannot swallow the frames behind it. At most one response
 * can ever come out of one fed byte, rescan included. */
int bl_proto_feed(bl_proto *st, uint8_t byte);

/* MANDATORY for the receive loop: call this after roughly 50 ms with no byte
 * received, whenever bl_proto_feed() has not left the framer hunting.
 *
 * Without it, a host killed part-way through a data frame leaves a legal
 * header with up to 522 bytes outstanding (518 payload + the 4-byte outro, for
 * a full 512-byte data frame), and the framer eats whatever arrives next to
 * satisfy the count. Both hosts send exactly two
 * 16-byte init frames at connect and then give up ("No device found."), so a
 * reconnect can be swallowed whole and the device looks dead until it is
 * unplugged. bl_proto_feed()'s rescan cannot see this case, because a frame
 * that is still waiting has not failed.
 *
 * 50 ms is comfortably below the 100 ms the hosts leave between frames and far
 * above any legitimate mid-frame gap at 2 Mbaud. Returns like bl_proto_feed():
 * >0 means transmit st->tx[0..n). */
int bl_proto_idle(bl_proto *st);

/* Convenience wrapper for the main loop / host harness. */
typedef void (*bl_tx_fn)(const uint8_t *buf, uint16_t len, void *user);
void bl_proto_feed_buf(bl_proto *st, const uint8_t *buf, uint32_t n,
                       bl_tx_fn tx, void *user);

/* Build a framed packet into `out` (needs 11 + plen + 5 bytes). Returns the
 * total length including the pad byte appended when the frame is odd. Exposed
 * so the host harness can build host-side frames with the same code. */
uint16_t bl_frame_build(uint8_t *out, uint8_t sender, uint16_t seq,
                        uint16_t cmd, const uint8_t *payload, uint16_t plen);

/* True once 0x23 has succeeded — the caller may then reboot into the app.
 *
 * ORDERING CONTRACT for the main loop: this becomes true DURING the
 * bl_proto_feed() call that parsed the 0x23, i.e. BEFORE the 16-byte ack has
 * left the USB endpoint. A loop shaped
 *     bl_proto_feed_buf(...); if (bl_proto_finalized(&st)) reset();
 * resets while the ack is still queued, and every host then reports failure on
 * a SUCCESSFUL update (FlashGBX "No response from device", the unlocker
 * UpdateError, getserial a TypeError on None) — after which the user's natural
 * move is to run the updater again. The reboot must be gated on the transmit
 * path having drained, not on this flag alone. */
int bl_proto_finalized(const bl_proto *st);

#if defined(BL_HOST) && !defined(BL_SELFTEST)
#define BL_SELFTEST 1
#endif
#if BL_SELFTEST
/* Returns 0 on success, or the 1-based index of the first failing check. */
int bl_proto_selftest(void);
/* Asserts the two fw.bin vectors against a caller-supplied image.
 * Returns 0 on success, non-zero on mismatch. */
int bl_proto_selftest_image(const uint8_t *fw, uint32_t n);
/* HOST ONLY. Exposes proto.c's static range_ok() so the suite can name the
 * addresses it rejects and print a readable failure. Returns 1 if
 * [addr, addr+len) lies inside the writable window. Never linked into the
 * target: BL_SELFTEST is 0 there.
 *
 * IT ANSWERS THE RANGE QUESTION ONLY, and len == 0 is not a range question: the
 * empty range at a legal address is inside the window and is reported as such.
 * Refusing a zero-length OPERATION belongs to the operation — program_ok() and
 * fl_read() test it themselves. */
int bl_proto_range_probe(uint32_t addr, uint32_t len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BL_PROTO_H */
