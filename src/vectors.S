/* vectors.S — GBFlash CH579 bootloader vector table + shared trampoline.
 *
 * OWNED BY: comp:vectors+ld
 *
 * WHY A TRAMPOLINE AT ALL
 * -----------------------
 * ARMv6-M has no VTOR.  0xE000ED08 is not implemented on this part, SRAM is not
 * aliased at 0, and RB_ROM_CODE_OFS is disqualified twice over (wrong
 * granularity, and it is not cleared by SYSRESETREQ so the bootloader could
 * never regain control).  The hardware will always fetch exceptions from a
 * table at 0x00000000 — ours.
 *
 * The escape is that a vector table is just a table of pointers, and the
 * application's entries at 0x4000 are absolute, self-contained, Thumb-bit-set
 * code addresses.  ARMv6-M gives us `MRS Rd, IPSR`, and the IPSR exception
 * number is numerically identical to the vector table index (2 = NMI,
 * 3 = HardFault, 15 = SysTick, 16+n = IRQn).  So ONE shared handler can read
 * its own exception number, index the application's table, and branch there.
 * Confirmed against the vendor datasheet Table 3-1, not just ARM lore:
 * SVCall 0x2C, PendSV 0x38, SysTick 0x3C, USB 0x58, UART1 0x6C, WDOG_BAT 0x8C
 * are exactly 4 * exception_number.  20 IRQs, so 36 entries, 0x90 bytes.
 *
 * See docs/DESIGN.md §1.  The dispatcher below carries a Thumb-bit gate on top
 * of the range check; do not "simplify" it.
 */

    .syntax unified
    .cpu    cortex-m0
    .thumb

/* ========================================================================= */
/* 1. The table.  36 entries, 0x90 bytes, at flash 0x00000000.               */
/* ========================================================================= */
/*
 * Entry 0 is the initial MSP and is NEVER trampolined — the hardware consumes
 * it directly at reset, and the handoff to the application loads the app's own
 * SP from 0x4000 by hand.  Entry 1 is our own Reset_Handler.  Entry 2 (NMI) is
 * bl_nmi_handler, which is a four-instruction prologue that FALLS THROUGH into
 * bl_vector_forward — see the note at the .rept below.  Entries 3..35 are
 * bl_vector_forward directly.  All of 1..35 carry the THUMB BIT.
 *
 * The Thumb bit is not optional and is not decoration: a cleared bit 0 in a
 * vector entry is an instant HardFault, on every single interrupt.  We get it
 * from `.thumb_func` + `.type %function` on the target symbols — the ARM ELF
 * ABI has the linker set bit 0 when it resolves an R_ARM_ABS32 relocation
 * against a Thumb STT_FUNC symbol.  This is verified in the disassembly of the
 * produced image, not assumed; see the build notes.
 */

    .section .vectors, "a", %progbits
    .align  2
    .global __vectors
    .type   __vectors, %object
__vectors:
    .word   __stack_top             /*  0  Initial MSP        (never forwarded) */
    .word   Reset_Handler           /*  1  Reset              (ours)            */

    /* 2..35 — 34 entries, all reaching the same trampoline code.
     *   2 NMI, 3 HardFault, 4..10 reserved, 11 SVCall, 12..13 reserved,
     *  14 PendSV, 15 SysTick, 16..35 IRQ0..IRQ19.
     *
     * Note that we install the trampoline even in the ARM-reserved slots
     * (4..10, 12, 13).  Those exceptions cannot be taken on ARMv6-M, so the
     * entries are unreachable; pointing them at the guarded trampoline rather
     * than at 0 costs nothing and keeps the table uniform.  The trampoline's
     * range check turns the application's zeros in those same slots into a
     * defined spin instead of a branch to address 0.
     *
     * NMI (slot 2) is the ONE exception that does not go straight to the
     * trampoline.  It enters bl_nmi_handler, four instructions that lock
     * CodeFlash and then FALL THROUGH into the identical trampoline code.  Why:
     * the only NMI source on this part is the brown-out detector, the shipping
     * application arms it (R8_BAT_DET_CTRL = 0x0D, written by SystemInit ->
     * PowerMonitor before main()), and flash.c's `cpsid i` does NOT mask NMI —
     * so an NMI can land inside flash.c's R8_FLASH_PROTECT = 0x88 window and
     * either reset the chip mid-update (app present: the app's own NMI handler
     * at 0x442D issues RB_SOFTWARE_RESET and never returns) or reach the
     * trampoline's range guard with the app region erased and spin at .Lhang
     * with CodeFlash still unlocked.  Locking first makes the second case safe.
     *
     * Slot 2 must still FORWARD in the general case: the bootloader's table
     * stays installed for the application's entire life, and the application
     * re-arms its own brown-out NMI and needs 0x442D.  Hence fall-through
     * rather than a local-only handler.
     *
     * This is defence in depth.  start.S disarms the brown-out NMI outright
     * (R8_BAT_DET_CTRL = 0x01, both interrupt enables clear) before anything
     * can reach flash.c, so bl_nmi_handler should never execute at all.
     */
    .word   bl_nmi_handler          /*  2  NMI — locks flash, then forwards    */

    .rept   33
    .word   bl_vector_forward
    .endr

    .size   __vectors, . - __vectors

    /* Assemble-time proof of the table's shape.  The linker script asserts the
     * same thing again from the other side. */
    .if (. - __vectors) != 0x90
    .error  "vectors.S: vector table is not 0x90 bytes"
    .endif

/* ========================================================================= */
/* 2. The shared trampoline.                                                 */
/* ========================================================================= */
/*
 * WHICH REGISTERS ARE FREE
 * ------------------------
 * On ARMv6-M exception entry the hardware auto-stacks r0-r3, r12, LR, PC and
 * xPSR before the handler's first instruction runs.  So r0-r3 and r12 are
 * already saved and are scratch by AAPCS, and exception handlers take no
 * arguments.  r4-r11 are NOT stacked and MUST NOT be touched — this code
 * clobbers only r0 and r1.
 *
 * LR still holds EXC_RETURN when we `bx r0`, so the real handler's ordinary
 * epilogue (`pop {pc}` or `bx lr`) performs a correct exception return.  The
 * forwarding is completely transparent to the application's handler.
 *
 * THE RANGE CHECK IS MANDATORY
 * ----------------------------
 * Guarding only against 0 is not enough.  Erased CH579 CodeFlash reads 0xFF
 * (proven on this exact device), so in update mode with
 * the application region erased the fetched entry is 0xFFFFFFFF and a zero test
 * does not fire.  And the failure is worse than executing erased flash: `bx r0`
 * runs in Handler mode, where ARMv6-M interprets a PC value of 0xFFFFFFFx as
 * EXC_RETURN rather than a branch.  0xFFFFFFFF is a reserved EXC_RETURN (only
 * F1/F9/FD are valid), so it faults — and a fault taken from a fault handler on
 * M0 escalates to LOCKUP, not the intended defined spin.
 *
 * The guard accepts exactly [0x4000, 0x40000):
 *   lsrs r1, r0, #14  == 0  iff  r0 <  0x4000   -> rejects 0x00000000
 *   lsrs r1, r0, #18  != 0  iff  r0 >= 0x40000  -> rejects 0xFFFFFFFF
 * which covers the whole of CodeFlash at or above the application base.
 *
 * THE THUMB-BIT TEST IS ALSO MANDATORY
 * ------------------------------------
 * Range alone is not enough.  `bx` to an address with bit 0 CLEAR is an attempt
 * to enter ARM state, which ARMv6-M does not have: it raises a HardFault at the
 * branch.  Taken from inside a fault handler that is exactly the LOCKUP the
 * range check exists to avoid, and taken from NMI it is a HardFault the erased
 * application table cannot service either.  An in-range even word is not
 * hypothetical — it is what a table half-programmed by an interrupted update
 * looks like, and CodeFlash programming ANDs into existing contents, so a
 * partially written entry can be any bit-subset of the intended value.
 *
 *   lsls r1, r0, #31  == 0  iff  bit 0 of r0 is clear  -> rejects even targets
 *
 * (LSLS immediate always sets the flags on ARMv6-M, so this is one instruction
 * plus the branch; the shifted-out C flag is not used.)  Placed last, directly
 * above the `bx`, so nothing can be inserted between the test and the branch it
 * guards.
 *
 * BEHAVIOUR ON A REJECTED ENTRY: an unconditional `b .` spin at .Lhang.
 * This is deliberate and it is the same thing the application itself does for
 * its unused vectors (0x40B4 is a bare `b .`).  A spin is the most debuggable
 * defined state available: the device simply stops, a debugger finds the PC
 * inside the bootloader, and a power cycle plus H1 always recovers it.  The
 * alternatives are worse — resetting could loop forever, and returning is not
 * possible because we have no valid handler to return from.
 *
 * ALIGNMENT IS LOAD-BEARING
 * -------------------------
 * ARMv6-M computes a PC-relative literal address as Align(PC, 4) + imm.  If
 * this function began at a 2-mod-4 address the pool read would miss.  Hence
 * `.align 2` (= 4 bytes, ARM gas) immediately before the label, and the pool is
 * placed by the assembler — never hand-code the offset.
 *
 * COST: 44 bytes total, once, for all 34 forwarded exceptions —
 *       8 of NMI prologue (bl_nmi_handler) + 36 of dispatcher
 *       (bl_vector_forward = 26 bytes of code + 2 pad + two 4-byte literals).
 *       mrs 4 + lsls 1 + ldr-lit 2 + ldr-reg 2 + lsrs 1 + beq 1 + lsrs 1
 *       + bne 1 + lsls 1 + beq 1 + bx 3  ~= 18 cycles ~= 0.56 us at 32 MHz,
 *       plus two extra flash reads.  Baseline M0 exception entry is already
 *       ~16 cycles, and 0.56 us is ~11% of one UART byte time at 2 Mbaud.
 *       Acceptable.
 */

    .section .text.bl_vector_forward, "ax", %progbits

/* ------------------------------------------------------------------------- *
 * 2a. NMI prologue — vector slot 2 only, falls through into the dispatcher.  *
 * ------------------------------------------------------------------------- */
/*
 * Only r0/r1 are touched, and both are hardware-stacked on exception entry, so
 * this is transparent exactly as the dispatcher below is.
 *
 * R8_FLASH_PROTECT (0x40001809) is WO[7:6] / RW[3:2] per the datasheet p.16 —
 * it is NOT a safe-access (SAM) register, so re-locking is a single
 * unconditional byte store with no 0x57/0xA8 window and no branch.  0x80 is
 * RB_ROM_WE_MUST_10 with both write-enable bits clear, i.e. fully locked; it is
 * the same FLASH_LOCK value src/flash.c writes.  Storing it when flash was
 * already locked is a no-op, and it is harmless once the application is running
 * because the application contains no flash-controller code at all (a
 * whole-image scan for 0x40001800/04/08/0A returns zero hits).
 *
 * FALL-THROUGH, NOT A BRANCH: an ARMv6-M unconditional `b` reaches only +/-2 KB
 * and these two symbols are not guaranteed to stay that close in a 15.8 KB
 * image.  Keeping them adjacent in one section deletes the range question.  The
 * explicit `nop` makes the prologue 8 bytes so bl_vector_forward stays 4-byte
 * aligned WITHOUT the assembler choosing the padding for us — `.align` in a
 * text section is not guaranteed to emit an instruction we would want to
 * execute, and here the pad is on the fall-through path.
 */
    .align  2
    .thumb_func
    .global bl_nmi_handler
    .type   bl_nmi_handler, %function
bl_nmi_handler:
    ldr     r0, .Lflash_protect     /* r0 = 0x40001809                         */
    movs    r1, #0x80               /* FLASH_LOCK: RB_ROM_WE_MUST_10, WE clear */
    strb    r1, [r0]                /* CodeFlash locked before anything else   */
    nop                             /* keep the fall-through 4-byte aligned    */
    .size   bl_nmi_handler, . - bl_nmi_handler

/* ------------------------------------------------------------------------- *
 * 2b. The dispatcher proper — entered directly by slots 3..35, and by         *
 *     fall-through from bl_nmi_handler for slot 2.                           *
 * ------------------------------------------------------------------------- */
    .align  2                       /* LOAD-BEARING — 4-byte align for the pool */
    .thumb_func
    .global bl_vector_forward
    .type   bl_vector_forward, %function
bl_vector_forward:
    mrs     r0, ipsr                /* exception number == vector table index  */
    lsls    r0, r0, #2              /* -> byte offset into the app's table     */
    ldr     r1, .Lapp_vectors       /* r1 = 0x00004000                         */
    ldr     r0, [r1, r0]            /* r0 = the application's handler address  */
    lsrs    r1, r0, #14             /* == 0 iff r0 <  0x4000  (catches 0)      */
    beq     .Lhang
    lsrs    r1, r0, #18             /* != 0 iff r0 >= 0x40000 (catches 0xFFFF..)*/
    bne     .Lhang
    lsls    r1, r0, #31             /* == 0 iff Thumb bit clear (catches even) */
    beq     .Lhang
    bx      r0                      /* enter the real handler, LR = EXC_RETURN */
.Lhang:
    b       .Lhang                  /* defined spin — see the note above       */
    .align  2                       /* NOP pad, unreachable                    */
.Lapp_vectors:
    .word   0x00004000              /* == __app_base; literal, not a reloc, so
                                     * the encoding is stable and inspectable  */
.Lflash_protect:
    .word   0x40001809              /* R8_FLASH_PROTECT — read by the NMI
                                     * prologue above, never by the dispatcher */
    .size   bl_vector_forward, . - bl_vector_forward
