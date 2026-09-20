#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "sun.h"
#include "time_sync.h"
#include "ui_landscape.h"
#include "weather.h"

static time_t s_now = 1780401600;
static time_t s_rise;
static time_t s_set;
static bool s_synced = true;
static bool s_location = true;
static weather_report_t s_forecast;
static weather_obs_t s_observation;
static weather_alert_t s_alert;
static uint16_t s_frame[240 * 240];
static uint16_t s_before[240 * 240];
static uint16_t s_draw[240 * 20];
static unsigned s_flushes;

time_t time(time_t *out)
{
    if (out != NULL) *out = s_now;
    return s_now;
}

bool time_sync_synced(void) { return s_synced; }
void weather_report_copy(weather_report_t *out) { *out = s_forecast; }
void weather_obs_copy(weather_obs_t *out) { *out = s_observation; }
void weather_alert_copy(weather_alert_t *out) { *out = s_alert; }

esp_err_t weather_location_get(double *latitude, double *longitude)
{
    *latitude = 40;
    *longitude = -75;
    return s_location ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t sun_times(double latitude, double longitude, time_t when,
                    time_t *rise, time_t *set)
{
    (void)latitude;
    (void)longitude;
    (void)when;
    *rise = s_rise;
    *set = s_set;
    return ESP_OK;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *map)
{
    const uint16_t *pixels = (const uint16_t *)map;
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < 240 && y >= 0 && y < 240);
            s_frame[y * 240 + x] = *pixels++;
        }
    }
    ++s_flushes;
    lv_display_flush_ready(display);
}

static void advance(unsigned milliseconds)
{
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 25) {
        lv_tick_inc(25);
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}

static bool has_caption(const char *text)
{
    lv_obj_t *screen = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) &&
            strcmp(lv_label_get_text(child), text) == 0) return true;
    }
    return false;
}

static void save(const char *scenario, const char *suffix)
{
    char path[96];
    snprintf(path, sizeof(path), "%s%s.ppm", scenario, suffix);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fprintf(file, "P6\n240 240\n255\n");
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 240; ++x) {
            uint16_t pixel = s_frame[y * 240 + x];
            const int dx = 2 * x - 239, dy = 2 * y - 239;
            if (dx * dx + dy * dy > 240 * 240) pixel = 0;
            const unsigned char rgb[] = {
                (unsigned char)(((pixel >> 11) & 31) * 255 / 31),
                (unsigned char)(((pixel >> 5) & 63) * 255 / 63),
                (unsigned char)((pixel & 31) * 255 / 31),
            };
            assert(fwrite(rgb, 1, 3, file) == 3);
        }
    }
    assert(fclose(file) == 0);
}

static void scenario(const char *name)
{
    if (strcmp(name, "dusk") == 0) s_now += 7 * 3600;
    if (strcmp(name, "night") == 0) s_now += 10 * 3600;
    s_rise = s_now - 6 * 3600;
    s_set = s_now + 6 * 3600;
    s_observation.observed = s_now - 15 * 60;
    s_observation.temperature = 74;
    s_observation.wind = 8;
    snprintf(s_observation.text, sizeof(s_observation.text), "Partly Cloudy");
    snprintf(s_forecast.now, sizeof(s_forecast.now), "Chance Rain Showers");
    s_forecast.fetched = s_now - 60;
    s_forecast.forecast_start = s_now - 3600;
    s_forecast.forecast_end = s_now + 3600;

    if (strcmp(name, "dusk") == 0) s_set = s_now + 10 * 60;
    if (strcmp(name, "night") == 0) {
        s_rise = s_now + 6 * 3600;
        s_set = s_now + 18 * 3600;
        snprintf(s_observation.text, sizeof(s_observation.text), "Clear");
    }
    if (strcmp(name, "cloudy") == 0) snprintf(s_observation.text, sizeof(s_observation.text), "Overcast");
    if (strcmp(name, "rain") == 0) snprintf(s_observation.text, sizeof(s_observation.text), "Light Rain");
    if (strcmp(name, "snow") == 0) {
        snprintf(s_observation.text, sizeof(s_observation.text), "Snow");
        s_observation.temperature = 28;
    }
    if (strcmp(name, "storm") == 0) snprintf(s_observation.text, sizeof(s_observation.text), "Thunderstorm");
    if (strcmp(name, "fog") == 0) snprintf(s_observation.text, sizeof(s_observation.text), "Fog");
    if (strcmp(name, "forecast") == 0) s_observation.observed = 0;
    if (strcmp(name, "missing-temperature") == 0) s_observation.temperature = WEATHER_UNKNOWN;
    if (strcmp(name, "stale") == 0 || strcmp(name, "recovery") == 0) {
        s_observation.observed -= 3 * 3600;
        s_forecast.forecast_end = s_now - 1;
    }
    if (strcmp(name, "waiting") == 0) {
        s_synced = false;
        s_location = false;
        s_observation.observed = 0;
        s_forecast.fetched = 0;
    }
    if (strcmp(name, "watch") == 0 || strcmp(name, "long-watch") == 0) {
        s_alert.level = WEATHER_ALERT_WATCH;
        s_alert.checked = s_now;
        snprintf(s_alert.event, sizeof(s_alert.event), "Severe Thunderstorm Watch");
        if (strcmp(name, "long-watch") == 0)
            snprintf(s_alert.event, sizeof(s_alert.event), "Severe Thunderstorm Watch - Extended Coverage");
    }
    if (strcmp(name, "warning") == 0 || strcmp(name, "stale-warning") == 0 ||
        strcmp(name, "long-warning") == 0) {
        s_alert.level = WEATHER_ALERT_WARNING;
        s_alert.severe = true;
        s_alert.checked = s_now - (strcmp(name, "stale-warning") == 0 ? 3600 : 0);
        snprintf(s_alert.event, sizeof(s_alert.event), "Severe Thunderstorm Warning");
        if (strcmp(name, "long-warning") == 0)
            snprintf(s_alert.event, sizeof(s_alert.event), "Hurricane Force Wind Warning");
    }
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "day";
    _putenv("TZ=UTC0");
    _tzset();
    scenario(name);
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    ui_landscape_screen_build();
    advance(250);
    assert(s_flushes > 0);
    save(name, "");
    memcpy(s_before, s_frame, sizeof(s_frame));
    if (argc > 2 && strcmp(argv[2], "--frames") == 0) {
        for (int frame = 0; frame < 32; ++frame) {
            char suffix[24];
            snprintf(suffix, sizeof(suffix), "-frame-%02d", frame);
            save(name, suffix);
            advance(125);
        }
    }
    const bool warning = s_alert.level == WEATHER_ALERT_WARNING;
    if (warning) assert(has_caption(s_alert.event));
    if (strcmp(name, "forecast") == 0) assert(has_caption("Forecast"));
    if (strcmp(name, "stale") == 0) assert(has_caption("Weather unavailable"));
    if (strcmp(name, "waiting") == 0) assert(has_caption("Waiting for weather"));
    if (strcmp(name, "forecast") == 0 || strcmp(name, "stale") == 0 ||
        strcmp(name, "waiting") == 0 || strcmp(name, "missing-temperature") == 0)
        assert(!has_caption("74\xC2\xB0" "F"));
    advance(2000);
    assert((memcmp(s_before, s_frame, sizeof(s_frame)) != 0) != warning);
    save(name, "-motion");

    if (warning || strcmp(name, "recovery") == 0) {
        s_alert.level = WEATHER_ALERT_NONE;
        s_observation.observed = s_now;
        advance(10000);
        assert(has_caption("Partly cloudy"));
        assert(!has_caption("NWS alert"));
        assert(memcmp(s_before, s_frame, sizeof(s_frame)) != 0);
        save(name, "-recovered");
    }
    if (strcmp(name, "aging") == 0) {
        s_now += 4 * 3600;
        advance(10000);
        assert(has_caption("Weather unavailable"));
        assert(!has_caption("74\xC2\xB0" "F"));
        save(name, "-expired");
    }

    /* Run beyond multiple weather refreshes so heap leaks and resumed warning
     * animation are exercised, still within the device's 16 kB LVGL arena. */
    advance(60000);
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    assert(lv_mem_test() == LV_RESULT_OK);
    printf("%s: motion/alerts OK, LVGL heap peak %zu/%zu bytes\n", name,
           memory.max_used, memory.total_size);
    lv_deinit();
    return 0;
}
