/* bootloader.ld — GBFlash CH579M update-mode bootloader
 *
 * OWNED BY: comp:vectors+ld
 *
 * Layout, from BOOTLOADER.md §5.2.2 (all boundaries are exact multiples of the
 * 512-byte erase sector, verified):
 *
 *   0x00000000 .. 0x00003DFF   bootloader image      sectors  0..30  15,872 B
 *   0x00003E00 .. 0x00003FFF   boot-info record      sector  31        512 B
 *   0x00004000 .. 0x0003E7FF   application           sectors 32..
 *
 *   0x20000000 .. 0x2000009F   NO-INIT hole, 160 B — never copied, never
 *                              zeroed, survives SYSRESETREQ.  Holds the
 *                              0xAA55BB01 update-request magic at 0x20000090.
 *   0x200000A0 .. 0x20007FFF   .data then .bss, then the stack growing down
 *                              from 0x20008000.
 *
 * The NO-INIT hole is reproduced byte-for-byte from the application's own
 * scatter layout (re/startup.md §7).  It is the reason .bss can never reach
 * 0x20000090 — and there is a hard ASSERT below that proves it at link time.
 */

OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 15872      /* 0x0000..0x3DFF */
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32768      /* 32 KB          */
}

/* ---- absolute constants exported to C and assembly ---------------------- */

/* Only constants that C or assembly actually reference are given symbols, and
 * every one of them has a VALUE outside this image's 0x0000..0x3DFF address
 * range.  That restriction is deliberate: a link-script assignment emits an ABS
 * symbol, and objdump labels any instruction whose ADDRESS equals that VALUE
 * with the symbol's name.  A small constant like 0xA0 would therefore appear as
 * a bogus label in the middle of the vector table, and on this project the
 * disassembly is the review artifact.  Small constants stay literal.
 *
 * The rule is applied strictly, so the list is short.  The boot-info base
 * (0x3E00) and the update-request magic word (0x20000090) used to be exported
 * here too and were referenced by nothing: C takes them from include/boot.h as
 * BL_BOOTINFO_BASE and BL_UPDATE_MAGIC_ADDR, which is also where they can be
 * overridden for the native host build.  Two names for one address is how the
 * two drift apart. */

__stack_top     = 0x20008000;        /* vector[0]; top of SRAM               */
__app_base      = 0x00004000;        /* application vector table — the value
                                      * src/vectors.S hardcodes in its literal
                                      * pool and names in the comment there  */

SECTIONS
{
  /* ---------------------------------------------------------------------- */
  /* .vectors — MUST be first, MUST be at 0x00000000                        */
  /* ---------------------------------------------------------------------- */
  .vectors 0x00000000 : ALIGN(4)
  {
    __vectors_start = .;
    KEEP(*(.vectors))
    KEEP(*(.vectors.*))
    __vectors_end = .;
  } > FLASH

  ASSERT(__vectors_start == 0x00000000,
         "bootloader.ld: .vectors must start at 0x00000000")
  ASSERT(__vectors_end - __vectors_start == 0x90,
         "bootloader.ld: vector table must be exactly 0x90 bytes (36 entries)")

  /* ---------------------------------------------------------------------- */
  /* .text                                                                  */
  /* ---------------------------------------------------------------------- */
  .text : ALIGN(4)
  {
    __text_start = .;
    /* the shared trampoline first, so it stays close to 0 and easy to eyeball
       in a disassembly */
    KEEP(*(.text.bl_vector_forward))
    *(.text.Reset_Handler)
    *(.text .text.*)
    *(.glue_7)
    *(.glue_7t)
    . = ALIGN(4);
    __text_end = .;
  } > FLASH

  /* ---------------------------------------------------------------------- */
  /* .rodata                                                                */
  /* ---------------------------------------------------------------------- */
  .rodata : ALIGN(4)
  {
    __rodata_start = .;
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    . = ALIGN(4);
    __rodata_end = .;
  } > FLASH

  /* Unwind tables.  Bare-metal builds should compile with -fno-unwind-tables
     and -fno-asynchronous-unwind-tables, in which case these are empty; they
     are kept rather than discarded so a stray entry is a size error, not a
     silent loss. */
  .ARM.extab : ALIGN(4) { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
  .ARM.exidx : ALIGN(4)
  {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } > FLASH

  /* ---------------------------------------------------------------------- */
  /* RAM: the no-init hole comes first and is reserved in full               */
  /* ---------------------------------------------------------------------- */
  .noinit (NOLOAD) : ALIGN(4)
  {
    __noinit_start = .;
    KEEP(*(.noinit))
    KEEP(*(.noinit.*))
    /* Reserve the whole 0xA0 hole whether or not anything was placed in it.
       This is what keeps .data/.bss above 0x200000A0 and therefore keeps the
       .bss zeroing loop away from the 0x20000090 magic word. */
    . = __noinit_start + 0xA0;
    __noinit_end = .;
  } > RAM

  ASSERT(__noinit_start == 0x20000000,
         "bootloader.ld: the no-init hole must start at 0x20000000")
  ASSERT(__noinit_end == 0x200000A0,
         "bootloader.ld: the no-init hole must end at 0x200000A0")

  /* ---------------------------------------------------------------------- */
  /* .data — runs in RAM, loads from FLASH                                  */
  /* ---------------------------------------------------------------------- */
  .data : ALIGN(4)
  {
    __data_start = .;
    *(.data .data.*)
    *(.sdata .sdata.*)
    . = ALIGN(4);
    __data_end = .;
  } > RAM AT> FLASH

  __data_load = LOADADDR(.data);

  /* ---------------------------------------------------------------------- */
  /* .bss                                                                   */
  /* ---------------------------------------------------------------------- */
  .bss (NOLOAD) : ALIGN(4)
  {
    __bss_start = .;
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM

  /* ---------------------------------------------------------------------- */
  /* size and safety assertions                                             */
  /* ---------------------------------------------------------------------- */

  /* The image is based at 0, so its end address is also its size; there is no
     separate __flash_image_size symbol for exactly that reason. */
  __flash_image_end  = __data_load + SIZEOF(.data);

  /* HARD CONSTRAINT 1: the whole image must fit below the boot-info record. */
  ASSERT(__flash_image_end <= 15872,
         "bootloader.ld: image exceeds 15872 bytes (0x3E00) and would collide with the boot-info record at 0x3E00")

  /* CRITICAL: the .bss zeroing loop must never touch the update-request magic
     at 0x20000090.  .data and .bss both live above the reserved no-init hole,
     so this can only fail if someone edits the hole away. */
  ASSERT(__data_start >= 0x200000A0,
         "bootloader.ld: .data must start at or above 0x200000A0 — it would otherwise overlap the 0x20000090 update magic")
  ASSERT(__bss_start >= 0x200000A0,
         "bootloader.ld: .bss must start at or above 0x200000A0 — the zeroing loop would otherwise destroy the 0x20000090 update magic")

  /* Leave a full 4 KB for the stack. */
  __stack_limit = __stack_top - 0x1000;
  ASSERT(__bss_end <= __stack_limit,
         "bootloader.ld: RAM overflow — .bss collides with the reserved stack")

  /DISCARD/ :
  {
    *(.ARM.attributes)
    *(.comment)
    *(.note .note.*)
    *(.eh_frame*)
  }
}
