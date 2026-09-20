#include "weather.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs.h"
#include "storage.h"
#include "time_sync.h"

static const char *TAG = "weather";

#define KEY_LOCATION "wx_loc"

/* api.weather.gov asks callers to identify themselves and to contact them if a
 * client misbehaves. A repo URL is the honest answer and is public already —
 * unlike an email address, which this file would then be leaking. */
#define USER_AGENT "PandaDeath/1.0 (github.com/Cr3L/PandaDeath)"

/* Twelve-hourly forecasts are reissued a few times a day, so polling faster
 * than this buys nothing and spends someone else's bandwidth. The service is
 * free, unauthenticated and run by a public agency; 48 requests a day is a
 * guest's share. */
#define POLL_INTERVAL_MS (30 * 60 * 1000)

/* After a failure, rather than the full interval — a router that came back
 * should not cost half an hour of staleness. Not a backoff ladder: unlike
 * association, a failed fetch has a working fallback (the previous report) and
 * the retry costs one request. */
#define RETRY_INTERVAL_MS (2 * 60 * 1000)

/* While waiting for the address and the clock — a different question from a
 * failed fetch, and it was briefly given the same two-minute answer. Both
 * arrive within seconds of boot, so a board that was ready at five seconds sat
 * idle until two minutes with everything working and nothing to show. Found on
 * hardware; it reads as correct in the source, because the mistake is that one
 * constant answered two questions. */
#define READY_POLL_MS 2000

/* Not-ready wakeups before dropping to the retry pace — a minute at
 * READY_POLL_MS, which is far longer than a working boot takes. */
#define READY_POLL_LIMIT 30

/* The twelve-hourly document measured 14 kB. 24 kB is room for a wordier
 * forecast office without being a gamble; a response that exceeds it is
 * rejected rather than parsed from a truncated buffer, because half a JSON
 * document is not a smaller forecast, it is a wrong one. */
#define RESPONSE_MAX 24576

/* The /points metadata is about 2 kB — an order of magnitude under the
 * forecast, and it does not deserve the forecast's buffer. It is also
 * requested at the worst heap moment the firmware has: first poll after boot,
 * with Wi-Fi, an open TLS session and LVGL all resident. */
#define POINTS_MAX 4096

/* TLS against the full certificate bundle and a cJSON parse of 14 kB, on one
 * task. The console's 8 kB is not enough for this, which is why weather_refresh
 * signals this task instead of doing the work itself. */
#define TASK_STACK 12288
#define TASK_PRIORITY 4

/* Enough for "https://api.weather.gov/gridpoints/ABC/123,456/forecast" with
 * room to spare. */
#define URL_MAX 160

/* The station list is 75 kB and only its head is wanted, so it is read as a
 * prefix and scanned as text — a truncated document is not JSON and cJSON
 * cannot be asked to parse one.
 *
 * 8 kB because the identifiers sit about 1.3 kB apart: measured against the
 * real response, the first is 2.0 kB in and the third 4.7 kB, so 4 kB would
 * reach only two of them. The size is a property of the API's payload, not a
 * round number.
 *
 * Ordering is what makes this work at all — the list is nearest-first. That is
 * not stated in the API documentation, so it was measured: of the 53 stations
 * returned for one grid square, the first 31 are in exact distance order and
 * the first inversion is 130 km out and 300 m wide, which is the grid cell's
 * centre differing from the coordinate rather than disorder. Reliable for
 * picking among the closest few, which is all this does. */
#define STATIONS_MAX 8192

/* One observation is 4.3 kB — small enough that the buffer is not the
 * interesting constraint, sized with the same headroom as the others. */
#define OBSERVATION_MAX 8192

/* Stations report about hourly, so polling faster buys nothing from any one
 * reading. It is not set to an hour because the pressure history is built from
 * these and a board that has just booted would otherwise wait an hour for its
 * first sample. Over-polling is harmless: a reading is only recorded when the
 * station's own timestamp has moved. */
#define OBS_INTERVAL_MS (15 * 60 * 1000)

/* Older than this and the station is treated as not reporting, and the next one
 * along is tried. Two hours is past the point where a station that claims to
 * report hourly has plainly stopped, while still tolerating one missed cycle. */
#define OBS_STALE_SECONDS (2 * 60 * 60)

/* Pressure readings kept for the trend.
 *
 * Sized against the poll interval rather than the station's: at one sample per
 * OBS_INTERVAL_MS this spans six hours, which is twice the window the trend
 * looks back over. The margin is deliberate — the nearest test station
 * republishes every five minutes, so the ring fills at the poll rate and not at
 * the station's, and a ring that spanned only three hours would have no reading
 * old enough to compare against the moment one sample was missed. */
#define PRESSURE_HISTORY 24

/* The window the trend is quoted over. Three hours is the interval every
 * falling-barometer rule of thumb is stated in, so it is the one that makes the
 * number mean something to a person. */
#define TREND_SPAN_SECONDS (3 * 60 * 60)

/* Refuse to report a trend built from less history than this. A delta measured
 * over twenty minutes and scaled up to three hours is noise wearing a confident
 * arrow, which is worse than saying nothing. */
#define TREND_MIN_SECONDS (2 * 60 * 60)

/* Alerts are checked far more often than the forecast. One arriving half an
 * hour late has missed the weather it was warning about; a forecast has not. */
#define ALERT_INTERVAL_MS (5 * 60 * 1000)

/* A point query returns only alerts covering that one coordinate — typically
 * none, occasionally two or three — and each runs about 6 kB because the
 * description text is long. Four will not fit, and that is handled: the fetch
 * refuses an oversized response and the previous alert stands.
 *
 * Note there is no way to ask for fewer. The API has no `limit` parameter; a
 * request carrying one is answered with a 400 whose body is small enough to
 * look like an empty result, which is exactly how it was nearly mistaken for a
 * working filter. */
#define ALERT_MAX 24576

static weather_report_t s_report;
static weather_alert_t s_alert;
static weather_obs_t s_obs;
static TaskHandle_t s_task;

/* Guards s_report. The poll task writes it whole, and the screen reads it
 * whole; without this a fetch landing mid-read puts a temperature from the new
 * report beside a storm time from the old one, which is not a stale forecast
 * but an invented one. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Consecutive not-ready wakeups, so a board that will never be ready stops
 * asking every two seconds forever. */
static int s_not_ready_polls;

/* Uptime of the last successful forecast, 0 for never. Uptime rather than wall
 * clock because it is only ever used as a difference, and it cannot jump when
 * SNTP steps the clock — which it does, by decades, on the first sync. */
static int64_t s_last_forecast_ms;
static int64_t s_last_obs_ms;

/* The grid square the coordinates fall in never moves, so the /points lookup
 * that derives it is made once and its answer kept. Cleared when the location
 * changes, which is what makes weather_location_set() take effect.
 *
 * Both URLs come out of the same /points response, so resolving them together
 * costs one request rather than two. The stations URL is a field in that
 * document and is not derived from the forecast URL by editing its last path
 * segment — those two strings look interchangeable and only one of them is
 * promised by the API.
 *
 * RAM, not NVS. These are re-derived on every boot, which is a few kilobytes
 * over a connection the board needs anyway, and it keeps the coordinates the
 * single stored fact about where this thing is. */
static char s_forecast_url[URL_MAX];
static char s_stations_url[URL_MAX];

/* The coordinates, cached after the first read.
 *
 * weather_location_get is called from the UI refresh, which runs every ten
 * seconds, and it used to open NVS, read a string and close it every time — a
 * blocking flash access on the task driving the display, thousands of times a
 * day, for a value that changes only when someone types `loc`. The store stays
 * the source of truth; this is just the answer, kept. weather_location_set
 * updates it, which is the only way it can go stale.
 *
 * Guarded by s_lock, like everything else here that crosses tasks. It is read
 * from the LVGL timer, the console and the poll task, and without the lock a
 * `loc` landing between the two stores hands a reader a new latitude beside an
 * old longitude — a coordinate in neither place. */
static bool s_location_cached;
static double s_latitude;
static double s_longitude;

/* Nearest observation stations, nearest first, empty strings for unresolved.
 * Derived from s_stations_url and cleared alongside it. */
static char s_stations[WEATHER_STATION_COUNT][WEATHER_STATION_ID_MAX];

/* Pressure history, oldest overwritten first.
 *
 * Keyed on the station's own reading time, never on when the fetch happened.
 * That is what makes the poll interval a free parameter rather than a
 * load-bearing one: a station republishing every five minutes and a board
 * asking every fifteen still yields one sample per reading, and "three hours"
 * means three real hours regardless of either rate. Polling more often than the
 * station reports is harmless, because a repeated timestamp is not a new
 * sample. */
typedef struct {
    time_t at;
    int tenths;   /* tenths of a millibar; the display rounds, this does not */
} pressure_sample_t;

static pressure_sample_t s_pressure[PRESSURE_HISTORY];
static int s_pressure_count;   /* how many slots are filled, up to the ring size */
static int s_pressure_next;    /* where the next sample goes */

/* Which station the history was built from.
 *
 * The readings are station pressure, which depends on the station's elevation —
 * the nearest test station sits at 293 m, and a neighbour at a different height
 * reports a different number for identical weather. Falling back to that
 * neighbour would put a step of several millibars into the history and report
 * it as a plummeting barometer. So a change of station discards the history
 * rather than continuing it: a few hours of no trend, instead of a confident
 * wrong one. */
static char s_pressure_station[WEATHER_STATION_ID_MAX];

const char *weather_alert_level_name(weather_alert_level_t level)
{
    switch (level) {
    case WEATHER_ALERT_NONE:     return "none";
    case WEATHER_ALERT_ADVISORY: return "advisory";
    case WEATHER_ALERT_WATCH:    return "watch";
    case WEATHER_ALERT_WARNING:  return "warning";
    }
    return "unknown";
}

const char *weather_storm_name(weather_storm_t storm)
{
    switch (storm) {
    case WEATHER_STORM_NONE:     return "none";
    case WEATHER_STORM_POSSIBLE: return "possible";
    case WEATHER_STORM_LIKELY:   return "likely";
    case WEATHER_STORM_NOW:      return "now";
    }
    return "unknown";
}

/* Fetches `url` into `buf`. Streams rather than using esp_http_client_perform,
 * because perform gives no way to refuse a response that is too large — it is
 * already in memory by the time anyone could ask.
 *
 * `allow_partial` chooses what an oversized response means. For a document that
 * will be handed to cJSON it is an error, because half a JSON document is not a
 * smaller forecast but an invalid one. For a document only the head of is
 * wanted it is the point: see resolve_stations(). */
static esp_err_t fetch_bounded(const char *url, char *buf, size_t len, bool allow_partial)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .user_agent = USER_AGENT,
        /* Certificate validation against IDF's bundled roots. Without this the
         * handshake fails; with it, a wrong clock does too, which is why the
         * caller waits for time_sync. */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_FAIL, TAG, "client init");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "http %d from %s", status, url);
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    /* A chunked response reports -1, which is not an error and not a length —
     * the read loop below bounds itself either way. */
    if (!allow_partial && content_length > 0 && (size_t)content_length >= len) {
        ESP_LOGE(TAG, "response is %lld bytes, buffer is %u",
                 (long long)content_length, (unsigned)len);
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t total = 0;
    while (total < len - 1) {
        int got = esp_http_client_read(client, buf + total, (int)(len - 1 - total));
        if (got < 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (got == 0) {
            break;  /* complete */
        }
        total += (size_t)got;
    }
    buf[total] = '\0';

    /* Ran to the end of the buffer with the connection still open, so the
     * document is longer than the space for it and what was read is a prefix. */
    if (!allow_partial && total >= len - 1) {
        ESP_LOGE(TAG, "response exceeds %u bytes", (unsigned)len);
        err = ESP_ERR_NO_MEM;
        goto done;
    }

done:
    /* Closing with bytes still unread is how a prefix read stays cheap: the
     * remainder of the document is never pulled over the air. */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t fetch(const char *url, char *buf, size_t len)
{
    return fetch_bounded(url, buf, len, false);
}

/* Copies a string field out of a JSON object, refusing one that would not fit
 * rather than storing a truncated URL — which would fail later as a 404 and
 * look like the service having moved. */
static esp_err_t copy_string_field(const cJSON *obj, const char *name, char *buf, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strlen(item->valuestring) >= len) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(buf, item->valuestring, len);
    return ESP_OK;
}

/* Resolves coordinates to the per-grid-square URLs, both from one response. */
static esp_err_t resolve_grid(void)
{
    double lat, lon;
    ESP_RETURN_ON_ERROR(weather_location_get(&lat, &lon), TAG, "no location");

    char url[URL_MAX];
    snprintf(url, sizeof(url), "https://api.weather.gov/points/%.4f,%.4f", lat, lon);

    char *body = malloc(POINTS_MAX);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    esp_err_t err = fetch(url, body, POINTS_MAX);
    if (err != ESP_OK) {
        free(body);
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "points json");

    const cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "properties");
    err = copy_string_field(props, "forecast", s_forecast_url, sizeof(s_forecast_url));
    if (err == ESP_OK) {
        /* Not fatal. Without a station list there are no live observations, but
         * the forecast — which is what the dial is built on — still works, and
         * failing the whole resolution would take it down too. */
        esp_err_t serr = copy_string_field(props, "observationStations",
                                           s_stations_url, sizeof(s_stations_url));
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "no station list: %s", esp_err_to_name(serr));
            s_stations_url[0] = '\0';
        }
    }

    cJSON_Delete(root);
    return err;
}

/* Fills s_stations from the head of the station list.
 *
 * Text scan, not a parse. The response is 75 kB against a chip with tens of
 * kilobytes to spare, so only its first pages are read, and a truncated
 * document is not JSON — cJSON would reject it outright. What is being looked
 * for is a fixed key in a machine-generated document, which is the narrow case
 * where scanning for a literal is sound rather than lazy. */
static esp_err_t resolve_stations(void)
{
    ESP_RETURN_ON_FALSE(s_stations_url[0] != '\0', ESP_ERR_INVALID_STATE,
                        TAG, "no station list url");

    char *body = malloc(STATIONS_MAX);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    esp_err_t err = fetch_bounded(s_stations_url, body, STATIONS_MAX, true);
    if (err != ESP_OK) {
        free(body);
        return err;
    }

    /* Scanned into a local and published in one step. Writing the shared array
     * entry by entry would let a reader see the first station of the new
     * location beside the third of the old one. */
    char found_ids[WEATHER_STATION_COUNT][WEATHER_STATION_ID_MAX] = {0};

    /* The key only. The separator is matched by skipping to the value's opening
     * quote rather than by spelling it out: this document is pretty-printed
     * ("stationIdentifier": "KMGY") and the compact form appears nowhere in it,
     * which is exactly the assumption the first version of this made and found
     * zero stations for. Whitespace inside JSON is the server's choice and not
     * something to encode a guess about. */
    static const char KEY[] = "\"stationIdentifier\"";
    int found = 0;
    const char *p = body;
    while (found < WEATHER_STATION_COUNT && (p = strstr(p, KEY)) != NULL) {
        p += sizeof(KEY) - 1;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') {
            p++;
        }
        if (*p != '"') {
            break;
        }
        p++;
        const char *end = strchr(p, '"');
        /* A closing quote missing, or an identifier longer than any real one,
         * means the prefix ended mid-field. Stop rather than store a fragment:
         * the truncation point is arbitrary and the last entry is the one it
         * lands in. */
        if (end == NULL || (size_t)(end - p) >= WEATHER_STATION_ID_MAX) {
            break;
        }
        memcpy(found_ids[found], p, (size_t)(end - p));
        found++;
        p = end;
    }
    free(body);

    if (found == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    portENTER_CRITICAL(&s_lock);
    memcpy(s_stations, found_ids, sizeof(s_stations));
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "stations: %s %s %s", s_stations[0], s_stations[1], s_stations[2]);
    return ESP_OK;
}

/* Days since 1970-01-01 for a proleptic Gregorian date — Howard Hinnant's
 * civil-from-days algorithm, which is exact for any date this will ever see and
 * needs no lookup table or leap-year special case. The shift by three months
 * puts the leap day at the end of the year, which is what removes the special
 * case. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

/* Converts an ISO 8601 timestamp with a numeric offset — "2026-07-26T22:00:00-05:00",
 * which is the only form NWS emits — to epoch seconds. 0 on anything else.
 *
 * Hand-rolled because strptime cannot read the offset: %z is a glibc extension
 * that newlib does not implement, and without it the timestamp would be taken
 * as local, putting the answer out by the board's own timezone offset. */
static time_t parse_iso8601(const char *s)
{
    int year, month, day, hour, minute, second, off_hour, off_minute;
    char sign;

    if (s == NULL) {
        return 0;
    }
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%c%2d:%2d",
               &year, &month, &day, &hour, &minute, &second,
               &sign, &off_hour, &off_minute) != 9) {
        return 0;
    }
    if (sign != '+' && sign != '-') {
        return 0;
    }

    /* Converted arithmetically rather than with mktime, which would apply the
     * board's TZ to fields that are already at the offset the string states —
     * putting the answer out by the local UTC offset, four hours here, which is
     * small enough to look plausible on a dial. timegm would be the right call
     * and this toolchain's libc does not provide one. */
    const int64_t days = days_from_civil(year, month, day);
    return (time_t)(days * 86400LL + hour * 3600 + minute * 60 + second
                    - ((sign == '-') ? -1 : 1) * ((off_hour * 3600) + (off_minute * 60)));
}

/* Reads probabilityOfPrecipitation, which is an object holding a value that is
 * null as often as it is a number. Returns -1 for "not stated", which is not
 * the same as zero and should not be shown as one. */
static int period_pop(const cJSON *period)
{
    const cJSON *pop = cJSON_GetObjectItemCaseSensitive(period, "probabilityOfPrecipitation");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(pop, "value");
    return cJSON_IsNumber(value) ? value->valueint : -1;
}

/* NWS writes shortForecast from a controlled vocabulary, so matching on it is
 * reading a field rather than guessing at prose — "Chance Showers And
 * Thunderstorms", "Slight Chance Showers And Thunderstorms". Matching the same
 * way against the whole JSON document would also hit the office name, the icon
 * URL and the detailed text, which is why this takes one field. */
static bool mentions_thunder(const char *short_forecast)
{
    return short_forecast != NULL && strcasestr(short_forecast, "thunder") != NULL;
}

/* The threshold between "possible" and "likely". NWS phrasing puts "Slight
 * Chance" at 20% or below and "Chance" from 30 to 50, so this splits the two
 * bands the service itself uses rather than inventing one. */
#define POP_LIKELY 30

static esp_err_t parse_forecast(const char *body, weather_report_t *out)
{
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "forecast json");

    esp_err_t err = ESP_ERR_NOT_FOUND;

    const cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "properties");
    const cJSON *periods = cJSON_GetObjectItemCaseSensitive(props, "periods");
    if (!cJSON_IsArray(periods) || cJSON_GetArraySize(periods) == 0) {
        goto done;
    }

    weather_report_t r = { 0 };
    r.pop = -1;
    r.periods_ahead = -1;
    r.storm = WEATHER_STORM_NONE;

    /* The current conditions come from period 0 regardless of storms — the
     * screen wants a temperature whether or not anything is brewing. */
    const cJSON *first = cJSON_GetArrayItem(periods, 0);
    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(first, "temperature");
    const cJSON *unit = cJSON_GetObjectItemCaseSensitive(first, "temperatureUnit");
    const cJSON *now_short = cJSON_GetObjectItemCaseSensitive(first, "shortForecast");
    const cJSON *first_start = cJSON_GetObjectItemCaseSensitive(first, "startTime");
    const cJSON *first_end = cJSON_GetObjectItemCaseSensitive(first, "endTime");
    r.forecast_start = parse_iso8601(cJSON_IsString(first_start) ? first_start->valuestring : NULL);
    r.forecast_end = parse_iso8601(cJSON_IsString(first_end) ? first_end->valuestring : NULL);
    if (cJSON_IsNumber(temp)) {
        r.temperature = temp->valueint;
    }
    if (cJSON_IsString(unit) && unit->valuestring != NULL) {
        strlcpy(r.temperature_unit, unit->valuestring, sizeof(r.temperature_unit));
    }
    if (cJSON_IsString(now_short) && now_short->valuestring != NULL) {
        strlcpy(r.now, now_short->valuestring, sizeof(r.now));
    }

    /* First storm wins: the question this answers is "when is the next one",
     * so scanning past it to a worse one later would report the wrong period. */
    int index = 0;
    const cJSON *period = NULL;
    cJSON_ArrayForEach(period, periods) {
        const cJSON *short_forecast =
            cJSON_GetObjectItemCaseSensitive(period, "shortForecast");
        const char *text = cJSON_IsString(short_forecast) ? short_forecast->valuestring : NULL;

        if (mentions_thunder(text)) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(period, "name");
            const int pop = period_pop(period);

            const cJSON *start = cJSON_GetObjectItemCaseSensitive(period, "startTime");
            r.starts_at = parse_iso8601(cJSON_IsString(start) ? start->valuestring : NULL);

            r.periods_ahead = index;
            r.pop = pop;
            if (cJSON_IsString(name) && name->valuestring != NULL) {
                strlcpy(r.when, name->valuestring, sizeof(r.when));
            }
            strlcpy(r.summary, text, sizeof(r.summary));

            if (index == 0) {
                r.storm = WEATHER_STORM_NOW;
            } else {
                r.storm = (pop >= POP_LIKELY) ? WEATHER_STORM_LIKELY
                                              : WEATHER_STORM_POSSIBLE;
            }
            break;
        }
        index++;
    }

    r.fetched = time(NULL);

    /* Filled without locking. This function knows nothing about which object it
     * is writing, so taking a lock that guards one particular global would be
     * correct only for the caller that happens to pass it — publishing is the
     * caller's job, and poll_once() does it by name. */
    *out = r;
    err = ESP_OK;

done:
    cJSON_Delete(root);
    return err;
}

/* Reads one observation field.
 *
 * Each is an object of its own — {"value": 27, "unitCode": "wmoUnit:degC"} —
 * and a station that does not measure something still sends the object, with a
 * null value. So "absent" and "present but null" both mean the same thing here,
 * and both have to be caught: cJSON_IsNumber is false for null, which is what
 * makes one test enough. */
static double obs_value(const cJSON *props, const char *name, bool *ok)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(props, name);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(field, "value");
    if (!cJSON_IsNumber(value)) {
        *ok = false;
        return 0.0;
    }
    *ok = true;
    return value->valuedouble;
}

/* The conversions, each applied once, here. lround rather than a cast so that
 * -0.4 °C becomes 0 °F rather than truncating toward zero on one side of the
 * scale and not the other. */
static int obs_int(const cJSON *props, const char *name, double scale, double offset)
{
    bool ok = false;
    const double v = obs_value(props, name, &ok);
    return ok ? (int)lround(v * scale + offset) : WEATHER_UNKNOWN;
}

#define C_TO_F_SCALE  1.8
#define C_TO_F_OFFSET 32.0
#define PA_TO_MB      0.01
#define KMH_TO_MPH    0.621371
#define M_TO_MILES    (1.0 / 1609.344)

/* `pressure_tenths` is separate from the struct's whole-millibar `pressure`
 * rather than replacing it, because the two have different jobs: the display
 * wants a number a person reads, and the trend wants resolution the rounding
 * would throw away. Putting tenths in the public struct would push a unit
 * conversion into every consumer to save one parameter here. */
static esp_err_t parse_observation(const char *body, weather_obs_t *out, int *pressure_tenths)
{
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "obs json");

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "properties");
    if (!cJSON_IsObject(props)) {
        goto done;
    }

    weather_obs_t o = {0};

    const cJSON *stamp = cJSON_GetObjectItemCaseSensitive(props, "timestamp");
    if (!cJSON_IsString(stamp) || stamp->valuestring == NULL) {
        goto done;
    }
    o.observed = parse_iso8601(stamp->valuestring);
    if (o.observed == 0) {
        goto done;
    }

    o.temperature    = obs_int(props, "temperature", C_TO_F_SCALE, C_TO_F_OFFSET);
    o.dewpoint       = obs_int(props, "dewpoint", C_TO_F_SCALE, C_TO_F_OFFSET);
    o.humidity       = obs_int(props, "relativeHumidity", 1.0, 0.0);
    /* Read once and rounded once. The whole-millibar figure is the tenths
     * rounded again, not a second look at the same JSON field. */
    *pressure_tenths = obs_int(props, "barometricPressure", PA_TO_MB * 10.0, 0.0);
    o.pressure = (*pressure_tenths == WEATHER_UNKNOWN)
                 ? WEATHER_UNKNOWN
                 : (int)lround(*pressure_tenths / 10.0);
    o.pressure_trend = WEATHER_UNKNOWN;   /* filled by the caller, which owns the history */
    o.wind           = obs_int(props, "windSpeed", KMH_TO_MPH, 0.0);
    o.gust           = obs_int(props, "windGust", KMH_TO_MPH, 0.0);
    o.wind_direction = obs_int(props, "windDirection", 1.0, 0.0);
    o.visibility     = obs_int(props, "visibility", M_TO_MILES, 0.0);

    /* One field, because they are the same question. Only one of the two is
     * ever reported — heat index above about 80 °F, wind chill below 50 — and
     * showing "feels like" is more use than showing which formula produced it.
     * Falls back to the measured temperature so the field is blank only when
     * the temperature itself is. */
    const int heat = obs_int(props, "heatIndex", C_TO_F_SCALE, C_TO_F_OFFSET);
    const int chill = obs_int(props, "windChill", C_TO_F_SCALE, C_TO_F_OFFSET);
    o.feels_like = (heat != WEATHER_UNKNOWN) ? heat
                 : (chill != WEATHER_UNKNOWN) ? chill
                 : o.temperature;

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(props, "textDescription");
    if (cJSON_IsString(text) && text->valuestring != NULL) {
        strlcpy(o.text, text->valuestring, sizeof(o.text));
    }

    /* Usable if it measured *anything*, not specifically a temperature.
     *
     * Requiring a temperature was wrong, and a live severe thunderstorm showed
     * why: the nearest station reported a 59 km/h gust and a dewpoint with its
     * temperature field null, and this threw the whole document away — losing
     * the most interesting reading of the storm to a missing field the screen
     * would happily have left blank. Every field already carries a "not
     * reported" state; the only document worth rejecting is one carrying no
     * measurement at all, which is a station that is powered but not sensing. */
    if (o.temperature == WEATHER_UNKNOWN && o.dewpoint == WEATHER_UNKNOWN &&
        o.pressure == WEATHER_UNKNOWN && o.wind == WEATHER_UNKNOWN &&
        o.gust == WEATHER_UNKNOWN && o.humidity == WEATHER_UNKNOWN) {
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }

    *out = o;
    err = ESP_OK;

done:
    cJSON_Delete(root);
    return err;
}

/* Classifies from the event name's last word. NWS names are a controlled
 * vocabulary — "Severe Thunderstorm Warning", "Tornado Watch", "Heat Advisory",
 * "Special Weather Statement" — and that final word is the issuing forecaster's
 * own statement of whether the thing is happening or merely possible. */
static weather_alert_level_t classify_alert(const char *event)
{
    if (event == NULL) {
        return WEATHER_ALERT_NONE;
    }
    if (strcasestr(event, "Warning") != NULL) {
        return WEATHER_ALERT_WARNING;
    }
    if (strcasestr(event, "Watch") != NULL) {
        return WEATHER_ALERT_WATCH;
    }
    return WEATHER_ALERT_ADVISORY;
}

static esp_err_t parse_alerts(const char *body, weather_alert_t *out)
{
    cJSON *root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "alerts json");

    weather_alert_t worst = { 0 };
    worst.level = WEATHER_ALERT_NONE;

    /* An error document has no features array, which is also what a quiet sky
     * looks like — so this cannot distinguish them and does not try. The fetch
     * has already rejected a non-200, which is where a bad request is caught. */
    const cJSON *features = cJSON_GetObjectItemCaseSensitive(root, "features");
    const cJSON *feature = NULL;
    cJSON_ArrayForEach(feature, features) {
        const cJSON *props = cJSON_GetObjectItemCaseSensitive(feature, "properties");
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(props, "event");
        if (!cJSON_IsString(event) || event->valuestring == NULL) {
            continue;
        }

        const weather_alert_level_t level = classify_alert(event->valuestring);
        if (level <= worst.level) {
            continue;  /* keep the most serious one only */
        }

        worst.level = level;
        strlcpy(worst.event, event->valuestring, sizeof(worst.event));

        const cJSON *severity = cJSON_GetObjectItemCaseSensitive(props, "severity");
        worst.severe = cJSON_IsString(severity) && severity->valuestring != NULL &&
                       (strcmp(severity->valuestring, "Extreme") == 0 ||
                        strcmp(severity->valuestring, "Severe") == 0);
    }

    worst.checked = time(NULL);
    *out = worst;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t poll_alerts(void)
{
    double lat, lon;
    ESP_RETURN_ON_ERROR(weather_location_get(&lat, &lon), TAG, "no location");

    char url[URL_MAX];
    snprintf(url, sizeof(url),
             "https://api.weather.gov/alerts/active?point=%.4f,%.4f", lat, lon);

    char *body = malloc(ALERT_MAX);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    weather_alert_t parsed;
    esp_err_t err = fetch(url, body, ALERT_MAX);
    if (err == ESP_OK) {
        err = parse_alerts(body, &parsed);
    }
    free(body);

    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_alert = parsed;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

/* Adds a reading to the history, unless it is one already held.
 *
 * The test is the station's timestamp, not ours. Polling faster than the
 * station publishes is the normal case and must not fill the ring with copies
 * of one reading, which would shorten the span it covers and quietly stop the
 * trend from ever having three hours to look back over. */
static void record_pressure(const char *station, time_t at, int tenths)
{
    if (tenths == WEATHER_UNKNOWN || at == 0) {
        return;
    }
    if (strcmp(station, s_pressure_station) != 0) {
        ESP_LOGD(TAG, "pressure history restarts at %s", station);
        s_pressure_count = 0;
        s_pressure_next = 0;
        strlcpy(s_pressure_station, station, sizeof(s_pressure_station));
    }
    if (s_pressure_count > 0) {
        const int newest = (s_pressure_next + PRESSURE_HISTORY - 1) % PRESSURE_HISTORY;
        if (at <= s_pressure[newest].at) {
            return;
        }
    }

    s_pressure[s_pressure_next].at = at;
    s_pressure[s_pressure_next].tenths = tenths;
    s_pressure_next = (s_pressure_next + 1) % PRESSURE_HISTORY;
    if (s_pressure_count < PRESSURE_HISTORY) {
        s_pressure_count++;
    }
}

/* Change over TREND_SPAN_SECONDS in tenths of a millibar, or WEATHER_UNKNOWN
 * while there is not enough history.
 *
 * The comparison reading is the one whose age is closest to three hours, not
 * simply the oldest held: once the ring spans six hours, the oldest sample
 * would quote a six-hour change as if it were a three-hour one. Whatever is
 * chosen, the delta is scaled to the full window, so a gap in the readings
 * changes the precision of the answer and not its units. */
static int pressure_trend(void)
{
    if (s_pressure_count < 2) {
        return WEATHER_UNKNOWN;
    }

    const int newest = (s_pressure_next + PRESSURE_HISTORY - 1) % PRESSURE_HISTORY;
    const time_t now = s_pressure[newest].at;

    int best = -1;
    time_t best_miss = 0;
    for (int i = 0; i < s_pressure_count; i++) {
        if (i == newest) {
            continue;
        }
        const time_t span = now - s_pressure[i].at;
        if (span < TREND_MIN_SECONDS) {
            continue;
        }
        const time_t miss = span > TREND_SPAN_SECONDS ? span - TREND_SPAN_SECONDS
                                                      : TREND_SPAN_SECONDS - span;
        if (best < 0 || miss < best_miss) {
            best = i;
            best_miss = miss;
        }
    }
    if (best < 0) {
        return WEATHER_UNKNOWN;
    }

    const time_t span = now - s_pressure[best].at;
    const int delta = s_pressure[newest].tenths - s_pressure[best].tenths;
    return (int)((int64_t)delta * TREND_SPAN_SECONDS / span);
}

/* Reads the latest observation, trying each station in turn.
 *
 * Nearest first, and the first usable answer wins — usable meaning it parsed,
 * carries a temperature, and is recent. A station that is merely quiet today is
 * indistinguishable from one that is broken, and neither is worth the screen
 * going blank over when there is another a few miles away. */
static esp_err_t poll_observation(void)
{
    char stations[WEATHER_STATION_COUNT][WEATHER_STATION_ID_MAX];
    weather_stations_copy(stations);
    ESP_RETURN_ON_FALSE(stations[0][0] != '\0', ESP_ERR_INVALID_STATE,
                        TAG, "no stations");

    char *body = malloc(OBSERVATION_MAX);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    const time_t now = time(NULL);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    esp_err_t first_error = ESP_OK;
    weather_obs_t parsed = {0};
    int tenths = WEATHER_UNKNOWN;

    for (int i = 0; i < WEATHER_STATION_COUNT && stations[i][0] != '\0'; i++) {
        char url[URL_MAX];
        snprintf(url, sizeof(url),
                 "https://api.weather.gov/stations/%s/observations/latest",
                 stations[i]);

        err = fetch(url, body, OBSERVATION_MAX);
        if (err == ESP_OK) {
            err = parse_observation(body, &parsed, &tenths);
        }
        if (err == ESP_OK && now - parsed.observed > OBS_STALE_SECONDS) {
            ESP_LOGD(TAG, "%s is %lld min stale", stations[i],
                     (long long)((now - parsed.observed) / 60));
            err = ESP_ERR_TIMEOUT;
        }
        if (err == ESP_OK) {
            strlcpy(parsed.station, stations[i], sizeof(parsed.station));
            break;
        }

        /* Kept only if nothing has been reported yet, so the error that
         * survives is the first station's rather than the last one's. One of
         * the three nearest here answers /observations/latest with a 404 —
         * it is listed but publishes nothing — and reporting that as the reason
         * pointed at a service outage when the truth was one dud neighbour. */
        if (first_error == ESP_OK) {
            first_error = err;
        }
        ESP_LOGD(TAG, "%s unusable: %s", stations[i], esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        err = first_error;
    }
    free(body);

    /* Recorded only for the station the loop settled on, and only once it is
     * known which that was — a reading from a station that was then rejected as
     * stale has no business in the history. */
    if (err == ESP_OK) {
        record_pressure(parsed.station, parsed.observed, tenths);
        parsed.pressure_trend = pressure_trend();
    }

    portENTER_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        s_obs = parsed;
    }
    /* Recorded either way, and on the live object rather than the parsed one,
     * so a run where every station failed still says why. */
    s_obs.last_error = err;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

static esp_err_t poll_once(void)
{
    if (s_forecast_url[0] == '\0') {
        ESP_RETURN_ON_ERROR(resolve_grid(), TAG, "resolve");
        ESP_LOGI(TAG, "grid resolved: %s", s_forecast_url);
    }

    /* Tested separately from the grid, not folded into resolving it.
     *
     * These come from the same /points response but they fail independently:
     * the station list is a second request, and one 503 or one dropped
     * association used to leave s_stations empty for the entire life of the
     * boot — every later poll skipped this block because the *grid* had
     * resolved, so poll_observation returned ESP_ERR_INVALID_STATE forever and
     * only a reboot or a `loc` change recovered. Best effort still: a station
     * list that cannot be read costs the observations and nothing else. */
    if (s_stations[0][0] == '\0' && s_stations_url[0] != '\0') {
        esp_err_t serr = resolve_stations();
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "stations unresolved: %s", esp_err_to_name(serr));
        }
    }

    char *body = malloc(RESPONSE_MAX);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "no memory");

    weather_report_t parsed;
    esp_err_t err = fetch(s_forecast_url, body, RESPONSE_MAX);
    if (err == ESP_OK) {
        err = parse_forecast(body, &parsed);
    }
    free(body);

    /* Published in one assignment under the lock, so a reader sees either the
     * whole old report or the whole new one — never a temperature from this
     * fetch beside a storm time from the last. Parsing happens off to the side
     * precisely so the critical section is a struct copy and nothing else. */
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_report = parsed;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

/* Whether a schedule tracked by uptime has come round, 0 meaning never run.
 * Shared by the forecast and the observation, which differ only in their
 * interval — the second one arriving is what turned this from an inline
 * comparison into something worth naming. */
static bool due(int64_t last_ms, int64_t now_ms, int64_t interval_ms)
{
    return last_ms == 0 || now_ms - last_ms >= interval_ms;
}

static void weather_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t wait_ms = POLL_INTERVAL_MS;

        /* Both conditions, not just the address: a certificate is rejected when
         * the clock says 1970, and that failure reads like a network fault
         * while being a boot-ordering one. */
        if (!net_have_address() || !time_sync_synced()) {
            /* Fast while there is reason to expect it soon — both arrive within
             * seconds of a normal boot — then at the ordinary retry pace. A
             * board with no credentials is never ready, and without this it
             * would wake 43,000 times a day to ask. */
            wait_ms = (s_not_ready_polls < READY_POLL_LIMIT) ? READY_POLL_MS
                                                             : RETRY_INTERVAL_MS;
            s_not_ready_polls++;
        } else {
            s_not_ready_polls = 0;
            wait_ms = ALERT_INTERVAL_MS;

            /* Alerts every wake; the forecast only when its own interval is up.
             * Tracked by elapsed time rather than by counting wakes, because a
             * refresh asked for at the console resets neither schedule and
             * would otherwise pull the forecast along with it. */
            const int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t last_forecast, last_obs;
            portENTER_CRITICAL(&s_lock);
            last_forecast = s_last_forecast_ms;
            last_obs = s_last_obs_ms;
            portEXIT_CRITICAL(&s_lock);

            esp_err_t err = poll_alerts();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "alert poll failed: %s", esp_err_to_name(err));
            } else if (s_alert.level != WEATHER_ALERT_NONE) {
                ESP_LOGW(TAG, "active: %s", s_alert.event);
            }

            if (due(last_forecast, now_ms, POLL_INTERVAL_MS)) {
                err = poll_once();
                portENTER_CRITICAL(&s_lock);
                s_report.last_error = err;
                portEXIT_CRITICAL(&s_lock);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "poll failed: %s", esp_err_to_name(err));
                    /* The previous report is left standing. Stale weather with
                     * a timestamp beside it is more use than no weather, and
                     * the console prints how old it is. */
                    wait_ms = RETRY_INTERVAL_MS;
                } else {
                    portENTER_CRITICAL(&s_lock);
                    s_last_forecast_ms = now_ms;
                    portEXIT_CRITICAL(&s_lock);
                    ESP_LOGI(TAG, "%d%s %s; storm %s",
                             s_report.temperature, s_report.temperature_unit,
                             s_report.now, weather_storm_name(s_report.storm));
                }
            }

            /* After the forecast, because the stations are resolved by the same
             * /points lookup the forecast triggers — on a first boot this is
             * what lets the first reading arrive on the same wake rather than
             * fifteen minutes later. */
            if (due(last_obs, now_ms, OBS_INTERVAL_MS)) {
                err = poll_observation();
                if (err != ESP_OK) {
                    ESP_LOGD(TAG, "no observation: %s", esp_err_to_name(err));
                } else {
                    portENTER_CRITICAL(&s_lock);
                    s_last_obs_ms = now_ms;
                    portEXIT_CRITICAL(&s_lock);
                    ESP_LOGI(TAG, "%s: %d F, dew %d, %d mb",
                             s_obs.station, s_obs.temperature, s_obs.dewpoint,
                             s_obs.pressure);
                }
            }
        }

        /* Waits, but wakes early when weather_refresh() notifies. The
         * notification is the whole mechanism: it keeps every TLS handshake on
         * this task and its stack, no matter who asked for one. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t weather_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    /* Not pinned. The task is asleep almost always and its wakeups are neither
     * latency-critical nor cache-sensitive, so choosing a core for it would be
     * a decision with no reason behind it. */
    BaseType_t ok = xTaskCreate(weather_task, "weather", TASK_STACK, NULL,
                                TASK_PRIORITY, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t weather_refresh(void)
{
    ESP_RETURN_ON_FALSE(s_task != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void weather_alert_copy(weather_alert_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_alert;
    portEXIT_CRITICAL(&s_lock);
}

void weather_report_copy(weather_report_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_report;
    portEXIT_CRITICAL(&s_lock);
}

void weather_obs_copy(weather_obs_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_obs;
    portEXIT_CRITICAL(&s_lock);
}

void weather_stations_copy(char out[WEATHER_STATION_COUNT][WEATHER_STATION_ID_MAX])
{
    portENTER_CRITICAL(&s_lock);
    memcpy(out, s_stations, sizeof(s_stations));
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t weather_location_set(double lat, double lon)
{
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }

    char value[48];
    snprintf(value, sizeof(value), "%.4f,%.4f", lat, lon);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, KEY_LOCATION, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        /* The cached grid square belongs to the old coordinates. Clearing it
         * here rather than recomputing keeps this function free of network
         * work — the next poll resolves it, and fails visibly if it cannot. */
        s_forecast_url[0] = '\0';
        s_stations_url[0] = '\0';
        for (int i = 0; i < WEATHER_STATION_COUNT; i++) {
            s_stations[i][0] = '\0';
        }
        /* Not strictly needed — record_pressure discards the history when the
         * station changes, and a new location means a new station. Done here
         * anyway so the state a location change leaves behind is stated in one
         * place rather than deduced from a rule two functions away. */
        s_pressure_count = 0;
        s_pressure_next = 0;
        s_pressure_station[0] = '\0';
        portENTER_CRITICAL(&s_lock);
        s_latitude = lat;
        s_longitude = lon;
        s_location_cached = true;
        s_report.fetched = 0;
        /* The forecast schedule belongs to the old coordinates too. Without
         * this the task holds the timestamp of the last fetch and declines to
         * make another for up to half an hour, leaving a board that has just
         * been told where it is with no forecast and no plan to get one — the
         * cached URL and the report were cleared, and the *timer* was not.
         * Found by changing location twice in a row on hardware. */
        s_last_forecast_ms = 0;
        /* The observation belongs to the old coordinates as much as the
         * forecast does, and its schedule with it. */
        s_obs.observed = 0;
        s_last_obs_ms = 0;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

esp_err_t weather_location_get(double *lat, double *lon)
{
    bool cached;
    portENTER_CRITICAL(&s_lock);
    cached = s_location_cached;
    if (cached) {
        *lat = s_latitude;
        *lon = s_longitude;
    }
    portEXIT_CRITICAL(&s_lock);
    if (cached) {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char value[48];
    size_t size = sizeof(value);
    err = nvs_get_str(handle, KEY_LOCATION, value, &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    /* Written by this module in one format, so a value that will not parse means
     * the key was written by something else — worth an error rather than a
     * silently wrong location. */
    if (sscanf(value, "%lf,%lf", lat, lon) != 2) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_latitude = *lat;
    s_longitude = *lon;
    s_location_cached = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
