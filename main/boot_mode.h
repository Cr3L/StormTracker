#pragma once

/* What the board brings up at boot.
 *
 * These are rungs of one ladder, not independent switches: each adds a layer to
 * the one below it, so working down the ladder is how you find which layer owns
 * a fault. The self-test drives the panel directly and never touches LVGL; the
 * test screen brings LVGL up with no assets; the zoo adds the sprite tables.
 *
 * One ordered value rather than a boolean here and another in ui.c. They were
 * never two decisions — a build cannot run the self-test *and* a screen — and
 * two booleans in two files make that meaningless combination representable,
 * while forcing anyone asking "what will this build show?" to read both.
 *
 * Deliberately a #define and not a Kconfig choice. Kconfig would move the
 * setting into sdkconfig, which is generated, gitignored, and already the
 * subject of a warning in CLAUDE.md about silently winning over
 * sdkconfig.defaults. Switching modes means editing source either way, so the
 * visible constant is worth more than the menu.
 */
#define BOOT_MODE_SELFTEST    0
#define BOOT_MODE_TEST_SCREEN 1
#define BOOT_MODE_ZOO         2
#define BOOT_MODE_STORM       3
#define BOOT_MODE_LANDSCAPE   4

/* STORM is a peer of ZOO, not its successor, and the ladder metaphor stops
 * being exact here — worth stating rather than papering over, because the
 * ladder's value is that stepping down changes one thing.
 *
 * ZOO adds sprites to LVGL. STORM adds the network to LVGL and uses no sprites
 * at all, so STORM -> ZOO swaps two variables at once and does not bisect
 * anything. To isolate a fault under the storm screen, step to TEST_SCREEN,
 * which is genuinely below both: LVGL with no assets and no network. */
/* The landscape and original storm dial share the same weather service. */
#define BOOT_MODE BOOT_MODE_LANDSCAPE
