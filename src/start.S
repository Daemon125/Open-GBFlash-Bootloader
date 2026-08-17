/* start.S — GBFlash CH579 bootloader reset entry.
 *
 * OWNED BY: comp:vectors+ld
 *
 * Runs from flash 0x0000 via vector[1].  Establishes MSP, DISARMS THE BROWN-OUT
 * NMI (§1a — the only NMI source on this part, and the reason flash.c's unlock
 * window is otherwise interruptible), sets the system clock, initialises
 * .data/.bss, and calls bl_main() — which never returns.
 *
 * Build-time switches (all default to the safe/documented behaviour):
 *   -DBL_INIT_CLOCK=0        skip the clock write entirely (see §2 below)
 */

    .syntax unified
    .cpu    cortex-m0
    .thumb

/* Register addresses are spelled out here rather than #included from
 * bl_config.h so this file stays readable on its own; they are the same values,
 * and that header is the reference.
 *
 * These are cpp #defines, NOT `.equ`.  `.equ` emits an ABS symbol into the
 * symbol table, and objdump then annotates any instruction whose ADDRESS
 * happens to equal the constant's VALUE with that name — which turns the
 * disassembly of a file full of 0x40001040-style constants into nonsense.  On
 * this project the disassembly is the review artifact, so it must stay clean. */
#define R8_SAFE_ACCESS_SIG  0x40001040
#define SAFE_ACCESS_SIG1    0x57
#define SAFE_ACCESS_SIG2    0xA8
#define R16_CLK_SYS_CFG     0x40001008
#define CLK_SYS_CFG_VALUE   0x0088
#define R8_BAT_DET_CTRL     0x40001024      /* RWA/SAM.  [+0] CTRL, [+1] CFG   */
#define BAT_DET_EN_ONLY     0x01            /* RB_BAT_DET_EN; both IEs CLEAR   */

#ifndef BL_INIT_CLOCK
#define BL_INIT_CLOCK 1
#endif

    .section .text.Reset_Handler, "ax", %progbits
    .align  2
    .thumb_func
    .global Reset_Handler
    .type   Reset_Handler, %function
Reset_Handler:

/* ------------------------------------------------------------------------- */
/* 1. Stack pointer                                                          */
/* ------------------------------------------------------------------------- */
/* The hardware already loaded MSP from vector[0] at reset, so this is
 * redundant on the reset path — but it costs three instructions and makes the
 * entry point correct if it is ever reached by a branch rather than a reset
 * (for example from a future in-flash recovery stub).  There is no PSP on this
 * design and thread mode always uses MSP. */
    ldr     r0, =__stack_top
    msr     msp, r0

/* NOTE, deliberately absent: no `cpsid i`.
 * PRIMASK is left exactly as reset delivered it (interrupts enabled, but no
 * NVIC source enabled, so nothing can actually fire).  Setting PRIMASK here
 * would be inherited by the application across the handoff — the application's
 * startup never clears it, so its USB and UART interrupts would never fire and
 * the device would appear dead after a perfectly successful boot.  The "run
 * fully polled" rule (docs/DESIGN.md §2) is satisfied by never ENABLING an
 * NVIC source, not by masking. */

/* ------------------------------------------------------------------------- */
/* 1a. Disarm the brown-out NMI.  FIRST REGISTER WRITE OF THE BOOTLOADER.     */
/* ------------------------------------------------------------------------- */
/*
 * The stock application runs with R8_BAT_DET_CTRL = 0x0D (RB_BAT_DET_EN |
 * RB_BAT_LOWER_IE | RB_BAT_LOW_IE) — confirmed in the image at SystemInit
 * 0x46F0 -> PowerMonitor 0x4524, which runs before main() on every boot, and
 * read back as 0x0D from the live device.  Per the datasheet p.20 an NMI is
 * requested when the ULTRA-LOW threshold is crossed AND both interrupt enables
 * are set; the brown-out detector is the only NMI source on this part
 * (DS Table 3-1).  Clearing either IE makes the NMI unrequestable.
 *
 * Whether SYSRESETREQ already cleared them is NOT knowable from the manual.
 * DS §4.1 carves out only R8_GLOB_RESET_KEEP and RB_ROM_CODE_OFS as surviving
 * a software reset, which IMPLIES this register is cleared — but that is an
 * inference from an exception list, it is never stated, and it has never been
 * measured (any post-reset read on a stock device is taken after SystemInit has
 * already re-armed 0x0D).  src/flash.c guards its unlock window with `cpsid i`,
 * which does NOT mask NMI, so an NMI could land between R8_FLASH_PROTECT = 0x88
 * and the re-lock.  Ten instructions delete the question; do not replace them
 * with a datasheet reading.
 *
 * Written UNCONDITIONALLY, with no build knob: the entire point is to stop
 * depending on an untested assumption, and a knob is a way to reintroduce one.
 *
 * 0x01 leaves RB_BAT_DET_EN SET on purpose — the detector stays available for a
 * polled supply gate via R8_BAT_STATUS (0x40001026, RO, no SAM unlock needed),
 * which is the NMI-free equivalent of WCH's V1.1 flash-write voltage gate.  The
 * reference needs ~1 us to settle after DET_EN (DS p.20); the .data/.bss init
 * below provides far more.  R8_BAT_DET_CFG is written 0x01 in the same window:
 * RB_BAT_LOW_VTH = 01, i.e. 2.05 V ultra-low / 2.33 V low.  That is both the
 * hardware reset value and what the application writes, so it changes nothing —
 * it is here so the threshold pair is stated rather than assumed.
 *
 * BYTE stores, never `str`: R8_BAT_DET_CTRL (+0) and R8_BAT_DET_CFG (+1) share
 * the word at 0x40001024 with R8_BAT_STATUS at +2, which is read-only.  Same
 * discipline the clock write below applies at 0x40001008.
 *
 * Its OWN safe-access window, not shared with the clock write: the window is
 * only ~16 Tsys (DS §4.2) and an overrun silently DROPS the write, so a shared
 * window that overran would lose whichever store came last.  Every address and
 * value is in a register before the 0xA8 store, and nothing is allowed between
 * the unlock pair and the protected stores.
 */
    ldr     r0, =R8_SAFE_ACCESS_SIG     /* r0 = 0x40001040                    */
    ldr     r1, =R8_BAT_DET_CTRL        /* r1 = 0x40001024                    */
    movs    r2, #SAFE_ACCESS_SIG1       /* 0x57                               */
    movs    r3, #SAFE_ACCESS_SIG2       /* 0xA8                               */
    movs    r4, #BAT_DET_EN_ONLY        /* 0x01 — same value for both stores  */
    strb    r2, [r0]                    /* unlock 1 of 2                      */
    strb    r3, [r0]                    /* unlock 2 of 2 — window opens here  */
    strb    r4, [r1, #1]                /* R8_BAT_DET_CFG  = 0x01 (threshold) */
    strb    r4, [r1]                    /* R8_BAT_DET_CTRL = 0x01 — NMI GONE  */
    movs    r2, #0
    strb    r2, [r0]                    /* close safe access early            */

/* ------------------------------------------------------------------------- */
/* 2. Clock                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * DECISION: write the clock configuration, and write exactly the value the
 * shipping application's SystemInit writes.
 *
 *  - Doing nothing leaves Fsys at the reset default, which is NOT 32 MHz (the
 *    application's own NMI clock-fail handler falls back to 0x0005 = mode 0,
 *    div 5 = 6.4 MHz, the shape of the low-speed default).  Update mode needs
 *    32 MHz for USB, so the write has to happen sooner or later, and anywhere
 *    other than here means a second, later clock switch.
 *
 *  - The risk is bounded.  R16_CLK_SYS_CFG is a safe-access (SAM) register: if
 *    the 0x57/0xA8 unlock window is missed the write is simply IGNORED.  The
 *    failure mode is "clock stays at the default", not corruption.
 *
 *  - The value 0x0088 is PLL_DIV = 8, SYS_MOD = 2 ("directly from 32 MHz"), and
 *    critically RB_CLK_OSC32M_XT = 0 — the INTERNAL RC oscillator.  The crystal
 *    selection bit is not touched (hard constraint 6): some boards have no
 *    crystal fitted, and the internal RC is proven on this device to drive USB
 *    CDC at 2 Mbaud.
 *
 *  - A wrong clock cannot brick the part: the ROM ISP at H1 runs before any of
 *    our code and configures its own clock, and reset restores the register.
 *
 *  - FLAGGED DISCREPANCY, not silently absorbed: the live device reads
 *    R16_CLK_SYS_CFG = 0x8088, while the application's SystemInit writes
 *    0x0088.  Bit 15 is unaccounted for in the register decode (the three PON
 *    bits are in R8_HFCK_PWR_CTRL = 0x1C, a different register).  The most
 *    likely reading is that bit 15 is a hardware status/ready bit that reads
 *    back set and is not software-written; writing 0x0088 then reproduces
 *    byte-for-byte what the known-good application does, which is the highest-
 *    confidence option available.  If stage 2 shows the clock behaving
 *    differently from the application's, this is the first thing to re-measure.
 *
 * The two signature writes and the target write must be back to back — the SAM
 * window is only a few CPU cycles wide — so every address and value is loaded
 * into a register FIRST and no instruction is allowed between them.
 *
 * Only the 16-bit half at 0x40001008 is written.  R8_HFCK_PWR_CTRL lives at
 * 0x4000100A in the same word and must not be disturbed, so this is a `strh`
 * and never a `str`.
 */
#if BL_INIT_CLOCK
    ldr     r0, =R8_SAFE_ACCESS_SIG     /* r0 = 0x40001040                    */
    ldr     r1, =R16_CLK_SYS_CFG        /* r1 = 0x40001008                    */
    movs    r2, #SAFE_ACCESS_SIG1       /* 0x57                               */
    movs    r3, #SAFE_ACCESS_SIG2       /* 0xA8                               */
    movs    r4, #CLK_SYS_CFG_VALUE      /* 0x88 -> half-word 0x0088           */
    strb    r2, [r0]                    /* unlock 1 of 2                      */
    strb    r3, [r0]                    /* unlock 2 of 2 — window opens here  */
    strh    r4, [r1]                    /* ...and the protected write is next */
    movs    r2, #0
    strb    r2, [r0]                    /* re-lock safe access                */
#endif

/* ------------------------------------------------------------------------- */
/* 3. Copy .data from its flash load address to RAM                          */
/* ------------------------------------------------------------------------- */
/* Both ends are 4-aligned and the size is a multiple of 4 — the linker script
 * guarantees it with ALIGN(4) at both boundaries — so a word loop is safe. */
    ldr     r1, =__data_load
    ldr     r2, =__data_start
    ldr     r3, =__data_end
.Ldata_loop:
    cmp     r2, r3
    bhs     .Ldata_done
    ldr     r0, [r1]
    str     r0, [r2]
    adds    r1, r1, #4
    adds    r2, r2, #4
    b       .Ldata_loop
.Ldata_done:

/* ------------------------------------------------------------------------- */
/* 4. Zero .bss                                                              */
/* ------------------------------------------------------------------------- */
/*
 * CRITICAL: this loop must never reach 0x20000090, which carries the
 * 0xAA55BB01 update-request magic across SYSRESETREQ.  If it were cleared, the
 * update request from the running application would be lost and the "hold U22
 * for a second to enter update mode" path would silently stop working.
 *
 * That is guaranteed structurally, not by inspection: ld/bootloader.ld places a
 * NOLOAD .noinit section covering the whole reserved hole 0x20000000..0x2000009F
 * as the FIRST thing in RAM, so __bss_start can never be below 0x200000A0, and
 * the script carries a link-time ASSERT to that effect:
 *
 *     ASSERT(__bss_start >= 0x200000A0, "...")
 *
 * The layout mirrors the application's own UNINIT region exactly, so neither
 * side of the handshake ever writes the other's hole.
 */
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    movs    r0, #0
.Lbss_loop:
    cmp     r1, r2
    bhs     .Lbss_done
    str     r0, [r1]
    adds    r1, r1, #4
    b       .Lbss_loop
.Lbss_done:

/* ------------------------------------------------------------------------- */
/* 5. Into C.  bl_main() owns the boot decision and never returns.           */
/* ------------------------------------------------------------------------- */
    bl      bl_main
.Lmain_returned:
    b       .Lmain_returned         /* bl_main() must not return; spin if it does */

    .ltorg
    .size   Reset_Handler, . - Reset_Handler

/* NOTE, deliberately absent: no local stub for bl_main(), weak or otherwise.
 * src/boot.c is the only definition.  A weak fallback here would let a build
 * link successfully even if boot.c stopped defining bl_main under that exact
 * name, and the device would then ship a bootloader that spins forever at
 * reset.  An unresolved symbol is the correct outcome in that case. */
