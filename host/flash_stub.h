/* flash_stub.h — RAM-backed CH579 CodeFlash model for the native harness.
 *
 * Models the parts of src/flash.c that can bite the protocol layer:
 *   - 512-byte erase granularity, sector-aligned addresses only;
 *   - programming CLEARS bits only (1 -> 0). The stub ANDs into the array and
 *     then read-back-verifies, exactly as bl_flash_program() does, so a program
 *     over non-erased flash FAILS here just as it does on silicon;
 *   - word alignment on both address and length (of the DESTINATION only: the
 *     real driver assembles each word byte-wise and explicitly tolerates an
 *     unaligned source, so the stub does too. What it does refuse is a source
 *     that aliases the modelled flash, standing in for the driver's
 *     range_in_sram() / BL_FLASH_ERR_SRC check);
 *   - word-at-a-time programming that stops at the first word that fails to
 *     verify, leaving the rest of the range untouched;
 *   - a hard floor at 0x3E00 ON THE WRITE PATHS. Anything below that is the
 *     bootloader's own code and must never be erased or programmed. Attempts
 *     are counted, refused, and additionally caught by a whole-region compare
 *     against a pattern written at init.
 *
 * THE READ OP IS DELIBERATELY MORE PERMISSIVE THAN THE ONE THAT SHIPS, and
 * that asymmetry is the point rather than drift. On the device the `read`
 * member is boot.c's bl_update_flash_read(), which refuses anything below
 * BL_BOOTINFO_BASE as well as anything at or past BL_CODEFLASH_END. fs_read()
 * enforces only the upper bound. If it enforced the floor too, a proto.c that
 * asked to read its own vector table would be stopped by the STUB, the test
 * would still go green, and proto.c's range_ok() -- the guard actually under
 * test in this harness -- would never have to be right. So the floor is left
 * to proto.c and asserted directly against it: see t_range_guard() in
 * test_proto.c, which drives bl_proto_range_probe() at 0x0000, 0x3DFC and
 * 0x3DFF. rehearse_update.c's rh_flash_read() is the copy that DOES repeat
 * boot.c's four guards, and it has its own difference test.
 *
 * The array is 250 KB, the real CodeFlash size.
 */

#ifndef FLASH_STUB_H
#define FLASH_STUB_H

#include <stdint.h>
#include "proto.h"   /* bl_flash_ops */

#define FS_FLASH_END    0x0003E800u   /* exclusive: 250 KB CodeFlash        */
#define FS_WRITE_FLOOR  0x00003E00u   /* first byte the updater may touch   */
#define FS_SECTOR       512u

/* Statistics / violation counters. Read with fs_stat(). */
enum {
    FS_STAT_ERASES = 0,      /* accepted erases                            */
    FS_STAT_PROGRAMS,        /* accepted programs                          */
    FS_STAT_READS,
    FS_STAT_VIOLATIONS,      /* refused: floor, bounds or alignment        */
    FS_STAT_FLOOR_HITS,      /* refused specifically for being below 0x3E00*/
    FS_STAT_VERIFY_FAILS,    /* program over non-erased flash              */
    FS_STAT_PROG_BYTES,
    FS_STAT_INJECTED,        /* faults actually delivered by fs_inject()   */
    FS_STAT_NCOUNTERS
};

/* ---- fault injection ------------------------------------------------- *
 *
 * The stub above cannot fail, so without this every "the flash misbehaved"
 * path in proto.c — NAK on a failed erase, NAK on a failed program, NAK on a
 * read-back mismatch, a failed boot-info commit — is dead code in the harness.
 * Those are exactly the paths that decide whether a device with a marginal
 * sector ends up with a half-written application and a VALID boot-info record.
 *
 * Each injector arms a one-shot fault on the Nth call of its kind, counting
 * from 1, and is cleared by fs_init(). n == 0 disarms.
 */
typedef enum {
    FS_FAULT_NONE = 0,
    FS_FAULT_ERASE,       /* the Nth erase returns -1 and changes nothing   */
    FS_FAULT_PROGRAM,     /* the Nth program word refuses to take           */
    FS_FAULT_READ,        /* the Nth read returns -1                        */
    FS_FAULT_READ_CORRUPT /* the Nth read succeeds but flips one bit in buf */
} fs_fault_t;

void fs_inject(fs_fault_t what, uint32_t nth);

/* Reset the model: bootloader region [0,0x3E00) refilled with a deterministic
 * pattern, application region [0x3E00,END) set to 0xFF, counters zeroed. */
void     fs_init(void);

/* 1 when every byte below FS_WRITE_FLOOR still holds its init pattern. */
int      fs_bootloader_intact(void);

uint32_t fs_stat(int which);

/* Direct inspection (does not count as a device read). */
const uint8_t *fs_mem(void);

/* The ops vector handed to bl_proto_bind(). */
const bl_flash_ops *fs_ops(void);
/* Same, but with the optional read op set to NULL, to exercise the
 * "no read-back verification available" path. */
const bl_flash_ops *fs_ops_noread(void);

#endif /* FLASH_STUB_H */
