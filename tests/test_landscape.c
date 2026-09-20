#include "landscape.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static landscape_input_t sample(void)
{
    return (landscape_input_t) {
        .now = 100000,
        .clock_synced = true,
        .observation_text = "Clear",
        .observed = 99700,
        .forecast_text = "Rain",
        .forecast_fetched = 99000,
        .forecast_start = 80000,
        .forecast_end = 120000,
        .sun_valid = true,
        .sunrise = 80000,
        .sunset = 120000,
    };
}

static void test_conditions(void)
{
    const struct {
        const char *text;
        landscape_condition_t condition;
    } cases[] = {
        {"Clear", LANDSCAPE_CONDITION_CLEAR},
        {"SUNNY", LANDSCAPE_CONDITION_CLEAR},
        {"Fair", LANDSCAPE_CONDITION_CLEAR},
        {"Gradual Clearing", LANDSCAPE_CONDITION_CLEAR},
        {"Mostly Sunny", LANDSCAPE_CONDITION_PARTLY_CLOUDY},
        {"Mostly Clear", LANDSCAPE_CONDITION_PARTLY_CLOUDY},
        {"Partly Sunny", LANDSCAPE_CONDITION_PARTLY_CLOUDY},
        {"Partly Cloudy", LANDSCAPE_CONDITION_PARTLY_CLOUDY},
        {"A Few Clouds", LANDSCAPE_CONDITION_PARTLY_CLOUDY},
        {"Scattered Clouds", LANDSCAPE_CONDITION_PARTLY_CLOUDY},
        {"Mostly Cloudy", LANDSCAPE_CONDITION_CLOUDY},
        {"Overcast", LANDSCAPE_CONDITION_CLOUDY},
        {"Broken Clouds", LANDSCAPE_CONDITION_CLOUDY},
        {"Light Rain", LANDSCAPE_CONDITION_RAIN},
        {"Chance Rain Showers", LANDSCAPE_CONDITION_RAIN},
        {"Freezing Drizzle", LANDSCAPE_CONDITION_RAIN},
        {"Light Snow and Rain", LANDSCAPE_CONDITION_SNOW},
        {"Snow Showers", LANDSCAPE_CONDITION_SNOW},
        {"Flurries", LANDSCAPE_CONDITION_SNOW},
        {"Sleet", LANDSCAPE_CONDITION_SNOW},
        {"Ice Pellets", LANDSCAPE_CONDITION_SNOW},
        {"Thunderstorm in Vicinity", LANDSCAPE_CONDITION_STORM},
        {"Chance Showers And THUNDERSTORMS", LANDSCAPE_CONDITION_STORM},
        {"Rain with Thunder", LANDSCAPE_CONDITION_STORM},
        {"Patchy Fog", LANDSCAPE_CONDITION_FOG},
        {"Mist", LANDSCAPE_CONDITION_FOG},
        {"Haze", LANDSCAPE_CONDITION_FOG},
        {"Smoke", LANDSCAPE_CONDITION_FOG},
        {"", LANDSCAPE_CONDITION_UNKNOWN},
        {NULL, LANDSCAPE_CONDITION_UNKNOWN},
        {"Unknown", LANDSCAPE_CONDITION_UNKNOWN},
        {"No rain", LANDSCAPE_CONDITION_UNKNOWN},
        {"Not clear", LANDSCAPE_CONDITION_UNKNOWN},
        {"Clear data unavailable", LANDSCAPE_CONDITION_UNKNOWN},
        {"Missing sky condition", LANDSCAPE_CONDITION_UNKNOWN},
        {"No weather reported", LANDSCAPE_CONDITION_UNKNOWN},
        {"Windy", LANDSCAPE_CONDITION_UNKNOWN},
        {"Unclear", LANDSCAPE_CONDITION_UNKNOWN},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        landscape_input_t input = sample();
        input.observation_text = cases[i].text;
        input.forecast_text = NULL;
        const landscape_scene_t scene = landscape_resolve(&input);
        assert(scene.condition == cases[i].condition);
        assert(scene.state == (cases[i].condition == LANDSCAPE_CONDITION_UNKNOWN
                                   ? LANDSCAPE_STATE_NO_DATA : LANDSCAPE_STATE_CURRENT));
    }

    char long_text[128];
    memset(long_text, ' ', sizeof(long_text));
    memcpy(long_text, "Clear", 5);
    long_text[sizeof(long_text) - 1] = '\0';
    landscape_input_t input = sample();
    input.observation_text = long_text;
    input.forecast_text = NULL;
    assert(landscape_resolve(&input).condition == LANDSCAPE_CONDITION_UNKNOWN);
}

static void test_freshness(void)
{
    landscape_input_t input = sample();
    landscape_scene_t scene = landscape_resolve(&input);
    assert(scene.condition == LANDSCAPE_CONDITION_CLEAR);
    assert(scene.source == LANDSCAPE_SOURCE_OBSERVATION);
    assert(scene.state == LANDSCAPE_STATE_CURRENT);

    input.observed = input.now - LANDSCAPE_OBSERVATION_MAX_AGE;
    assert(landscape_resolve(&input).source == LANDSCAPE_SOURCE_OBSERVATION);
    input.observed--;
    scene = landscape_resolve(&input);
    assert(scene.condition == LANDSCAPE_CONDITION_RAIN);
    assert(scene.source == LANDSCAPE_SOURCE_FORECAST);

    input.forecast_fetched = input.now - LANDSCAPE_FORECAST_MAX_AGE;
    assert(landscape_resolve(&input).source == LANDSCAPE_SOURCE_FORECAST);
    input.forecast_fetched--;
    scene = landscape_resolve(&input);
    assert(scene.condition == LANDSCAPE_CONDITION_UNKNOWN);
    assert(scene.source == LANDSCAPE_SOURCE_NONE);
    assert(scene.state == LANDSCAPE_STATE_STALE);

    input = sample();
    input.observation_text = "Unknown";
    assert(landscape_resolve(&input).source == LANDSCAPE_SOURCE_FORECAST);
    input.observed = 0;
    input.forecast_end = input.now;
    assert(landscape_resolve(&input).state == LANDSCAPE_STATE_STALE);
    input.forecast_end++;
    input.forecast_start = input.now;
    assert(landscape_resolve(&input).source == LANDSCAPE_SOURCE_FORECAST);
    input.forecast_start++;
    assert(landscape_resolve(&input).state == LANDSCAPE_STATE_NO_DATA);

    input = sample();
    input.observed = input.now + 1;
    assert(landscape_resolve(&input).source == LANDSCAPE_SOURCE_FORECAST);
    input.forecast_fetched = input.now + 1;
    assert(landscape_resolve(&input).state == LANDSCAPE_STATE_NO_DATA);
    input = sample();
    input.observed = 0;
    input.forecast_start = 0;
    assert(landscape_resolve(&input).state == LANDSCAPE_STATE_NO_DATA);
    input = sample();
    input.observed = 0;
    input.forecast_end = input.forecast_start;
    assert(landscape_resolve(&input).state == LANDSCAPE_STATE_NO_DATA);

    input = sample();
    input.clock_synced = false;
    scene = landscape_resolve(&input);
    assert(scene.condition == LANDSCAPE_CONDITION_UNKNOWN);
    assert(scene.phase == LANDSCAPE_PHASE_UNKNOWN);
    assert(scene.state == LANDSCAPE_STATE_NO_DATA);
    input = (landscape_input_t) {0};
    assert(landscape_resolve(&input).state == LANDSCAPE_STATE_NO_DATA);
    assert(landscape_resolve(NULL).state == LANDSCAPE_STATE_NO_DATA);
    assert(!landscape_observation_fresh(100, 101, true));
    assert(!landscape_observation_fresh(100, 0, true));
    assert(!landscape_observation_fresh(100, 100, false));
}

static void test_daylight(void)
{
    landscape_input_t input = sample();
    const struct {
        time_t when;
        landscape_phase_t phase;
    } cases[] = {
        {input.sunrise - LANDSCAPE_TWILIGHT_SECONDS - 1, LANDSCAPE_PHASE_NIGHT},
        {input.sunrise - LANDSCAPE_TWILIGHT_SECONDS, LANDSCAPE_PHASE_TWILIGHT},
        {input.sunrise, LANDSCAPE_PHASE_TWILIGHT},
        {input.sunrise + LANDSCAPE_TWILIGHT_SECONDS, LANDSCAPE_PHASE_TWILIGHT},
        {input.sunrise + LANDSCAPE_TWILIGHT_SECONDS + 1, LANDSCAPE_PHASE_DAY},
        {input.sunset - LANDSCAPE_TWILIGHT_SECONDS - 1, LANDSCAPE_PHASE_DAY},
        {input.sunset - LANDSCAPE_TWILIGHT_SECONDS, LANDSCAPE_PHASE_TWILIGHT},
        {input.sunset, LANDSCAPE_PHASE_TWILIGHT},
        {input.sunset + LANDSCAPE_TWILIGHT_SECONDS, LANDSCAPE_PHASE_TWILIGHT},
        {input.sunset + LANDSCAPE_TWILIGHT_SECONDS + 1, LANDSCAPE_PHASE_NIGHT},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        input.now = cases[i].when;
        assert(landscape_resolve(&input).phase == cases[i].phase);
    }
    input.now = input.sunrise - LANDSCAPE_TWILIGHT_SECONDS;
    assert(landscape_resolve(&input).light == 0);
    input.now = input.sunrise;
    assert(landscape_resolve(&input).light == 128);
    input.now = input.sunrise + LANDSCAPE_TWILIGHT_SECONDS;
    assert(landscape_resolve(&input).light == 256);
    input.now = input.sunset - LANDSCAPE_TWILIGHT_SECONDS;
    assert(landscape_resolve(&input).light == 256);
    input.now = input.sunset;
    assert(landscape_resolve(&input).light == 128);
    input.now = input.sunset + LANDSCAPE_TWILIGHT_SECONDS;
    assert(landscape_resolve(&input).light == 0);
    input.sun_valid = false;
    assert(landscape_resolve(&input).phase == LANDSCAPE_PHASE_UNKNOWN);
    input.sun_valid = true;
    input.sunrise = 0;
    assert(landscape_resolve(&input).phase == LANDSCAPE_PHASE_UNKNOWN);
    input.sunrise = input.sunset;
    assert(landscape_resolve(&input).phase == LANDSCAPE_PHASE_UNKNOWN);
}

int main(void)
{
    test_conditions();
    test_freshness();
    test_daylight();
    assert(strcmp(landscape_condition_name(LANDSCAPE_CONDITION_RAIN), "Rain") == 0);
    assert(strcmp(landscape_phase_name(LANDSCAPE_PHASE_DAY), "Day") == 0);
    assert(strcmp(landscape_source_name(LANDSCAPE_SOURCE_FORECAST), "Forecast") == 0);
    puts("landscape model: all tests passed");
    return 0;
}
