/* dev_api.c — see dev_api.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dev_api.h"
#include "flash_stub.h"
#include "guard.h"
#include "proto.h"

/* The protocol object lives at the very end of a mapping whose next page is
 * PROT_NONE, so any write one byte past sizeof(bl_proto) is a SIGSEGV rather
 * than silent corruption of whatever the linker put next. See guard.h for why
 * this stands in for AddressSanitizer on this machine. */
static bl_proto *g_stp;

static bl_proto *st(void)
{
    if (!g_stp) {
        g_stp = gp_alloc_tail(sizeof(bl_proto));
        if (((uintptr_t)g_stp & 7u) != 0u) {
            fprintf(stderr, "guarded bl_proto is not 8-byte aligned\n");
            abort();
        }
    }
    return g_stp;
}

static void dev_reset_with(const bl_flash_ops *ops)
{
    fs_init();
    /* bl_proto_reset() is documented as safe on an uninitialised object; give
     * it a dirty one on purpose so the harness exercises that claim. */
    memset(st(), 0xA5, sizeof(bl_proto));
    bl_proto_reset(st());
    bl_proto_bind(st(), ops);
}

void dev_reset(void)         { dev_reset_with(fs_ops());        }
void dev_reset_noread(void)  { dev_reset_with(fs_ops_noread()); }
void dev_reset_nullops(void) { dev_reset_with(0);               }

void dev_reset_preserve(void)
{
    /* No memset, no bl_proto_bind(): whatever driver is bound must survive. */
    bl_proto_reset(st());
}

uint32_t dev_feed(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t outcap)
{
    uint32_t used = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        int r = bl_proto_feed(st(), in[i]);
        if (r > 0) {
            if ((uint32_t)r > outcap - used) return 0xFFFFFFFFu;
            memcpy(out + used, st()->tx, (size_t)r);
            used += (uint32_t)r;
        }
    }
    return used;
}

uint32_t dev_idle(uint8_t *out, uint32_t outcap)
{
    int r = bl_proto_idle(st());
    if (r <= 0) return 0;
    if ((uint32_t)r > outcap) return 0xFFFFFFFFu;
    memcpy(out, st()->tx, (size_t)r);
    return (uint32_t)r;
}

/* --- bl_proto_feed_buf() plumbing --- */
typedef struct { uint8_t *out; uint32_t cap, used; int overflow; } fb_sink;

static void fb_tx(const uint8_t *buf, uint16_t len, void *user)
{
    fb_sink *s = (fb_sink *)user;
    if ((uint32_t)len > s->cap - s->used) { s->overflow = 1; return; }
    memcpy(s->out + s->used, buf, len);
    s->used += len;
}

uint32_t dev_feed_buf(const uint8_t *in, uint32_t n, uint8_t *out,
                      uint32_t outcap)
{
    fb_sink s;
    s.out = out; s.cap = outcap; s.used = 0; s.overflow = 0;
    bl_proto_feed_buf(st(), in, n, fb_tx, &s);
    return s.overflow ? 0xFFFFFFFFu : s.used;
}

int dev_finalized(void) { return bl_proto_finalized(st()); }

uint32_t dev_flash_read(uint32_t addr, uint8_t *buf, uint32_t n)
{
    if (addr >= FS_FLASH_END) return 0;
    if (n > FS_FLASH_END - addr) n = FS_FLASH_END - addr;
    memcpy(buf, fs_mem() + addr, n);
    return n;
}

int dev_bootloader_intact(void) { return fs_bootloader_intact(); }

uint32_t dev_flash_stat(int which) { return fs_stat(which); }

uint32_t dev_proto_stat(int which)
{
    switch (which) {
    case DEV_PS_RESYNC:      return st()->n_resync;
    case DEV_PS_NAK:         return st()->n_nak;
    case DEV_PS_NEXT_INDEX:  return st()->next_index;
    case DEV_PS_BYTES:       return st()->bytes;
    case DEV_PS_STATE:       return st()->state;
    case DEV_PS_HAVE_HDR:    return st()->have_hdr_page;
    case DEV_PS_ERASED_HDR:  return st()->erased_hdr;
    case DEV_PS_SHORT_SEEN:  return st()->short_seen;
    default:                 return 0;
    }
}
