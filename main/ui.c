#include "ui.h"

#include "boot_mode.h"
#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "ui_storm.h"
#include "ui_landscape.h"
#include "ui_test.h"
#include "ui_zoo.h"

static const char *TAG = "ui";

/* Which screen this build shows. Kept out of ui_init() so that bringing the
 * LVGL port up and choosing what to draw on it stay separate concerns — and so
 * the preprocessor branch is not sitting inside the lock. */
static void build_active_screen(void)
{
#if BOOT_MODE == BOOT_MODE_TEST_SCREEN
    ui_test_screen_build();
#elif BOOT_MODE == BOOT_MODE_ZOO
    ui_zoo_screen_build();
#elif BOOT_MODE == BOOT_MODE_STORM
    ui_storm_screen_build();
#elif BOOT_MODE == BOOT_MODE_LANDSCAPE
    ui_landscape_screen_build();
#else
/* Named rather than defaulted. A bare #else meant a new BOOT_MODE compiled
 * cleanly into whichever screen happened to be last. */
#error "BOOT_MODE does not name a screen this file knows how to build"
#endif
}

/* Rows held in each draw buffer. LVGL renders a slice at a time and flushes it,
 * so this trades RAM against the number of SPI transactions per frame; it does
 * not have to cover the screen.
 *
 * 20 rows double-buffered is 9.6 kB each, 19.2 kB total, of DMA-capable
 * internal DRAM. Doubling to 40 rows halves the flush count for a full-screen
 * refresh (12 -> 6) but each extra flush costs only tens of microseconds
 * against ~23 ms of SPI wire time, so it buys under 1% for another 19 kB —
 * a bad trade on a board that still has to fit Wi-Fi.
 *
 * These stay in internal DRAM even once PSRAM is enabled: LVGL's software
 * renderer does per-pixel read-modify-write here, and WROVER PSRAM is roughly
 * an order of magnitude slower, which would push render time past flush time.
 *
 * The row count and the resulting pixel count both come from display.h, so the
 * SPI bus transfer limit is sized from the same numbers rather than from a
 * second expression that has to agree with them by eye. */

esp_err_t ui_init(void)
{
    /* Asks the display whether it is usable rather than inferring it from a
     * non-NULL panel handle: the handle exists from the moment the object is
     * constructed, which is several failable steps before the panel is
     * configured. Equivalent today only because display_init()'s unwind nulls
     * the handle on every failure path — an invariant this has no business
     * depending on. */
    ESP_RETURN_ON_FALSE(display_ready(), ESP_ERR_INVALID_STATE,
                        TAG, "display not initialised");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* The default 5 ms tick wakes the LVGL task 200x/s, but LV_DEF_REFR_PERIOD
     * caps output at ~30 fps, so most of those wakeups walk the timer list and
     * go straight back to blocked. Animations interpolate on elapsed
     * milliseconds, not tick count, so a coarser tick costs no smoothness. */
    port_cfg.timer_period_ms = 10;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = display_io_handle(),
        .panel_handle = display_panel_handle(),
        .buffer_size  = DISPLAY_DRAW_BUF_PX,
        .double_buffer = true,
        .hres         = DISPLAY_WIDTH,
        .vres         = DISPLAY_HEIGHT,
        .monochrome   = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* Same constants display_init() applies. esp_lvgl_port mirrors the
         * panel itself when it attaches, so leaving these zeroed would silently
         * undo the driver's orientation rather than inherit it. */
        .rotation = {
            .swap_xy  = false,
            .mirror_x = PANEL_MIRROR_X,
            .mirror_y = PANEL_MIRROR_Y,
        },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = PANEL_SWAP_BYTES,
        },
    };

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp");

    /* LVGL is not thread safe and now has its own task. Every call into it from
     * elsewhere, including this one, has to hold the port lock. */
    /* A 0 timeout means wait forever in esp_lvgl_port, which would make the
     * failure branch unreachable and hang boot on a stuck lock. Bounded
     * instead: nothing else holds this yet, so a second is already generous. */
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "could not take lvgl lock");
        return ESP_ERR_TIMEOUT;
    }
    build_active_screen();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "lvgl running (%dx%d, %d-row buffers)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_DRAW_ROWS);
    return ESP_OK;
}
