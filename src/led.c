/* led.c — update-mode activity LED on PB12.
 *
 * Owner: comp:led.  Interface and the three rules: include/led.h — read it.
 * Design authority: docs/DESIGN.md §7, and include/led.h's three rules.
 *
 * ---------------------------------------------------------------------------
 * The hardware, from the disassembly of the shipping application
 * ---------------------------------------------------------------------------
 * PB12, output push-pull 5 mA, ACTIVE LOW.  CONFIRMED by an exhaustive scan
 * of every site in the application that builds the 0x1000 mask:
 *
 *   0x5CB0 bsp_gpio_init  PB_CLR<-PB12 ; ModeCfg(PB12, Out_PP_5mA) ; PB_OUT<-PB12
 *   0xA6C0 main           PB_CLR<-PB12 after the startup banner   -> app ready
 *   0x70D0 / 0xA56A       PB_CLR<-PB12 when a command starts/ends -> busy
 *   0x72A2 / 0x72D6       PB_OUT<-PB12 when idle >= 10 ms         -> idle
 *   0x5B9A cart_set_speed 100 ms HIGH/LOW pulse trains            -> indicator
 *
 * LOW = lit, HIGH = dark.  GPIOB_ModeCfg mode 3 (GPIO_ModeOut_PP_5mA) is
 * exactly `PD_DRV &= ~m; DIR |= m` — decoded from the jump table at 0x435A.
 * It does not touch PB_PU, and neither do we.
 *
 * Register semantics, and why the two writes are spelled differently:
 *   R32_PB_OUT (0x400010C8) is the real output latch.  The application reads it
 *     (0x5B9A `ldr r0,[r4,#8]`) and writes it back, so it must be read-modify-
 *     written or the other port-B output bits would be destroyed.
 *   R32_PB_CLR (0x400010CC) is write-1-to-clear.  A plain store of the mask is
 *     the WCH SDK's GPIOB_ResetBits and disturbs nothing else.  The application
 *     spells it as a read-modify-write, which is only self-consistent if the
 *     register reads back as zero — under that model the two spellings are the
 *     same store, and the plain store is the one that stays correct if it does
 *     not.  So: plain store.
 *
 * Nothing else in the bootloader ever writes port B except bl_button_init()
 * (PB23: PD_DRV, PU, DIR), so the read-modify-writes here cannot race anything.
 * PB22 — the application's PCB-revision strap, and believed to be the H1/ISP
 * boot pad — is never read or written from this file.
 *
 * ---------------------------------------------------------------------------
 * Timing
 * ---------------------------------------------------------------------------
 * Real milliseconds, from bl_time_ms() (include/timebase.h) — a free-running
 * SysTick counter with its interrupt DISABLED, sampled by polling, so no vector
 * is used and no NVIC line is enabled.  See the TIMEBASE section of led.h.
 * Nothing here blocks, delays or spins.
 */

/* boot.h and bl_config.h cannot both be included as-is: they define the port-B
 * register addresses, BL_SRAM_BASE and friends with identical values but
 * different spellings ("0x400010C8" vs "0x400010C8u"), which is a macro
 * redefinition.  host/check_headers.c hits the same wall and solves it the same
 * way.  boot.h comes first because bl_reason_t is the thing that must not be
 * duplicated; the addresses are then re-taken from bl_config.h, and the
 * build-time assertions below prove the two headers agreed before the #undef. */
#include "boot.h"

enum {
    LED_CHK_PB_DIR    = (int)BL_R32_PB_DIR,
    LED_CHK_PB_OUT    = (int)BL_R32_PB_OUT,
    LED_CHK_PB_CLR    = (int)BL_R32_PB_CLR,
    LED_CHK_PB_PD_DRV = (int)BL_R32_PB_PD_DRV
};

#undef BL_R32_PB_DIR
#undef BL_R32_PB_PIN
#undef BL_R32_PB_OUT
#undef BL_R32_PB_CLR
#undef BL_R32_PB_PU
#undef BL_R32_PB_PD_DRV
#undef BL_BTN_MASK
#undef BL_SRAM_BASE
#undef BL_APP_BASE
#undef BL_BOOTINFO_BASE
#undef BL_BOOTINFO_LEN
#undef BL_CODEFLASH_END

#include "bl_config.h"
#include "led.h"
#include "timebase.h"

/* ------------------------------------------------------------------------- */
/* Build-time checks.  These cost nothing in the image.                       */
/* ------------------------------------------------------------------------- */

_Static_assert(LED_CHK_PB_DIR    == (int)BL_R32_PB_DIR,    "PB_DIR disagrees");
_Static_assert(LED_CHK_PB_OUT    == (int)BL_R32_PB_OUT,    "PB_OUT disagrees");
_Static_assert(LED_CHK_PB_CLR    == (int)BL_R32_PB_CLR,    "PB_CLR disagrees");
_Static_assert(LED_CHK_PB_PD_DRV == (int)BL_R32_PB_PD_DRV, "PD_DRV disagrees");

_Static_assert(BL_LED_PIN == 12, "the activity LED is PB12");
_Static_assert(BL_LED_MASK == (1u << BL_LED_PIN), "LED mask/pin disagree");

/* The pattern word is 16 bits, one bit per slot, and led_slot wraps with a
 * mask — so the slot count must be 16 and must stay a power of two. */
_Static_assert(BL_LED_SLOTS == 16u, "pattern words are 16 bits wide");

/* A zero-length slot would advance the pattern on every poll.  Nonsense, so
 * reject it at compile time. */
_Static_assert(BL_LED_SLOT_MS >= 1u, "BL_LED_SLOT_MS must be >= 1");

/* A scale check, not a safety property: bl_led_poll() samples bl_time_ms() on
 * EVERY call regardless of the slot length, so it always keeps the timebase's
 * 24-bit counter unambiguous by itself.  But a slot longer than one lap of that
 * counter (BL_TIME_MAX_GAP_MS, 524 ms) would mean the LED was measuring an
 * interval the clock cannot express in one step, which is a sign the pattern
 * has been retuned into a different design rather than adjusted. */
_Static_assert(BL_LED_SLOT_MS < BL_TIME_MAX_GAP_MS,
               "a pattern slot must be shorter than one lap of the timebase");

/* The idiom has to stay recognisable as "keep blinking twice": a period of a
 * couple of seconds, not six. */
_Static_assert(BL_LED_PERIOD_MS >= 1000u && BL_LED_PERIOD_MS <= 2500u,
               "the pattern period must read as a heartbeat, ~1.6 s");

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

/* Zero-initialised in .bss by start.S.  led_active == 0 is what makes every
 * entry point inert until bl_led_init() has run, which is rule 1 of led.h:
 * nothing here can touch PB12 on the application-handoff path. */
static uint8_t  led_active;     /* 0 = PB12 untouched, all entry points inert */
static uint8_t  led_slot;       /* 0..15, index of the slot being played      */
static uint16_t led_pattern;    /* bit per slot, bit 0 played first           */
static uint32_t led_slot_ms;    /* bl_time_ms() when the current slot started */

/* ------------------------------------------------------------------------- */
/* Pin                                                                        */
/* ------------------------------------------------------------------------- */

/* ACTIVE LOW: driving the pin low lights the LED. */
static void led_drive(unsigned int lit)
{
    if (lit != 0u) {
        /* PB_CLR is write-1-to-clear; a plain store touches only PB12. */
        BL_REG32(BL_R32_PB_CLR) = BL_LED_MASK;
    } else {
        /* PB_OUT is the real latch; read-modify-write, as the application. */
        BL_REG32(BL_R32_PB_OUT) = BL_REG32(BL_R32_PB_OUT) | BL_LED_MASK;
    }
}

/* Apply the slot the state machine is currently on. */
static void led_apply(void)
{
    led_drive((unsigned int)((led_pattern >> led_slot) & 1u));
}

/* ------------------------------------------------------------------------- */
/* Interface                                                                  */
/* ------------------------------------------------------------------------- */

void bl_led_init(void)
{
    /* The clock this module blinks off.  Idempotent and it does NOT restart an
     * epoch already running, so calling it here cannot disturb bl_update_mode()
     * if the update loop started the timebase first — and starting it here
     * means the LED blinks even if some future caller forgets.  SysTick only:
     * no GPIO, no interrupt, none of the three rules in led.h is at risk. */
    bl_time_init();

    /* RULE 2 — preset the latch DARK while PB12 is still an input.  Doing this
     * after the direction change drives whatever the latch held and produces a
     * visible flash on entry to update mode.  Active low, so dark = HIGH. */
    BL_REG32(BL_R32_PB_OUT) = BL_REG32(BL_R32_PB_OUT) | BL_LED_MASK;

    /* GPIOB_ModeCfg(BL_LED_MASK, GPIO_ModeOut_PP_5mA) — mode 3, which is
     * exactly these two writes in this order.  PB_PU is not
     * touched by mode 3 and is not touched here. */
    BL_REG32(BL_R32_PB_PD_DRV) = BL_REG32(BL_R32_PB_PD_DRV) & ~(uint32_t)BL_LED_MASK;
    BL_REG32(BL_R32_PB_DIR)    = BL_REG32(BL_R32_PB_DIR)    |  (uint32_t)BL_LED_MASK;

    /* Arm the state machine.  If bl_led_set_pattern() has not been called yet,
     * led_pattern is still 0 (dark forever), so fall back to the "no reason
     * recorded" pattern rather than looking like a dead board. */
    if (led_pattern == 0u) {
        led_pattern = BL_LED_PAT_SLOW;
    }
    led_slot    = 0u;
    led_slot_ms = bl_time_ms(); /* slot 0 starts now */
    led_active  = 1u;

    led_apply();                /* show slot 0 immediately */
}

void bl_led_set_pattern(uint8_t reason)
{
    /* Indexed by bl_reason_t.  Entry 0 covers BL_REASON_NONE, which should
     * never reach update mode, and doubles as the out-of-range fallback. */
    static const uint16_t table[4] = {
        BL_LED_PAT_SLOW,        /* BL_REASON_NONE        — should not happen  */
        BL_LED_PAT_DOUBLE,      /* BL_REASON_MAGIC       — host asked for it  */
        BL_LED_PAT_TRIPLE,      /* BL_REASON_BUTTON      — U22 held at power-on */
        BL_LED_PAT_FAST         /* BL_REASON_APP_INVALID — nothing to boot    */
    };

    _Static_assert((int)BL_REASON_NONE        == 0, "table order vs bl_reason_t");
    _Static_assert((int)BL_REASON_MAGIC       == 1, "table order vs bl_reason_t");
    _Static_assert((int)BL_REASON_BUTTON      == 2, "table order vs bl_reason_t");
    _Static_assert((int)BL_REASON_APP_INVALID == 3, "table order vs bl_reason_t");

    if (reason >= (uint8_t)(sizeof table / sizeof table[0])) {
        reason = (uint8_t)BL_REASON_NONE;
    }
    led_pattern = table[reason];

    /* Restart the pattern so the first blink is immediate and a pattern change
     * can never be read as a half of one pattern followed by half of another.
     *
     * bl_time_ms() is safe to call before bl_led_init() / bl_time_init(): with
     * the clock stopped it returns a frozen value, and bl_led_init() re-stamps
     * led_slot_ms after starting the clock, so the pre-init case cannot leave a
     * stale timestamp behind. */
    led_slot    = 0u;
    led_slot_ms = bl_time_ms();

    /* Rule 1: no GPIO access unless bl_led_init() has already claimed PB12.
     * This makes the two calls order-independent — bl_led_init() applies
     * whatever pattern is stored. */
    if (led_active != 0u) {
        led_apply();
    }
}

void bl_led_poll(void)
{
    uint32_t now;

    /* RULE 3 — NON-BLOCKING.  Every path out of this function is a handful of
     * instructions.  No delay, no busy-wait, no loop.  Do not add one: the
     * caller is the loop that services USB, and stalling it drops host
     * transactions and can strand a firmware transfer mid-flash.  bl_time_ms()
     * is a counter read and obeys the same rule. */

    if (led_active == 0u) {
        return;                 /* PB12 not ours — see rule 1 */
    }

    now = bl_time_ms();

    /* Unsigned difference, so this stays correct across the 49.7-day wrap of
     * bl_time_ms() and cannot be tricked by comparing absolute values. */
    if ((uint32_t)(now - led_slot_ms) < BL_LED_SLOT_MS) {
        return;
    }

    /* Stamp `now` rather than adding a slot's worth to the old stamp.  Adding
     * would preserve phase across a stall and then fire on several consecutive
     * polls to catch up — a visible flicker after every long flash write.  This
     * advances AT MOST ONE SLOT per call, which is what led.h promises: a busy
     * loop stretches the pattern by the length of the stall and no more.  The
     * drift it costs is the overshoot past the boundary, tens of microseconds
     * per slot on the idle path. */
    led_slot_ms = now;
    led_slot    = (uint8_t)((led_slot + 1u) & (BL_LED_SLOTS - 1u));
    led_apply();
}
