/* rehearse_update.c — the offline dress rehearsal for the hardware update.
 *
 * OWNED BY: comp:integrate.   Built and run by `make -C host test`.
 *
 * WHAT THIS IS
 * ------------
 * Every other suite in host/ tests one module.  This one drives a COMPLETE
 * firmware update of a REAL, UNMODIFIED stock fw.bin end to end, through the
 * shipping code, and then asks the shipping boot decision whether the result is
 * bootable.  The stack under test is:
 *
 *     host frames (bl_frame_build, the device's own builder)
 *          |
 *     src/proto.c            framer + 0x21/0x24/0x23 handlers, verbatim
 *          |  bl_flash_ops
 *     src/flash.c            the real CH579 driver, via build/flash_host.c
 *          |  R32_FLASH_ADDR / R8_FLASH_COMMAND / ...
 *     host/flash_model.c     register-level model of the flash controller
 *          |  250 KB array
 *     src/boot.c             bl_app_valid() + bl_jump_to_app(), via
 *                            build/boot_host.c  (see boot_model.h)
 *
 * Only usb.c is absent — it has its own suite (test_usb) and it carries bytes,
 * it does not interpret them.
 *
 * WHY IT MATTERS
 * --------------
 * The first hardware update will be performed on a device whose case has to be
 * opened to recover it.  Everything below happens first, at zero risk, against
 * the actual images that will be sent — the two the harness is pointed at with
 * FW_L14= and FW_L15= (see docs/BUILDING.md).  For the stock pair:
 *
 *     L14   30,484 bytes, applen 0x7514
 *     L15   30,496 bytes, applen 0x7520
 *
 * The two differ in SIZE, so running L14 -> L15 -> L14 exercises length and CRC
 * handling and the partially-rewritten tail sector, rather than rewriting
 * identical bytes three times.  Runs 2 and 3 deliberately start from the flash
 * left behind by the run before — that is the real case, an update over an
 * installed image, and it is the only way the erase path gets tested against
 * non-blank flash.
 *
 * FIDELITY: WHAT IS REAL AND WHAT IS MODELLED
 * -------------------------------------------
 *  REAL   the fw.bin bytes; the frame layout (built by the device's own
 *         bl_frame_build); proto.c's framer, handlers, CRCs and read-back
 *         verification; flash.c's guards, register sequence, status decode and
 *         re-lock; boot.c's bl_app_valid() and bl_jump_to_app() vector loads.
 *  MODEL  the flash controller (flash_model.c: program ANDs, 512-byte erase,
 *         0x88 reading back 0x08, status bit 9 stuck high, no busy bit — all
 *         hardware-measured, see docs/DESIGN.md §3).
 *  STAND-IN  the bl_flash_ops `read` member.  boot.c's bl_update_flash_read()
 *         is `static` inside the part of boot.c that -DBL_HOST_TEST excludes,
 *         so it cannot be linked; rh_flash_read() below repeats its FOUR
 *         guards, in the same order, and reads the model.  It is the one piece
 *         of the update path here that is a copy rather than the original, so
 *         rh_read_guards_match_boot_c() below tests it directly against the
 *         same table of addresses the real function's guards are written for —
 *         a copy that is not itself tested is worse than no copy, because it
 *         makes the end-to-end run look like it covered the original.
 *
 *         THIS COPY WAS ONCE STALE AND THAT IS WHY THE TEST EXISTS.  boot.c
 *         grew an `addr >= BL_CODEFLASH_END` test above the length test — the
 *         single subtraction `len > BL_CODEFLASH_END - addr` wraps to nearly
 *         4 GB for any addr past the end, so every length passed — and this
 *         file was not updated with it.  For one release the only end-to-end
 *         gate in the tree drove the update through a read path with the
 *         underflow hole still open and would not have noticed the fix being
 *         reverted.
 *  ABSENT the update-mode loop itself (bl_update_mode: USB draining, the idle
 *         timer, the session seam, the post-finalize settle window).  It is not
 *         linkable without usb.c and it has no automated coverage anywhere —
 *         see the concerns list.  What this file rehearses is everything that
 *         loop feeds and everything it commits.
 *
 * THE PROTO OBJECT LIVES IN MODELLED SRAM.  bl_proto is placed inside
 * fm_sram(), so the page pointer proto.c hands to bl_flash_program() is
 * genuinely inside the window flash.c's range_in_sram() accepts.  On a host
 * heap it would be rejected at every call and the programming path would never
 * run — the guard would "pass" by refusing everything.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>              /* _exit — async-signal-safe, for on_fault */

#include "proto.h"
#include "boot.h"
#include "flash.h"
#include "flash_model.h"
#include "boot_model.h"

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_fail;

static int check(int cond, const char *fmt, ...)
{
    va_list ap;
    g_checks++;
    if (!cond) {
        g_fail++;
        va_start(ap, fmt);
        printf("  FAIL: ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
    }
    return cond;
}

static void info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Faulting-access guard                                               */
/* ------------------------------------------------------------------ */

/* boot.c's bl_button_held() and bl_main() still dereference absolute target
 * addresses (GPIO, and the update magic at 0x20000090) — they are outside the
 * six transformed lines and this harness must never call them.  If a future
 * edit does, say so by name instead of dying with a bare SIGSEGV. */
/* Set while rh_read_guards_match_boot_c() is deliberately handing rh_flash_read
 * addresses it must refuse.  If one of those guards is ever deleted the refusal
 * becomes a memcpy from fm_mem() + 0xFFFFFFFF and the process dies inside the
 * memcpy rather than returning a wrong answer to check().  on_fault() reads
 * this so the diagnosis names the right function instead of blaming boot.c's
 * absolute MMIO — the failure is still loud either way, which is the point. */
static volatile int g_probing_read_guards;

static void on_fault(int sig)
{
    (void)sig;
    if (g_probing_read_guards) {
        fprintf(stderr,
            "\nrehearse_update: FAULT while probing rh_flash_read()'s guards.\n"
            "  A guard that boot.c's bl_update_flash_read() performs is MISSING\n"
            "  from rh_flash_read(), so an out-of-window address reached the\n"
            "  memcpy.  This is defect 5 reopening: compare the two functions,\n"
            "  test for test, in the order boot.c writes them.\n");
        _exit(70);
    }
    fprintf(stderr,
        "\nrehearse_update: FAULT on an absolute address.\n"
        "  Something called a src/boot.c function that still dereferences\n"
        "  target MMIO (bl_button_held, bl_main), or the six-line transform\n"
        "  in host/Makefile stopped covering an access in bl_app_valid /\n"
        "  bl_jump_to_app.  See host/boot_model.h.\n");
    _exit(70);
}

/* ------------------------------------------------------------------ */
/* boot_model.h — modelled CodeFlash accessors for build/boot_host.c   */
/* ------------------------------------------------------------------ */

const uint8_t *bh_p(uint32_t addr)
{
    if (addr >= FM_FLASH_SIZE) {
        fprintf(stderr, "rehearse_update: bh_p(0x%08X) outside CodeFlash\n", addr);
        _exit(70);
    }
    return fm_mem() + addr;
}

uint32_t bh_r32(uint32_t addr)
{
    const uint8_t *p;
    if (addr + 4u > FM_FLASH_SIZE) {
        fprintf(stderr, "rehearse_update: bh_r32(0x%08X) outside CodeFlash\n", addr);
        _exit(70);
    }
    p = fm_mem() + addr;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* bl_jump_to_app() under BL_HOST_TEST hands us the two words it would have
 * loaded into MSP and PC, then spins.  Capture them and unwind. */
static jmp_buf g_jump_back;
static uint32_t g_jump_sp, g_jump_pc;
static int g_jumped;

void bl_host_jump_to_app(uint32_t sp, uint32_t pc)
{
    g_jump_sp = sp;
    g_jump_pc = pc;
    g_jumped  = 1;
    longjmp(g_jump_back, 1);
}

/* ------------------------------------------------------------------ */
/* The flash ops bound into proto.c                                    */
/* ------------------------------------------------------------------ */

/* Repeats the guards of boot.c's bl_update_flash_read() (see the STAND-IN note
 * in the file header), same tests, same order:
 *
 *     if (buf == 0 || len == 0u)                     return -1;
 *     if (addr < (uint32_t)BL_BOOTINFO_BASE)         return -1;
 *     if (addr >= (uint32_t)BL_CODEFLASH_END)        return -1;
 *     if (len > (uint32_t)BL_CODEFLASH_END - addr)   return -1;
 *
 * The third test is the one that used to be missing here.  It is not
 * redundant with the fourth: for addr >= BL_CODEFLASH_END the subtraction in
 * the fourth wraps, so on its own it accepts every length at every address
 * above the end of the map — including 0xFFFFFFFF.  On the device that read
 * is a BusFault -> HardFault -> a trampoline into the application vector
 * table the update in progress has already erased -> .Lhang with CodeFlash
 * unlocked, which is the one fault in this design that needs the H1 jumper.
 *
 * boot.c's own (void)bl_time_ms() at the top has no analogue here: there is no
 * time base in this link, and the call is a timebase-contract measure, not a
 * guard.  Nothing else in the real function is omitted. */
static int rh_flash_read(uint32_t addr, void *buf, uint32_t len)
{
    if (buf == 0 || len == 0u)              return -1;
    if (addr < BL_BOOTINFO_BASE)            return -1;
    if (addr >= BL_CODEFLASH_END)           return -1;
    if (len > BL_CODEFLASH_END - addr)      return -1;
    memcpy(buf, fm_mem() + addr, len);
    return 0;
}

/* Identical in shape to boot.c's bl_update_flash_ops. */
static const bl_flash_ops rh_ops = {
    bl_flash_erase_sector,
    bl_flash_program,
    rh_flash_read
};

/* THE STAND-IN'S OWN TEST — see the STAND-IN note in the file header.
 *
 * Every case below is chosen to fail if one specific test is deleted from
 * rh_flash_read(), so this is a difference test against boot.c's guard set and
 * not a restatement of it:
 *
 *   buf == 0                     the null test
 *   len == 0                     the empty-read test
 *   addr < BL_BOOTINFO_BASE      the floor (our own code)
 *   addr >= BL_CODEFLASH_END     THE TEST THAT WAS MISSING.  Every one of
 *                                these passes the length test on its own,
 *                                because BL_CODEFLASH_END - addr has already
 *                                wrapped to a huge number.
 *   len > END - addr             the straddle, at the last legal byte
 *
 * The wrap cases are stated with their arithmetic so the reason they are here
 * survives a reader who does not have the defect report to hand. */
static void rh_read_guards_match_boot_c(void)
{
    uint8_t b[8];

    g_probing_read_guards = 1;

    /* --- accepted: the window's two edges and one byte inside it --- */
    check(rh_flash_read(BL_BOOTINFO_BASE, b, 1u) == 0,
          "read guard: the first byte of the boot-info record is readable");
    check(rh_flash_read(BL_CODEFLASH_END - 1u, b, 1u) == 0,
          "read guard: the last byte of CodeFlash is readable");
    check(rh_flash_read(BL_CODEFLASH_END - 4u, b, 4u) == 0,
          "read guard: a read ending exactly at BL_CODEFLASH_END is accepted");

    /* --- refused: arguments --- */
    check(rh_flash_read(BL_BOOTINFO_BASE, 0, 1u) == -1,
          "read guard: a null destination is refused");
    check(rh_flash_read(BL_BOOTINFO_BASE, b, 0u) == -1,
          "read guard: a zero-length read is refused");

    /* --- refused: below the floor --- */
    check(rh_flash_read(0u, b, 1u) == -1,
          "read guard: address 0 (the bootloader's own vector table) is refused");
    check(rh_flash_read(BL_BOOTINFO_BASE - 1u, b, 1u) == -1,
          "read guard: one byte below BL_BOOTINFO_BASE is refused");

    /* --- refused: at or above the end of the map ---------------------
     * These are the defect-5 cases.  With only `len > END - addr` in place:
     *   addr = END          -> END - addr = 0,          1 > 0 rejects,  so
     *                          this one alone does NOT prove the fix;
     *   addr = END + 1      -> END - addr = 0xFFFFFFFF, 1 passes;
     *   addr = 0x00040000   -> wraps, InfoFlash — reading it is harmless on
     *                          silicon but it is outside the window this
     *                          function claims to enforce;
     *   addr = 0x40001800   -> wraps, the flash controller's own MMIO;
     *   addr = 0xFFFFFFFF   -> wraps to 0x0003E801; len 1 passes and the read
     *                          itself then also wraps the pointer. */
    check(rh_flash_read(BL_CODEFLASH_END, b, 1u) == -1,
          "read guard: the first byte past BL_CODEFLASH_END is refused");
    check(rh_flash_read(BL_CODEFLASH_END + 1u, b, 1u) == -1,
          "read guard: BL_CODEFLASH_END + 1 is refused (the length test alone "
          "accepts it — END - addr has wrapped to 0xFFFFFFFF)");
    check(rh_flash_read(0x00040000u, b, 4u) == -1,
          "read guard: InfoFlash (0x40000) is refused");
    check(rh_flash_read(0x40001800u, b, 4u) == -1,
          "read guard: the flash controller's MMIO (0x40001800) is refused");
    check(rh_flash_read(0xFFFFFFFFu, b, 1u) == -1,
          "read guard: 0xFFFFFFFF with len 1 is refused (END - addr wraps to "
          "0x0003E801, so the length test alone accepts it)");
    check(rh_flash_read(0xFFFFFF00u, b, 8u) == -1,
          "read guard: 0xFFFFFF00 with len 8 is refused");

    /* --- refused: straddling the end from inside the window --- */
    check(rh_flash_read(BL_CODEFLASH_END - 1u, b, 2u) == -1,
          "read guard: a read that runs one byte off the end is refused");
    check(rh_flash_read(BL_CODEFLASH_END - 4u, b, 8u) == -1,
          "read guard: a read that straddles BL_CODEFLASH_END is refused");

    g_probing_read_guards = 0;
}

/* ------------------------------------------------------------------ */
/* Wire helpers — the host side of the conversation                    */
/* ------------------------------------------------------------------ */

static bl_proto *st;              /* lives inside fm_sram(); see the header */

#define RXCAP 256
static uint8_t  g_rx[RXCAP];
static uint32_t g_rxn;

/* Feed a frame in exactly as bl_update_mode() does: byte at a time through
 * bl_proto_feed(), transmitting whenever it returns a length. */
static void feed(const uint8_t *b, uint32_t n)
{
    uint32_t i;
    g_rxn = 0;
    for (i = 0; i < n; i++) {
        int r = bl_proto_feed(st, b[i]);
        if (r > 0) {
            if (g_rxn + (uint32_t)r > RXCAP) {
                fprintf(stderr, "rehearse_update: response overflow\n");
                _exit(2);
            }
            memcpy(g_rx + g_rxn, st->tx, (size_t)r);
            g_rxn += (uint32_t)r;
        }
    }
}

static uint16_t be16r(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

typedef struct {
    int      valid;
    uint16_t seq, cmd, plen;
    const uint8_t *payload;
} frame_t;

/* Parse a device frame the way the unlocker's get_packet() does: 11 header
 * bytes, plen payload bytes, 4 outro bytes, and never a pad byte.  An even
 * payload length desyncs a real host, so parsing this way is itself the test
 * for the odd-length rule. */
static frame_t parse_frame(const uint8_t *p, uint32_t n)
{
    frame_t f;
    memset(&f, 0, sizeof f);
    if (n < 15) return f;
    if (!(p[0] == 0x48 && p[1] == 0x48 && p[2] == 0x4A && p[3] == 0x4A)) return f;
    f.seq  = be16r(p + 5);
    f.cmd  = be16r(p + 7);
    f.plen = be16r(p + 9);
    if ((uint32_t)f.plen + 15u != n) return f;
    f.payload = p + 11;
    if (!(p[11 + f.plen] == 0x4A && p[12 + f.plen] == 0x4A
       && p[13 + f.plen] == 0x48 && p[14 + f.plen] == 0x48)) return f;
    f.valid = 1;
    return f;
}

static frame_t transact(uint16_t seq, uint16_t cmd,
                        const uint8_t *pl, uint16_t plen)
{
    uint8_t *buf = malloc((size_t)plen + 24u);
    uint16_t n = bl_frame_build(buf, 0x00, seq, cmd, pl, plen);
    feed(buf, n);
    free(buf);
    return parse_frame(g_rx, g_rxn);
}

/* ------------------------------------------------------------------ */
/* One complete update                                                 */
/* ------------------------------------------------------------------ */

static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(buf);
        return 0;
    }
    fclose(fp);
    *len = (uint32_t)n;
    return buf;
}

/* Drive the whole conversation exactly as FlashGBX does: TWO init frames at
 * connect (docs/PROTOCOL.md — both hosts send two and then give up),
 * then one 0x24 per 512-byte page, then 0x23 carrying crc and ~crc.
 * Returns 0 on a fully acked update. */
static int drive_update(const uint8_t *img, uint32_t len, uint32_t *out_pkts)
{
    const uint32_t page = BL_PAGE_SIZE;
    uint32_t npkt = (len + page - 1u) / page;
    uint16_t seq = 1;
    uint32_t k;
    frame_t f;

    for (k = 0; k < 2u; k++) {                  /* the two connect frames */
        f = transact(seq, BL_CMD_INIT, 0, 0);
        if (!f.valid || f.cmd != BL_CMD_INIT || f.seq != seq || f.plen != 9)
            return -3;
        if (be16r(f.payload + 1) != BL_INIT_MARKER)  return -3;
        if (be16r(f.payload + 7) != BL_PAGE_SIZE)    return -3;
        seq++;
    }

    for (k = 1; k <= npkt; k++) {
        uint32_t off  = (k - 1u) * page;
        uint16_t clen = (uint16_t)((len - off > page) ? page : (len - off));
        uint16_t crc  = bl_crc16(img + off, clen);
        uint8_t *pl   = malloc((size_t)clen + 6u);

        pl[0] = (uint8_t)(k >> 8);    pl[1] = (uint8_t)k;
        pl[2] = (uint8_t)(clen >> 8); pl[3] = (uint8_t)clen;
        memcpy(pl + 4, img + off, clen);
        pl[4 + clen] = (uint8_t)(crc >> 8);
        pl[5 + clen] = (uint8_t)crc;

        f = transact(seq, BL_CMD_DATA, pl, (uint16_t)(clen + 6u));
        free(pl);

        if (!f.valid || f.cmd != BL_CMD_DATA || f.seq != seq || f.plen != 3)
            return -3;
        if (be16r(f.payload) != (uint16_t)k)  return -3;
        if (f.payload[2] != BL_STATUS_OK)     return -1;
        seq++;
    }

    {
        uint16_t c = bl_crc16(img, len);
        uint8_t pl[4];
        pl[0] = (uint8_t)(c >> 8);    pl[1] = (uint8_t)c;
        pl[2] = (uint8_t)(~c >> 8);   pl[3] = (uint8_t)(~c);
        f = transact(seq, BL_CMD_FINALIZE, pl, 4);
        if (!f.valid || f.cmd != BL_CMD_FINALIZE || f.seq != seq || f.plen != 1)
            return -3;
        if (f.payload[0] != BL_STATUS_OK) return -2;
    }

    *out_pkts = npkt;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Assertions after one update                                         */
/* ------------------------------------------------------------------ */

static void assert_after(const char *label, const uint8_t *img, uint32_t len)
{
    const uint8_t *mem = fm_mem();
    uint32_t i;
    uint32_t first_bad = 0xFFFFFFFFu;
    uint32_t tail_lo, tail_hi, tail_bad = 0;
    uint32_t applen;
    uint16_t appcrc;

    /* 1. The whole fw.bin, byte for byte, at 0x3E00. */
    for (i = 0; i < len; i++) {
        if (mem[BL_IMAGE_BASE + i] != img[i]) { first_bad = i; break; }
    }
    check(first_bad == 0xFFFFFFFFu,
          "%s: flash at 0x3E00 differs from the input file at byte %u "
          "(0x%08X: got 0x%02X want 0x%02X)",
          label, first_bad, BL_IMAGE_BASE + first_bad,
          first_bad == 0xFFFFFFFFu ? 0 : mem[BL_IMAGE_BASE + first_bad],
          first_bad == 0xFFFFFFFFu ? 0 : img[first_bad]);

    /* 2. Nothing below the write floor was touched — both the byte-level
     *    tripwire pattern fm_init() laid down and the controller's own
     *    record of where commands were aimed. */
    check(fm_bootloader_intact(),
          "%s: bytes below 0x3E00 changed — the bootloader's own flash", label);
    check(fm_stat(FM_STAT_CMD_BELOW_FLOOR) == 0,
          "%s: %u flash commands were aimed below 0x3E00",
          label, fm_stat(FM_STAT_CMD_BELOW_FLOOR));

    /* 3. Nothing else the driver must never do, either. */
    check(fm_stat(FM_STAT_CMD_MISALIGNED) == 0,
          "%s: %u misaligned flash commands", label,
          fm_stat(FM_STAT_CMD_MISALIGNED));
    check(fm_stat(FM_STAT_CMD_IRQ_UNMASKED) == 0,
          "%s: %u flash commands ran with interrupts unmasked", label,
          fm_stat(FM_STAT_CMD_IRQ_UNMASKED));
    check(fm_stat(FM_STAT_DATAFLASH_WE) == 0,
          "%s: DataFlash/InfoFlash write-enable was armed — CFG_BOOT_EN is the "
          "ISP recovery floor", label);
    check(fm_stat(FM_STAT_BAD_PROTECT) == 0,
          "%s: a value other than 0x80/0x88 reached R8_FLASH_PROTECT", label);
    check(fm_stat(FM_STAT_READ_BELOW_FLOOR) == 0,
          "%s: the driver read below 0x3E00", label);
    check(fm_stat(FM_STAT_READ_OOR) == 0,
          "%s: the driver read outside CodeFlash", label);
    check(fm_stat(FM_STAT_CMD_WHILE_LOCKED) == 0,
          "%s: %u commands were issued against a locked controller", label,
          fm_stat(FM_STAT_CMD_WHILE_LOCKED));
    check(!fm_unlocked(),
          "%s: CodeFlash was left UNLOCKED after the update", label);

    /* 4. The tail of the last written sector.  proto.c erases a whole 512-byte
     *    sector per data packet, so everything past the end of the image up to
     *    the sector boundary must read erased — which is also the proof that
     *    the erase happened rather than the bytes merely being programmable. */
    tail_lo = BL_IMAGE_BASE + len;
    tail_hi = (tail_lo + BL_SECTOR - 1u) & ~(BL_SECTOR - 1u);
    for (i = tail_lo; i < tail_hi; i++) {
        if (mem[i] != 0xFF) tail_bad++;
    }
    check(tail_bad == 0,
          "%s: %u of %u bytes in the tail of the last sector (0x%04X..0x%04X) "
          "are not erased", label, tail_bad, tail_hi - tail_lo, tail_lo,
          tail_hi - 1u);

    /* 5. The committed boot-info record says what the file says. */
    applen = (uint32_t)mem[BL_IMAGE_BASE + 0x08]
           | ((uint32_t)mem[BL_IMAGE_BASE + 0x09] << 8)
           | ((uint32_t)mem[BL_IMAGE_BASE + 0x0A] << 16)
           | ((uint32_t)mem[BL_IMAGE_BASE + 0x0B] << 24);
    appcrc = (uint16_t)(mem[BL_IMAGE_BASE + 0x06]
           | ((uint16_t)mem[BL_IMAGE_BASE + 0x07] << 8));
    check(applen == len - BL_HDR_PAGE_LEN,
          "%s: stored applen 0x%X != file payload length 0x%X",
          label, applen, len - BL_HDR_PAGE_LEN);
    check(appcrc == bl_crc16(img + BL_HDR_PAGE_LEN, len - BL_HDR_PAGE_LEN),
          "%s: stored application CRC 0x%04X != CRC of the file payload 0x%04X",
          label, appcrc, bl_crc16(img + BL_HDR_PAGE_LEN, len - BL_HDR_PAGE_LEN));

    /* 6. THE QUESTION THIS FILE EXISTS TO ANSWER: would the bootloader boot it?
     *    This is src/boot.c's own bl_app_valid(), compiled from the shipping
     *    source with six address-forming lines redirected (boot_model.h). */
    check(bl_app_valid() == 1,
          "%s: bl_app_valid() REFUSED the freshly written image — the device "
          "would come back up in update mode", label);

    /* 7. And the two words it would hand the core.  bl_jump_to_app() loads
     *    them from the application vector table it just wrote; they must be
     *    the file's own, and the reset vector must carry the Thumb bit. */
    g_jumped = 0;
    if (setjmp(g_jump_back) == 0) {
        bl_jump_to_app();
    }
    {
        uint32_t want_sp = (uint32_t)img[0x200] | ((uint32_t)img[0x201] << 8)
                         | ((uint32_t)img[0x202] << 16) | ((uint32_t)img[0x203] << 24);
        uint32_t want_pc = (uint32_t)img[0x204] | ((uint32_t)img[0x205] << 8)
                         | ((uint32_t)img[0x206] << 16) | ((uint32_t)img[0x207] << 24);
        check(g_jumped, "%s: bl_jump_to_app() did not reach the handover", label);
        check(g_jump_sp == want_sp,
              "%s: handover MSP 0x%08X != the image's vector 0 0x%08X",
              label, g_jump_sp, want_sp);
        check(g_jump_pc == want_pc,
              "%s: handover PC 0x%08X != the image's reset vector 0x%08X",
              label, g_jump_pc, want_pc);
        check((g_jump_pc & 1u) == 1u,
              "%s: handover PC 0x%08X has the Thumb bit CLEAR", label, g_jump_pc);
        info("%s: handover would be MSP=0x%08X PC=0x%08X, bl_app_valid()=1",
             label, g_jump_sp, g_jump_pc);
    }
}

/* ------------------------------------------------------------------ */

static void run(const char *label, const char *path)
{
    uint32_t len = 0, npkt = 0;
    uint8_t *img = slurp(path, &len);
    uint32_t er0, pr0;
    int r;

    if (!img) {
        printf("  FAIL: cannot read %s\n", path);
        g_checks++; g_fail++;
        return;
    }

    /* A fresh framer session, exactly as the update-mode loop does at a session
     * seam.  The flash is NOT reset: runs 2 and 3 land on top of the image the
     * previous run installed, which is the real case. */
    bl_proto_reset(st);
    bl_proto_bind(st, &rh_ops);
    fm_stat_reset();
    er0 = fm_stat(FM_STAT_ERASES);
    pr0 = fm_stat(FM_STAT_PROGRAMS);

    r = drive_update(img, len, &npkt);
    check(r == 0, "%s: update did not complete (%d)", label, r);
    if (r == 0) {
        check(bl_proto_finalized(st) != 0,
              "%s: bl_proto_finalized() is false after an acked 0x23", label);
        info("%s: %s, %u bytes, %u data packets, "
             "%u sector erases, %u word programs, %u NAKs, %u resyncs",
             label, path, len, npkt,
             fm_stat(FM_STAT_ERASES) - er0, fm_stat(FM_STAT_PROGRAMS) - pr0,
             st->n_nak, st->n_resync);
        assert_after(label, img, len);
    }
    free(img);
}

int main(int argc, char **argv)
{
    const char *l14 = (argc > 1) ? argv[1]
                    : "../../fw_L14/fw.bin";
    const char *l15 = (argc > 2) ? argv[2]
                    : "../../fw/fw.bin";

    signal(SIGSEGV, on_fault);
    signal(SIGBUS,  on_fault);

    printf("\n== end-to-end update rehearsal "
           "(proto.c -> flash.c -> flash controller model -> boot.c)\n");

    /* [0, 0x3E00) gets a tripwire pattern with no 0xFF byte in it, so an erase
     * down there is always detectable; [0x3E00, 250 KB) starts erased. */
    fm_init();

    /* bl_proto must live where flash.c believes SRAM is — see the file header.
     * fm_sram() is a 32 KB block and bl_proto is about 2.2 KB. */
    if (sizeof(bl_proto) > FM_SRAM_SIZE) {
        fprintf(stderr, "rehearse_update: bl_proto does not fit modelled SRAM\n");
        return 2;
    }
    st = (bl_proto *)fm_sram();
    memset(st, 0, sizeof *st);

    check(fm_bootloader_intact(),
          "the tripwire pattern below 0x3E00 was not laid down");

    /* BEFORE the updates, not after: if the stand-in's guards do not match the
     * shipping function's, the three runs below prove nothing about the read
     * path and their result should not be read as if they did. */
    printf("\n-- rh_flash_read() vs boot.c's bl_update_flash_read() "
           "(the one copied function)\n");
    rh_read_guards_match_boot_c();

    run("L14", l14);
    run("L15", l15);
    run("L14 again", l14);

    /* The three runs together, not just each one: whatever else happened, the
     * bootloader's own 15,872 bytes were never a target. */
    check(fm_bootloader_intact(),
          "after three updates, bytes below 0x3E00 have changed");

    printf("\nrehearse_update: %d checks, %d failures\n", g_checks, g_fail);
    if (g_fail == 0) {
        printf("rehearse_update: OK — three complete stock-fw.bin updates, "
               "each byte-identical and each bootable\n");
    }
    return g_fail ? 1 : 0;
}
