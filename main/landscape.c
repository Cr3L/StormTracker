#include "landscape.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Both NWS text fields are shorter than this. Reject a truncated description
 * instead of letting a missing suffix change its meaning. */
#define DESCRIPTION_MAX 96

static bool phrase(const char *text, const char *words)
{
    const size_t length = strlen(words);
    const char *at = text;
    while ((at = strstr(at, words)) != NULL) {
        if ((at == text || !isalpha((unsigned char)at[-1])) &&
            !isalpha((unsigned char)at[length])) {
            return true;
        }
        at++;
    }
    return false;
}

static landscape_condition_t classify(const char *description)
{
    if (description == NULL) {
        return LANDSCAPE_CONDITION_UNKNOWN;
    }
    char text[DESCRIPTION_MAX];
    size_t i = 0;
    for (; i < sizeof(text) - 1 && description[i] != '\0'; i++) {
        text[i] = (char)tolower((unsigned char)description[i]);
    }
    if (description[i] != '\0') {
        return LANDSCAPE_CONDITION_UNKNOWN;
    }
    text[i] = '\0';

    /* The API normally uses positive condition phrases. Negation or missing
     * data is safer left unillustrated than interpreted as a weather report. */
    if (phrase(text, "no") || phrase(text, "not") || phrase(text, "without") ||
        phrase(text, "none") || phrase(text, "unknown") ||
        phrase(text, "unavailable") || phrase(text, "missing")) {
        return LANDSCAPE_CONDITION_UNKNOWN;
    }
    if (phrase(text, "thunderstorm") || phrase(text, "thunderstorms") ||
        phrase(text, "thunder")) {
        return LANDSCAPE_CONDITION_STORM;
    }
    if (phrase(text, "snow") || phrase(text, "flurries") ||
        phrase(text, "sleet") || phrase(text, "ice pellets") ||
        phrase(text, "blizzard")) {
        return LANDSCAPE_CONDITION_SNOW;
    }
    if (phrase(text, "rain") || phrase(text, "showers") ||
        phrase(text, "shower") || phrase(text, "drizzle")) {
        return LANDSCAPE_CONDITION_RAIN;
    }
    if (phrase(text, "fog") || phrase(text, "mist") || phrase(text, "haze") ||
        phrase(text, "smoke")) {
        return LANDSCAPE_CONDITION_FOG;
    }
    if (phrase(text, "partly cloudy") || phrase(text, "partly sunny") ||
        phrase(text, "mostly sunny") || phrase(text, "mostly clear") ||
        phrase(text, "few clouds") || phrase(text, "scattered clouds")) {
        return LANDSCAPE_CONDITION_PARTLY_CLOUDY;
    }
    if (phrase(text, "cloudy") || phrase(text, "overcast") ||
        phrase(text, "broken clouds")) {
        return LANDSCAPE_CONDITION_CLOUDY;
    }
    if (phrase(text, "clear") || phrase(text, "sunny") ||
        phrase(text, "fair") || phrase(text, "clearing")) {
        return LANDSCAPE_CONDITION_CLEAR;
    }
    return LANDSCAPE_CONDITION_UNKNOWN;
}

static bool recent(time_t now, time_t timestamp, int max_age)
{
    return now > 0 && timestamp > 0 && timestamp <= now &&
           difftime(now, timestamp) <= max_age;
}

bool landscape_observation_fresh(time_t now, time_t observed, bool clock_synced)
{
    return clock_synced && recent(now, observed, LANDSCAPE_OBSERVATION_MAX_AGE);
}

static landscape_phase_t classify_daylight(const landscape_input_t *input)
{
    if (!input->clock_synced || input->now <= 0 || !input->sun_valid ||
        input->sunrise <= 0 || input->sunset <= input->sunrise) {
        return LANDSCAPE_PHASE_UNKNOWN;
    }
    const double from_rise = difftime(input->now, input->sunrise);
    const double from_set = difftime(input->now, input->sunset);
    if ((from_rise >= -LANDSCAPE_TWILIGHT_SECONDS &&
         from_rise <= LANDSCAPE_TWILIGHT_SECONDS) ||
        (from_set >= -LANDSCAPE_TWILIGHT_SECONDS &&
         from_set <= LANDSCAPE_TWILIGHT_SECONDS)) {
        return LANDSCAPE_PHASE_TWILIGHT;
    }
    return input->now > input->sunrise && input->now < input->sunset
               ? LANDSCAPE_PHASE_DAY : LANDSCAPE_PHASE_NIGHT;
}

landscape_scene_t landscape_resolve(const landscape_input_t *input)
{
    landscape_scene_t scene = {
        .condition = LANDSCAPE_CONDITION_UNKNOWN,
        .phase = LANDSCAPE_PHASE_UNKNOWN,
        .source = LANDSCAPE_SOURCE_NONE,
        .state = LANDSCAPE_STATE_NO_DATA,
    };
    if (input == NULL) {
        return scene;
    }
    scene.phase = classify_daylight(input);
    if (scene.phase != LANDSCAPE_PHASE_UNKNOWN) {
        const double from_rise = difftime(input->now, input->sunrise);
        const double until_set = difftime(input->sunset, input->now);
        const double nearest = from_rise < until_set ? from_rise : until_set;
        const double light = (nearest + LANDSCAPE_TWILIGHT_SECONDS) * 256 /
                             (2 * LANDSCAPE_TWILIGHT_SECONDS);
        scene.light = light <= 0 ? 0 : light >= 256 ? 256 : (unsigned)light;
    }
    if (!input->clock_synced || input->now <= 0) {
        return scene;
    }

    const landscape_condition_t observed = classify(input->observation_text);
    if (observed != LANDSCAPE_CONDITION_UNKNOWN &&
        landscape_observation_fresh(input->now, input->observed, true)) {
        scene.condition = observed;
        scene.source = LANDSCAPE_SOURCE_OBSERVATION;
        scene.state = LANDSCAPE_STATE_CURRENT;
        return scene;
    }

    const landscape_condition_t forecast = classify(input->forecast_text);
    if (forecast != LANDSCAPE_CONDITION_UNKNOWN &&
        recent(input->now, input->forecast_fetched, LANDSCAPE_FORECAST_MAX_AGE) &&
        input->forecast_start > 0 && input->forecast_start <= input->now &&
        input->forecast_end > input->now) {
        scene.condition = forecast;
        scene.source = LANDSCAPE_SOURCE_FORECAST;
        scene.state = LANDSCAPE_STATE_CURRENT;
        return scene;
    }

    if ((observed != LANDSCAPE_CONDITION_UNKNOWN && input->observed > 0 &&
         input->observed <= input->now) ||
        (forecast != LANDSCAPE_CONDITION_UNKNOWN && input->forecast_fetched > 0 &&
         input->forecast_fetched <= input->now && input->forecast_start > 0 &&
         input->forecast_start <= input->now &&
         input->forecast_end > input->forecast_start)) {
        scene.state = LANDSCAPE_STATE_STALE;
    }
    return scene;
}

const char *landscape_condition_name(landscape_condition_t condition)
{
    switch (condition) {
    case LANDSCAPE_CONDITION_CLEAR: return "Clear";
    case LANDSCAPE_CONDITION_PARTLY_CLOUDY: return "Partly cloudy";
    case LANDSCAPE_CONDITION_CLOUDY: return "Cloudy";
    case LANDSCAPE_CONDITION_RAIN: return "Rain";
    case LANDSCAPE_CONDITION_SNOW: return "Snow";
    case LANDSCAPE_CONDITION_STORM: return "Thunderstorms";
    case LANDSCAPE_CONDITION_FOG: return "Fog / haze";
    default: return "Unknown";
    }
}

const char *landscape_phase_name(landscape_phase_t phase)
{
    switch (phase) {
    case LANDSCAPE_PHASE_DAY: return "Day";
    case LANDSCAPE_PHASE_TWILIGHT: return "Twilight";
    case LANDSCAPE_PHASE_NIGHT: return "Night";
    default: return "Unknown";
    }
}

const char *landscape_source_name(landscape_source_t source)
{
    switch (source) {
    case LANDSCAPE_SOURCE_OBSERVATION: return "Observed";
    case LANDSCAPE_SOURCE_FORECAST: return "Forecast";
    default: return "Unavailable";
    }
}
