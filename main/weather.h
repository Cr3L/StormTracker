#pragma once

#include <limits.h>
#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

/* Storm watch from the US National Weather Service.
 *
 * api.weather.gov is free, needs no key, and is the authoritative source for US
 * forecasts. It is a two-request API and there is no way around that: the
 * /points/{lat},{lon} endpoint returns *metadata* — which forecast office and
 * grid square the coordinates fall in — and the forecast lives at a URL it
 * hands back. The grid square never moves, so that first request is made once
 * and its answer kept.
 *
 * Uses the twelve-hourly forecast, not the hourly one. The hourly endpoint
 * would give "storm in three hours" instead of "storm tonight", and it is 92 kB
 * of JSON against 14 kB — enough to need PSRAM, which is a separate piece of
 * work with its own verification. Recorded in docs/open-items.md rather than
 * folded in here.
 *
 * Depends on the clock. TLS rejects a certificate whose validity window does
 * not contain the current time, and an unsynced board thinks it is 1970 — so
 * this waits for time_sync before its first request rather than failing in a
 * way that looks like a network fault. */

/* How close a thunderstorm is. Ordered by severity, and nothing depends on the
 * numbering. */
typedef enum {
    WEATHER_STORM_NONE,      /* nothing in the forecast window */
    WEATHER_STORM_POSSIBLE,  /* forecast mentions storms, low probability */
    WEATHER_STORM_LIKELY,    /* forecast mentions storms, high probability */
    WEATHER_STORM_NOW,       /* the current period is the stormy one */
} weather_storm_t;

const char *weather_storm_name(weather_storm_t storm);

typedef struct {
    /* Wall clock of the last successful fetch, 0 for never — which is also the
     * "is there a report" test. A separate valid flag was a second copy of the
     * same fact, and the invariant had already broken: changing the location
     * cleared the flag and left the timestamp, so the two disagreed by
     * construction. Same reasoning as time_sync.c's s_last. */
    time_t fetched;

    /* Why the last attempt failed, ESP_OK if it did not. Kept because the
     * reasons are not guessable from outside: coordinates outside the United
     * States fail exactly like a dead network, and telling someone to check
     * their Wi-Fi when the real answer is "that place is not in the service"
     * sends them the wrong way. */
    esp_err_t last_error;

    weather_storm_t storm;
    int periods_ahead;          /* 12-hour periods until the storm; 0 = now */

    /* When the stormy period begins, 0 if no storm is forecast. Parsed from the
     * period's ISO 8601 startTime rather than inferred from periods_ahead,
     * because the periods are not a uniform twelve hours — the first one runs
     * from now until the next boundary, so "two periods ahead" is anywhere from
     * twelve to thirty-six hours away. */
    time_t starts_at;
    int pop;                    /* probability of precipitation, percent, -1 unknown */
    char when[24];              /* the period's own name: "Monday Night" */
    char summary[64];           /* that period's shortForecast */

    int temperature;            /* current period, in temperature_unit */
    char temperature_unit[2];   /* "F" or "C", as the service reports it */
    char now[64];               /* current period's shortForecast */
    /* The landscape must not keep animating an expired forecast period. */
    time_t forecast_start;
    time_t forecast_end;
} weather_report_t;

/* How serious an active alert is, taken from the event's own name rather than
 * from the severity field. NWS names are a controlled vocabulary ending in
 * Warning, Watch, Advisory or Statement, and that ending is what the issuing
 * meteorologist chose to mean "happening" versus "possible" — severity is a
 * separate axis that rates heat advisories as Moderate alongside them. */
typedef enum {
    WEATHER_ALERT_NONE,
    WEATHER_ALERT_ADVISORY,  /* advisories and statements: information */
    WEATHER_ALERT_WATCH,     /* conditions are favourable, hours out */
    WEATHER_ALERT_WARNING,   /* it is happening, and near */
} weather_alert_level_t;

const char *weather_alert_level_name(weather_alert_level_t level);

typedef struct {
    weather_alert_level_t level;
    char event[48];    /* "Severe Thunderstorm Warning", verbatim */
    bool severe;       /* the API's own severity is Extreme or Severe */
    time_t checked;    /* when the alert feed was last read; 0 for never */
} weather_alert_t;

/* Nearby observation stations, nearest first.
 *
 * Three, not one. A station can be unreachable, or can go hours between
 * reports, and neither is rare enough to leave the screen blank for — so the
 * next one along is tried.
 *
 * The fallback is whole-observation, not per-field. Filling a missing gust from
 * a station 20 km away would produce a reading no station ever made, labelled
 * with a place it did not come from; a blank field is the honest answer and the
 * station's name is printed beside the numbers so it is clear whose reading it
 * is.
 *
 * The list they come from is 75 kB, which does not fit anywhere on this chip,
 * and only its head is wanted. It is read as a prefix and scanned as text; that
 * works because the list is ordered nearest-first, which the API documentation
 * does not state and which was therefore measured. See weather.c. */
#define WEATHER_STATION_COUNT 3

/* NWS identifiers are four characters for airports ("KMGY") and up to six for
 * the rest. */
#define WEATHER_STATION_ID_MAX 8

/* Copies the resolved station identifiers. Entries are empty strings until the
 * first successful poll, and beyond however many were found. */
void weather_stations_copy(char out[WEATHER_STATION_COUNT][WEATHER_STATION_ID_MAX]);

/* A reading the station did not report.
 *
 * Every numeric field needs one, because these arrive null individually rather
 * than as a missing document: on an ordinary clear day the nearest test station
 * reported no gust, no wind chill, no sea-level pressure and no three-hour
 * precipitation, while reporting everything else. Zero is not available as a
 * sentinel — 0 °F, 0 mph and 0% are all real readings — so it is a value no
 * instrument can produce. */
#define WEATHER_UNKNOWN INT_MIN

/* The latest observation from a real instrument, as opposed to the forecast.
 *
 * Converted to US customary units at parse time. The observations endpoint
 * reports SI (°C, Pa, km/h) while the forecast reports °F, and leaving both in
 * their source units would put the conversion in every consumer and let the two
 * halves of one screen disagree about what a temperature is.
 *
 * Rounded to integers because a 240 px circle cannot show a decimal place worth
 * having, and because the underlying instruments do not justify one — the
 * humidity in a live response came back as 65.564318547283 percent. */
typedef struct {
    /* Which station this came from, and when the station made the reading —
     * not when we fetched it. The distinction matters: a station reporting
     * hourly is up to an hour stale the moment it is collected, and that is
     * normal rather than a fault. 0 for never. */
    char station[WEATHER_STATION_ID_MAX];
    time_t observed;

    esp_err_t last_error;   /* why the last attempt failed, ESP_OK if it did not */

    int temperature;        /* °F */
    int dewpoint;           /* °F */
    int feels_like;         /* °F: heat index or wind chill, whichever applies */
    int humidity;           /* percent */
    int pressure;           /* millibars — the meteorological unit, and the one
                             * a falling-barometer rule of thumb is quoted in */
    int wind;               /* mph */
    int gust;               /* mph; usually unknown, which is itself the signal */
    int wind_direction;     /* degrees the wind blows *from*, 0..359 */
    int visibility;         /* miles */
    char text[32];          /* the station's own words: "Clear", "Thunderstorm" */

    /* Change in pressure over the last three hours, in tenths of a millibar,
     * negative for falling. WEATHER_UNKNOWN until enough readings have been
     * collected to span the window.
     *
     * The only figure here the board works out for itself rather than reads off
     * a wire, and the reason the pressure is worth showing at all: 1012 mb is
     * inert, 1012 and falling is a forecast. Tenths because the interesting
     * thresholds are 1 mb and 3 mb over three hours, and whole millibars would
     * quantise a "notable" fall into the same bucket as no change.
     *
     * The history behind it lives in RAM, so this is blank for the first hours
     * after every boot. That is the accepted cost of not writing to flash on a
     * schedule forever. */
    int pressure_trend;
} weather_obs_t;

/* A consistent snapshot, same discipline as weather_report_copy. `observed` is
 * 0 until a reading has arrived. */
void weather_obs_copy(weather_obs_t *out);

/* The most serious alert covering the stored coordinates.
 *
 * Only the worst one is kept. A point under three simultaneous alerts is not
 * three times as interesting, and a 240 px circle has room to say one thing. */
void weather_alert_copy(weather_alert_t *out);

/* Starts the poll task. Returns immediately; the first report arrives once
 * there is an address and a synced clock.
 *
 * The task polls two feeds on different clocks: the forecast every half hour,
 * because that is how often it is reissued, and alerts every few minutes,
 * because an alert that arrives twenty-five minutes late has missed its
 * purpose. */
esp_err_t weather_start(void);

/* Asks the poll task to fetch now, and returns without waiting.
 *
 * Deliberately does not fetch on the caller's task — not because the stack
 * could not take it (ota_cmd.c runs a TLS handshake on the console task and
 * that works), but because this one can block for the length of two HTTPS
 * round trips with the prompt held, and because the 24 kB buffer belongs to the
 * module that owns the poll interval rather than to whoever asked. */
esp_err_t weather_refresh(void);

/* A consistent snapshot, taken under the lock the poll task writes behind.
 * `fetched` is 0 until a report has arrived.
 *
 * The only accessor. There was briefly a second returning the live struct,
 * with a comment conceding it was safe for the console and unsafe for the
 * screen — a rule enforced by prose, on a codebase that otherwise deletes the
 * shape which makes the wrong thing representable. Copying ~200 bytes costs
 * well under a microsecond, so there was nothing to trade for the risk. */
void weather_report_copy(weather_report_t *out);

/* Coordinates, stored in NVS.
 *
 * Not compiled in. A latitude and longitude to two decimal places is someone's
 * neighbourhood, this repo is public, and git keeps a value after the file
 * holding it is edited — the same reasoning that keeps Wi-Fi credentials out of
 * the tree. Changing them clears the cached grid square, so the next poll
 * re-derives it. */
esp_err_t weather_location_set(double lat, double lon);
esp_err_t weather_location_get(double *lat, double *lon);
