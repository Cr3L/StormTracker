#pragma once

#include <stdbool.h>
#include <time.h>

#define LANDSCAPE_OBSERVATION_MAX_AGE (2 * 60 * 60)
#define LANDSCAPE_FORECAST_MAX_AGE (3 * 60 * 60)
#define LANDSCAPE_TWILIGHT_SECONDS (30 * 60)

typedef enum {
    LANDSCAPE_CONDITION_UNKNOWN,
    LANDSCAPE_CONDITION_CLEAR,
    LANDSCAPE_CONDITION_PARTLY_CLOUDY,
    LANDSCAPE_CONDITION_CLOUDY,
    LANDSCAPE_CONDITION_RAIN,
    LANDSCAPE_CONDITION_SNOW,
    LANDSCAPE_CONDITION_STORM,
    LANDSCAPE_CONDITION_FOG,
} landscape_condition_t;

typedef enum {
    LANDSCAPE_PHASE_UNKNOWN,
    LANDSCAPE_PHASE_DAY,
    LANDSCAPE_PHASE_TWILIGHT,
    LANDSCAPE_PHASE_NIGHT,
} landscape_phase_t;

typedef enum {
    LANDSCAPE_SOURCE_NONE,
    LANDSCAPE_SOURCE_OBSERVATION,
    LANDSCAPE_SOURCE_FORECAST,
} landscape_source_t;

typedef enum {
    LANDSCAPE_STATE_NO_DATA,
    LANDSCAPE_STATE_CURRENT,
    LANDSCAPE_STATE_STALE,
} landscape_state_t;

typedef struct {
    time_t now;
    bool clock_synced;
    const char *observation_text;
    time_t observed;
    const char *forecast_text; /* Current period only, never the next storm. */
    time_t forecast_fetched;
    time_t forecast_start;
    time_t forecast_end;
    bool sun_valid;
    time_t sunrise;
    time_t sunset;
} landscape_input_t;

typedef struct {
    landscape_condition_t condition;
    landscape_phase_t phase;
    landscape_source_t source;
    landscape_state_t state;
    unsigned light; /* 0..256: night to day, smoothly through twilight. */
} landscape_scene_t;

/* An old or unrecognised report produces an unknown condition, never an
 * assumed clear sky. Daylight remains independent of weather availability.
 * Missing sun times, including polar dates without a rise or set, give an
 * unknown phase: the caller should use neutral lighting, not invent a day. */
landscape_scene_t landscape_resolve(const landscape_input_t *input);

bool landscape_observation_fresh(time_t now, time_t observed, bool clock_synced);
const char *landscape_condition_name(landscape_condition_t condition);
const char *landscape_phase_name(landscape_phase_t phase);
const char *landscape_source_name(landscape_source_t source);
