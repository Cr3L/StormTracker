#include "ui_landscape.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "landscape.h"
#include "landscape_art.h"
#include "lvgl.h"
#include "sun.h"
#include "time_sync.h"
#include "weather.h"

#define FRAME_MS 125
#define WEATHER_REFRESH_MS 10000
#define ALERT_STALE_SECONDS (15 * 60)

/* One 28.8 kB native RGB565 framebuffer, outside LVGL's 16 kB heap. Scaling
 * with nearest-neighbour keeps the pixel art sharp without enabling PSRAM. */
static uint16_t s_pixels[LANDSCAPE_ART_SIZE * LANDSCAPE_ART_SIZE];
static const lv_image_dsc_t s_image = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .w = LANDSCAPE_ART_SIZE,
        .h = LANDSCAPE_ART_SIZE,
        .stride = LANDSCAPE_ART_SIZE * sizeof(uint16_t),
    },
    .data_size = sizeof(s_pixels),
    .data = (const uint8_t *)s_pixels,
};

static lv_obj_t *s_picture;
static lv_obj_t *s_clock;
static lv_obj_t *s_temperature;
static lv_obj_t *s_condition;
static lv_obj_t *s_source;
static landscape_scene_t s_scene;
static int s_wind;
static int s_daylight = -1;
static bool s_takeover;
static uint32_t s_started;
static lv_timer_t *s_animation;

static void warning_layout(bool takeover)
{
    static int previous = -1;
    if (previous == (int)takeover) return;
    previous = (int)takeover;
    lv_obj_t *objects[] = { s_picture, s_clock, s_temperature };
    for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
        if (takeover) lv_obj_add_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_color(lv_screen_active(),
                             lv_color_hex(takeover ? 0x690B25 : 0x142A35), 0);
    lv_obj_set_style_text_font(s_condition,
        takeover ? &lv_font_montserrat_20 : &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_condition, takeover ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_condition, takeover ? LV_SIZE_CONTENT : 32);
    if (takeover) {
        lv_obj_align(s_condition, LV_ALIGN_CENTER, 0, -8);
        lv_obj_align(s_source, LV_ALIGN_CENTER, 0, 75);
    } else {
        lv_obj_set_align(s_condition, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(s_condition, 28, 178);
        lv_obj_set_align(s_source, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(s_source, 54, 211);
    }
}

static void label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *make_caption(lv_obj_t *parent, int y, int width,
                              const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_pos(label, (240 - width) / 2, y);
    lv_label_set_text(label, "");
    return label;
}

static void animate(lv_timer_t *timer)
{
    (void)timer;
    if (s_takeover) {
        return;
    }
    landscape_art_render(s_pixels, &s_scene, lv_tick_elaps(s_started),
                         s_wind, s_daylight);
    lv_obj_invalidate(s_picture);
}

static void refresh_weather(lv_timer_t *timer)
{
    (void)timer;
    const time_t now = time(NULL);
    const bool synced = time_sync_synced();
    weather_report_t forecast;
    weather_obs_t observation;
    weather_alert_t alert;
    weather_report_copy(&forecast);
    weather_obs_copy(&observation);
    weather_alert_copy(&alert);

    double latitude, longitude;
    time_t rise = 0, set = 0;
    const bool sun_valid = synced &&
        weather_location_get(&latitude, &longitude) == ESP_OK &&
        sun_times(latitude, longitude, now, &rise, &set) == ESP_OK;
    const landscape_input_t input = {
        .now = now,
        .clock_synced = synced,
        .observation_text = observation.text,
        .observed = observation.observed,
        .forecast_text = forecast.now,
        .forecast_fetched = forecast.fetched,
        .forecast_start = forecast.forecast_start,
        .forecast_end = forecast.forecast_end,
        .sun_valid = sun_valid,
        .sunrise = rise,
        .sunset = set,
    };
    s_scene = landscape_resolve(&input);
    const uint32_t ink = (s_scene.phase == LANDSCAPE_PHASE_DAY ||
                          s_scene.phase == LANDSCAPE_PHASE_UNKNOWN) ? 0x142A35 : 0xFFFFFF;
    static uint32_t last_ink;
    if (ink != last_ink) {
        lv_obj_set_style_text_color(s_clock, lv_color_hex(ink), 0);
        lv_obj_set_style_text_color(s_temperature, lv_color_hex(ink), 0);
        last_ink = ink;
    }
    s_daylight = -1;
    if (sun_valid && set > rise) {
        const double progress = difftime(now, rise) / difftime(set, rise);
        s_daylight = progress <= 0 ? 0 : progress >= 1 ? 1000 : (int)(progress * 1000);
    }

    const bool observed = landscape_observation_fresh(now, observation.observed, synced);
    s_wind = observed && observation.wind != WEATHER_UNKNOWN ? observation.wind : 0;
    if (s_wind < 0) s_wind = 0;
    if (s_wind > 80) s_wind = 80;

    char text[80] = "";
    if (synced) {
        /* time_clock rounds astronomy event times; a live clock must floor. */
        struct tm local;
        localtime_r(&now, &local);
        strftime(text, sizeof(text), "%I:%M %p", &local);
        if (text[0] == '0') memmove(text, text + 1, strlen(text));
    }
    label_text(s_clock, text);

    text[0] = '\0';
    if (observed && observation.temperature != WEATHER_UNKNOWN) {
        snprintf(text, sizeof(text), "%d\xC2\xB0" "F", observation.temperature);
    }
    label_text(s_temperature, text);

    if (s_scene.state == LANDSCAPE_STATE_CURRENT) {
        snprintf(text, sizeof(text), "%s", landscape_condition_name(s_scene.condition));
    } else {
        snprintf(text, sizeof(text), "%s", s_scene.state == LANDSCAPE_STATE_STALE
                 ? "Weather unavailable" : "Waiting for weather");
    }
    label_text(s_condition, text);
    label_text(s_source, s_scene.source == LANDSCAPE_SOURCE_FORECAST
                 ? "Forecast" : "");

    /* Keep a cached warning visible if the feed fails, but say when its status
     * can no longer be verified. A network outage cannot silently clear it. */
    const bool alert_stale = !synced || alert.checked == 0 ||
        difftime(now, alert.checked) < 0 || difftime(now, alert.checked) > ALERT_STALE_SECONDS;
    s_takeover = alert.level == WEATHER_ALERT_WARNING && alert.severe;
    warning_layout(s_takeover);
    if (s_takeover) {
        label_text(s_condition, alert.event);
        label_text(s_source, alert_stale ? "Update overdue" : "NWS alert");
        if (s_animation != NULL) lv_timer_pause(s_animation);
    } else {
        if (s_animation != NULL) lv_timer_resume(s_animation);
        if (alert.level != WEATHER_ALERT_NONE) {
            label_text(s_condition, alert.event);
            label_text(s_source, alert_stale ? "Update overdue" : "NWS alert");
        }
        animate(NULL);
    }
}

void ui_landscape_screen_build(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x142A35), 0);

    s_picture = lv_image_create(screen);
    lv_image_set_src(s_picture, &s_image);
    lv_image_set_antialias(s_picture, false);
    lv_image_set_pivot(s_picture, LANDSCAPE_ART_SIZE / 2, LANDSCAPE_ART_SIZE / 2);
    lv_image_set_scale(s_picture, 2 * LV_SCALE_NONE);
    lv_obj_center(s_picture);

    s_clock = make_caption(screen, 23, 150, &lv_font_montserrat_14);
    s_temperature = make_caption(screen, 42, 170, &lv_font_montserrat_28);
    s_condition = make_caption(screen, 178, 184, &lv_font_montserrat_14);
    s_source = make_caption(screen, 211, 132, &lv_font_montserrat_14);
    /* Long event names stay within two lines above the source label. */
    lv_label_set_long_mode(s_condition, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_condition, 32);

    s_started = lv_tick_get();
    refresh_weather(NULL);
    s_animation = lv_timer_create(animate, FRAME_MS, NULL);
    if (s_takeover) lv_timer_pause(s_animation);
    lv_timer_create(refresh_weather, WEATHER_REFRESH_MS, NULL);
}
