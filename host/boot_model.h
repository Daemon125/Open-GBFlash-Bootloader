/* boot_model.h — the two accessors that let src/boot.c's bl_app_valid() and
 * bl_jump_to_app() run natively against the modelled CodeFlash array.
 *
 * OWNED BY: comp:integrate.  Used only by host/rehearse_update.c.
 *
 * WHY THIS EXISTS
 * ---------------
 * The end-to-end update rehearsal has to answer one question that no other
 * host suite answers: after a complete stock fw.bin has been driven through
 * src/proto.c and src/flash.c into the flash model, WOULD THE BOOTLOADER BOOT
 * IT?  That predicate is bl_app_valid(), and asserting it against a
 * reimplementation would prove nothing about the code that ships — the whole
 * point of the check is the exact set of gates boot.c applies (the "LFBG" tag,
 * the record CRC over bytes 0x00..0x0B, the length window, the SRAM mask on
 * vector 0, the Thumb-and-in-range test on vector 1, and the payload CRC).
 *
 * So the real function is compiled and executed.  It cannot be compiled as-is:
 * it reads the boot-info record and the application through raw pointers formed
 * from BL_BOOTINFO_BASE (0x3E00) and BL_APP_BASE (0x4000), and on arm64 macOS
 * the entire low 4 GB is __PAGEZERO — permanently unmappable, so those two
 * addresses cannot be made to exist in this process.  (This is the same wall
 * flash_model.h documents; the resolution here is deliberately the same one, so
 * there is one mechanism in this tree to understand rather than two.)
 *
 * HOW src/boot.c IS REDIRECTED
 * ----------------------------
 * src/boot.c is NOT edited and carries no host-only #ifdef for this.  The host
 * build generates build/boot_host.c from it with a fixed sed transform (the
 * $(BOOTGEN) rule in host/Makefile) that rewrites EXACTLY SIX LINES, all of
 * them address-forming expressions and all of them inside the two functions
 * this harness calls.  They are named by their EXPRESSION rather than by line
 * number on purpose — line numbers rot silently every time boot.c gains a
 * comment, and a stale citation here is worse than none:
 *
 *   in bl_app_valid()      (const uint8_t *)(uintptr_t)BL_BOOTINFO_BASE -> bh_p
 *                          REG32(BL_APP_BASE)          [the SP gate]    -> bh_r32
 *                          REG32(BL_APP_BASE + 4u)     [the PC gate]    -> bh_r32
 *                          (const uint8_t *)(uintptr_t)BL_APP_BASE      -> bh_p
 *                              [the payload-CRC argument]
 *   in bl_jump_to_app()    REG32(BL_APP_BASE)          [vector 0, MSP]  -> bh_r32
 *                          REG32(BL_APP_BASE + 4u)     [vector 1, PC]   -> bh_r32
 *
 * To see exactly which lines were redirected in the build you are looking at:
 *     diff ../src/boot.c build/boot_host.c
 *
 * EVERY comparison, every constant and every early return in
 * both functions is byte-identical to the shipping source.  The transform is
 * bounded by the same build-time tripwire the flash transform uses: it must
 * change exactly BOOT_XFORM_LINES lines and must not change the line count, and
 * the diff is printed on failure.
 *
 * WHAT IS *NOT* COVERED, AND HOW THAT IS ENFORCED
 * -----------------------------------------------
 * boot.c is compiled here with -DBL_HOST_TEST, which excludes bl_update_mode()
 * and the MMIO body of bl_jump_to_app() (SysTick/NVIC quiesce, the MSP switch,
 * the BX).  What survives and is NOT transformed is bl_button_held() (GPIO) and
 * bl_main() (the update-magic word at 0x20000090).  Those still dereference
 * absolute addresses and would fault; the rehearsal never calls them, and
 * rehearse_update.c installs a SIGSEGV/SIGBUS handler so that a future edit
 * which does call one fails with a named diagnostic instead of a bare crash.
 * A "no volatile survives" scan of the preprocessed output — the second
 * tripwire on the flash transform — is deliberately NOT applied here, because
 * those two untouched functions legitimately still contain MMIO and a scan that
 * has to pass with known exceptions is a scan nobody reads.
 *
 * bl_jump_to_app() reaches the harness through bl_host_jump_to_app(), which
 * src/boot.c already declares under BL_HOST_TEST.  It does not return.
 */

#ifndef BOOT_MODEL_H
#define BOOT_MODEL_H

#include <stdint.h>

/* A readable pointer to modelled CodeFlash at `addr`.  Bounds-checked: an
 * address outside the 250 KB array aborts the run rather than reading host
 * memory, so a transform that ever pointed somewhere unintended is loud. */
const uint8_t *bh_p(uint32_t addr);

/* Little-endian 32-bit read of modelled CodeFlash at `addr`.  The target does
 * this with a word load; the model assembles it from bytes so the harness is
 * endianness-explicit rather than endianness-lucky. */
uint32_t bh_r32(uint32_t addr);

/* Called by src/boot.c's bl_jump_to_app() under BL_HOST_TEST with the initial
 * MSP and the reset vector it would have branched to.  Implemented in
 * rehearse_update.c; longjmps back to the caller. */
void bl_host_jump_to_app(uint32_t sp, uint32_t pc);

#endif /* BOOT_MODEL_H */
