/* dev_api.h — "the device", as a callable object.
 *
 * Wraps src/proto.c plus the RAM flash stub behind a byte-in / byte-out API.
 * Two consumers:
 *   - test_proto.c / fuzz_proto.c link it statically;
 *   - gen_streams.py loads it through ctypes as a shared library and hides it
 *     behind a fake pyserial port, so the three REAL host implementations
 *     (FlashGBX's FirmwareUpdater, the unlocker, getserial.py) drive it
 *     unmodified and judge our responses with their own parsers.
 */

#ifndef DEV_API_H
#define DEV_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fresh device: flash re-initialised, protocol state reset, ops bound. */
void     dev_reset(void);
/* As dev_reset(), but with no read-back op, exercising the path where
 * per-packet verification and flash_verify() are skipped. */
void     dev_reset_noread(void);
/* As dev_reset(), but with no flash driver at all (ops == NULL). */
void     dev_reset_nullops(void);

/* bl_proto_reset() on the ALREADY-INITIALISED object, with no memset and no
 * re-bind. proto.h documents that a driver bound with bl_proto_bind() survives
 * a reset (the BL_PROTO_MAGIC branch); every other entry point here scrubs the
 * object first, so without this the documented invariant is never exercised. */
void     dev_reset_preserve(void);

/* Feed n bytes. Every response byte the device produces is appended to out.
 * Returns the number of bytes written to out, or 0xFFFFFFFF if outcap was too
 * small (which never happens for a well-formed stream: the largest response is
 * 24 bytes per input frame). */
uint32_t dev_feed(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t outcap);

/* Identical contract, but driven through bl_proto_feed_buf() + its tx callback
 * instead of the per-byte bl_proto_feed(). That entry point is the presumed
 * stage-3 USB integration point and has no other caller anywhere in the tree,
 * so it is exercised here rather than first meeting a real host. */
uint32_t dev_feed_buf(const uint8_t *in, uint32_t n, uint8_t *out,
                      uint32_t outcap);

/* bl_proto_idle(): "the receive line has been quiet". Any response it produces
 * is appended to out, exactly as dev_feed() does. */
uint32_t dev_idle(uint8_t *out, uint32_t outcap);

int      dev_finalized(void);

/* Copy n bytes of modelled flash. Returns bytes copied. */
uint32_t dev_flash_read(uint32_t addr, uint8_t *buf, uint32_t n);

/* 1 = every byte below 0x3E00 still holds its init pattern. */
int      dev_bootloader_intact(void);

/* Flash-model counters (FS_STAT_*) and protocol counters. */
uint32_t dev_flash_stat(int which);
uint32_t dev_proto_stat(int which);

enum {
    DEV_PS_RESYNC = 0,
    DEV_PS_NAK,
    DEV_PS_NEXT_INDEX,
    DEV_PS_BYTES,
    DEV_PS_STATE,
    DEV_PS_HAVE_HDR,
    DEV_PS_ERASED_HDR,
    DEV_PS_SHORT_SEEN
};

#ifdef __cplusplus
}
#endif

#endif /* DEV_API_H */
