#include "landscape_art.h"

#include <stddef.h>

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HEX(v) RGB(((v) >> 16) & 255, ((v) >> 8) & 255, (v) & 255)
#define WIDTH LANDSCAPE_ART_SIZE

typedef struct {
    uint16_t sky_top, sky_bottom, ridge_back, ridge_front;
    uint16_t meadow, grass, grass_light, grass_dark;
    uint16_t tree, tree_light, trunk;
    uint16_t lake_top, lake_bottom, reflection;
    uint16_t roof, roof_shadow, wall, wall_shadow, window;
    uint16_t cloud, cloud_shadow, snow, snow_shadow;
} palette_t;

static const palette_t day_palette = {
    HEX(0x4186a6), HEX(0xb9d9cd), HEX(0x819dae), HEX(0x4f7889),
    HEX(0x689c79), HEX(0x6b9a59), HEX(0x9abb70), HEX(0x365f49),
    HEX(0x285247), HEX(0x477c59), HEX(0x344740),
    HEX(0x508b9a), HEX(0x285365), HEX(0xa2cbc9),
    HEX(0xad5960), HEX(0x703e4c), HEX(0xebddbc), HEX(0xabae96), HEX(0xf8d48b),
    HEX(0xe5eee7), HEX(0xafc6c9), HEX(0xe3eeea), HEX(0xaabfc5),
};

static const palette_t twilight_palette = {
    HEX(0x46526e), HEX(0xd49a9c), HEX(0x85829c), HEX(0x536179),
    HEX(0x667d76), HEX(0x5d7454), HEX(0x8c9972), HEX(0x344d48),
    HEX(0x2b4647), HEX(0x4b6660), HEX(0x303d40),
    HEX(0x718194), HEX(0x35485f), HEX(0xcba8ad),
    HEX(0x985c67), HEX(0x5b3e51), HEX(0xc6b2a3), HEX(0x807e7a), HEX(0xffd495),
    HEX(0xcab9c5), HEX(0x9694ac), HEX(0xd4d2dc), HEX(0x949eb4),
};

static const palette_t night_palette = {
    HEX(0x13263c), HEX(0x3e5266), HEX(0x354b60), HEX(0x263e50),
    HEX(0x34534d), HEX(0x304c3f), HEX(0x42644d), HEX(0x1d3734),
    HEX(0x183932), HEX(0x2e5142), HEX(0x192e30),
    HEX(0x34576b), HEX(0x152e43), HEX(0x658493),
    HEX(0x574858), HEX(0x363343), HEX(0x818a8a), HEX(0x536566), HEX(0xffd18a),
    HEX(0x627585), HEX(0x455b70), HEX(0x8b9cae), HEX(0x5a748d),
};

static const palette_t unknown_palette = {
    HEX(0x516e7c), HEX(0xa9bdba), HEX(0x7c979f), HEX(0x55747e),
    HEX(0x709487), HEX(0x638468), HEX(0x8fa57c), HEX(0x3c6055),
    HEX(0x2c514c), HEX(0x52776a), HEX(0x344947),
    HEX(0x648d97), HEX(0x385d6e), HEX(0xa5c0c4),
    HEX(0x93686b), HEX(0x614a56), HEX(0xc4c6b2), HEX(0x949e94), HEX(0xb5ced1),
    HEX(0xcbd7d3), HEX(0x9eb2b6), HEX(0xd9e5e3), HEX(0xa0b6c0),
};

static uint16_t blend(uint16_t a, uint16_t b, unsigned amount)
{
    unsigned inverse = 256 - amount;
    unsigned red = (((a >> 11) * inverse) + ((b >> 11) * amount)) >> 8;
    unsigned green = ((((a >> 5) & 63) * inverse) + (((b >> 5) & 63) * amount)) >> 8;
    unsigned blue = (((a & 31) * inverse) + ((b & 31) * amount)) >> 8;
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static palette_t blend_palette(const palette_t *a, const palette_t *b, unsigned amount)
{
    palette_t result;
#define MIX(field) result.field = blend(a->field, b->field, amount)
    MIX(sky_top); MIX(sky_bottom); MIX(ridge_back); MIX(ridge_front);
    MIX(meadow); MIX(grass); MIX(grass_light); MIX(grass_dark);
    MIX(tree); MIX(tree_light); MIX(trunk);
    MIX(lake_top); MIX(lake_bottom); MIX(reflection);
    MIX(roof); MIX(roof_shadow); MIX(wall); MIX(wall_shadow); MIX(window);
    MIX(cloud); MIX(cloud_shadow); MIX(snow); MIX(snow_shadow);
#undef MIX
    return result;
}

static void pixel(uint16_t *pixels, int x, int y, uint16_t color)
{
    if ((unsigned)x < WIDTH && (unsigned)y < WIDTH) pixels[y * WIDTH + x] = color;
}

static void span(uint16_t *pixels, int x, int y, int length, uint16_t color)
{
    if ((unsigned)y >= WIDTH || length <= 0) return;
    int end = x + length;
    if (x < 0) x = 0;
    if (end > WIDTH) end = WIDTH;
    for (; x < end; ++x) pixels[y * WIDTH + x] = color;
}

static void rectangle(uint16_t *pixels, int x, int y, int w, int h, uint16_t color)
{
    for (int row = 0; row < h; ++row) span(pixels, x, y + row, w, color);
}

static void ellipse(uint16_t *pixels, int x, int y, int rx, int ry, uint16_t color)
{
    for (int dy = -ry; dy <= ry; ++dy) {
        for (int dx = -rx; dx <= rx; ++dx) {
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry)
                pixel(pixels, x + dx, y + dy, color);
        }
    }
}

static int horizon(const uint8_t *heights, int x)
{
    int index = x / 10;
    return heights[index] + (heights[index + 1] - heights[index]) * (x % 10) / 10;
}

static void ridge(uint16_t *pixels, const uint8_t *heights, uint16_t color)
{
    for (int x = 0; x < WIDTH; ++x)
        for (int y = horizon(heights, x); y < WIDTH; ++y)
            pixels[y * WIDTH + x] = color;
}

static void pine(uint16_t *pixels, int x, int base, int height,
                 const palette_t *p, int snow)
{
    rectangle(pixels, x, base - height, 2, height, p->trunk);
    for (int level = 0; level < 3; ++level) {
        int top = base - height + level * height / 5;
        int section = height * (level + 3) / 8;
        for (int row = 0; row <= section; ++row) {
            int half = row * (height / 3 + level) / (section + 2);
            span(pixels, x - half, top + row, half * 2 + 2, p->tree);
            if (row > 1) span(pixels, x - half, top + row, half + 1, p->tree_light);
            if (snow && row < 3) span(pixels, x - half, top + row, half * 2 + 1, p->snow);
        }
    }
}

static void leafy_tree(uint16_t *pixels, int x, int base, const palette_t *p, int snow)
{
    rectangle(pixels, x - 1, base - 15, 3, 16, p->trunk);
    rectangle(pixels, x - 4, base - 12, 4, 2, p->trunk);
    ellipse(pixels, x - 4, base - 16, 7, 6, p->tree);
    ellipse(pixels, x + 4, base - 16, 7, 7, p->tree);
    ellipse(pixels, x, base - 21, 7, 7, p->tree_light);
    ellipse(pixels, x - 6, base - 18, 5, 4, p->tree_light);
    ellipse(pixels, x + 3, base - 24, 4, 3, snow ? p->snow : p->grass_light);
    if (snow) ellipse(pixels, x - 6, base - 20, 4, 2, p->snow);
}

static void cloud(uint16_t *pixels, int x, int y, int size, const palette_t *p)
{
    ellipse(pixels, x, y, size + 4, 3, p->cloud_shadow);
    ellipse(pixels, x - size / 2, y - 2, size / 2 + 2, 3, p->cloud);
    ellipse(pixels, x + 1, y - 4, size / 2 + 1, 5, p->cloud);
    ellipse(pixels, x + size / 2 + 2, y - 1, size / 2 + 1, 3, p->cloud);
    span(pixels, x - size, y + 1, size * 2 + 2, p->cloud_shadow);
}

static void cottage(uint16_t *pixels, const palette_t *p, int snow, int lit)
{
    rectangle(pixels, 78, 72, 16, 11, p->wall);
    rectangle(pixels, 90, 72, 7, 11, p->wall_shadow);
    rectangle(pixels, 91, 64, 3, 7, p->roof_shadow);
    span(pixels, 90, 64, 5, p->roof_shadow);
    for (int y = 65; y <= 73; ++y) {
        int half = y - 65;
        span(pixels, 85 - half, y, half * 2 + 2, p->roof);
        span(pixels, 87 + half, y, 4, p->roof_shadow);
        if (snow && y < 68) span(pixels, 85 - half, y, half * 2 + 3, p->snow);
        else if (y == 70 || y == 72) pixel(pixels, 84 + (y & 2), y, p->roof_shadow);
    }
    span(pixels, 76, 74, 23, p->roof_shadow);
    rectangle(pixels, 85, 77, 3, 6, p->trunk);
    rectangle(pixels, 79, 76, 4, 4, p->roof_shadow);
    rectangle(pixels, 80, 76, 2, 3, lit ? p->window : p->lake_top);
    rectangle(pixels, 92, 76, 3, 4, p->roof_shadow);
    rectangle(pixels, 93, 76, 1, 3, lit ? p->window : p->lake_top);
    span(pixels, 77, 83, 20, p->grass_dark);
    span(pixels, 84, 84, 5, p->wall_shadow);
    span(pixels, 83, 85, 7, p->wall_shadow);
    if (lit) {
        span(pixels, 79, 80, 4, blend(p->window, p->wall, 128));
        span(pixels, 81, 84, 3, blend(p->window, p->grass, 190));
    }
}

static unsigned random_bits(unsigned value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

void landscape_art_render(uint16_t *pixels, const landscape_scene_t *scene,
                          uint32_t elapsed_ms, int wind_mph,
                          int daylight_progress)
{
    if (pixels == NULL || scene == NULL) return;
    palette_t p = day_palette;
    int night = scene->phase == LANDSCAPE_PHASE_NIGHT;
    int twilight = scene->phase == LANDSCAPE_PHASE_TWILIGHT;
    int unknown = scene->phase == LANDSCAPE_PHASE_UNKNOWN;
    int unknown_weather = scene->condition == LANDSCAPE_CONDITION_UNKNOWN;
    if (night) p = night_palette;
    else if (twilight) {
        const unsigned light = scene->light > 256 ? 256 : scene->light;
        p = light < 128 ? blend_palette(&night_palette, &twilight_palette, light * 2)
                       : blend_palette(&twilight_palette, &day_palette, (light - 128) * 2);
    }
    else if (unknown) p = unknown_palette;

    int snow = scene->condition == LANDSCAPE_CONDITION_SNOW;
    int storm = scene->condition == LANDSCAPE_CONDITION_STORM;
    int rain = storm || scene->condition == LANDSCAPE_CONDITION_RAIN;
    int fog = scene->condition == LANDSCAPE_CONDITION_FOG;
    int overcast = rain || snow || fog || scene->condition == LANDSCAPE_CONDITION_CLOUDY;
    if (overcast) {
        unsigned shade = storm ? 160 : 100;
        p.sky_top = blend(p.sky_top, night ? HEX(0x283444) : HEX(0x5b7580), shade);
        p.sky_bottom = blend(p.sky_bottom, night ? HEX(0x4b5967) : HEX(0x9caeb2), shade);
        p.cloud = blend(p.cloud, p.sky_top, storm ? 155 : 70);
        p.cloud_shadow = blend(p.cloud_shadow, p.sky_top, 90);
    }

    /* A little ordered dithering keeps the short sky gradient soft in RGB565. */
    for (int y = 0; y < WIDTH; ++y) {
        unsigned amount = (unsigned)(y < 68 ? y : 68) * 256 / 68;
        uint16_t color = blend(p.sky_top, p.sky_bottom, amount);
        uint16_t next = blend(p.sky_top, p.sky_bottom, amount < 251 ? amount + 4 : 256);
        for (int x = 0; x < WIDTH; ++x)
            pixels[y * WIDTH + x] = ((x + y * 2) & 3) == 0 ? next : color;
    }

    if (night && !overcast && !unknown_weather) {
        for (unsigned i = 0; i < 32; ++i) {
            unsigned seed = random_bits(i + 1907);
            int x = (int)(seed % 114) + 3;
            int y = (int)((seed >> 8) % 46) + 5;
            if (x > 36 && x < 81 && y < 39) continue;
            uint16_t star = (i % 5) == 0 ? HEX(0xe0e6cf) : HEX(0x8199ae);
            pixel(pixels, x, y, star);
        }
    }

    if (!unknown && !overcast && !unknown_weather) {
        int sun_y = 37;
        if (daylight_progress >= 0) {
            int distance = daylight_progress > 500 ? daylight_progress - 500 : 500 - daylight_progress;
            if (distance > 500) distance = 500;
            sun_y = 33 + distance * 14 / 500;
        }
        if (night) {
            ellipse(pixels, 91, 34, 7, 7, HEX(0xe1e5ce));
            ellipse(pixels, 93, 32, 6, 6, blend(p.sky_top, p.sky_bottom, 119));
        } else {
            ellipse(pixels, 91, twilight ? 47 : sun_y, 7, 7,
                    twilight ? HEX(0xf4c59f) : HEX(0xf7e5a9));
        }
    }

    if (wind_mph < 0) wind_mph = 0;
    if (wind_mph > 60) wind_mph = 60;
    unsigned drift = elapsed_ms / (unsigned)(350 - wind_mph * 4);
    int clouds = overcast ? 7 : (scene->condition == LANDSCAPE_CONDITION_PARTLY_CLOUDY ? 4 : 2);
    for (int i = 0; i < clouds; ++i) {
        int x = (int)((drift + (unsigned)i * 47 + 11) % 176) - 28;
        int y = 41 + (i % 3) * 6;
        cloud(pixels, x, y, overcast ? 15 : 10, &p);
    }

    static const uint8_t distant[] = {64, 61, 58, 61, 55, 49, 57, 55, 62, 56, 58, 53, 59};
    static const uint8_t near[] = {68, 64, 65, 61, 65, 67, 64, 60, 64, 67, 61, 64, 62};
    static const uint8_t meadow[] = {74, 72, 69, 70, 73, 76, 76, 74, 72, 71, 69, 71, 73};
    static const uint8_t shore[] = {83, 80, 82, 84, 82, 86, 88, 87, 85, 83, 83, 80, 81};
    ridge(pixels, distant, p.ridge_back);
    ridge(pixels, near, p.ridge_front);
    ridge(pixels, meadow, snow ? p.snow_shadow : p.meadow);
    ridge(pixels, shore, snow ? p.snow : p.grass);

    if (snow) {
        for (int x = 40; x < 60; ++x) {
            int y = horizon(distant, x);
            if (y < 55) span(pixels, x, y, 1, p.snow_shadow);
            if (y < 52) pixel(pixels, x, y + 1, p.snow_shadow);
        }
    }

    /* The two banks open into a lake; their uneven edges remain fixed as water moves. */
    for (int y = 88; y < WIDTH; ++y) {
        int left = 47 - (y - 88) * 2;
        int right = 73 + (y - 88) * 2;
        if (left < 9) left = 9 - (y - 108) / 2;
        if (right > 112) right = 112 + (y - 108) / 3;
        if ((y & 3) == 0) left -= 2;
        if ((y & 7) == 2) right += 2;
        unsigned amount = (unsigned)(y - 88) * 256 / 32;
        uint16_t water = blend(p.lake_top, p.lake_bottom, amount);
        span(pixels, left, y, right - left, water);
        span(pixels, left - 1, y, 2, snow ? p.snow_shadow : p.grass_dark);
        span(pixels, right, y, 2, snow ? p.snow_shadow : p.grass_dark);
    }
    unsigned ripple_step = elapsed_ms / 850;
    for (unsigned i = 0; i < 34; ++i) {
        unsigned seed = random_bits(i + 461);
        int y = 91 + (int)(seed % 28);
        int spread = (y - 86) * 2;
        if (spread > 50) spread = 50;
        int x = 60 - spread + (int)((seed >> 9) % (unsigned)(spread * 2));
        int shift = (int)((ripple_step + i) % 5) - 2;
        int length = 2 + (int)((seed >> 17) % 7);
        uint16_t color = blend(p.lake_top, p.reflection, i % 3 == 0 ? 115 : 55);
        if (x + length < 60 + spread) span(pixels, x + shift, y, length, color);
    }

    for (unsigned i = 0; i < 40; ++i) {
        unsigned seed = random_bits(i + 37);
        int x = (int)(seed % WIDTH);
        int y = 76 + (int)((seed >> 9) % 12);
        if (y > horizon(shore, x) && !(x > 73 && x < 101))
            span(pixels, x, y, 2 + (int)(i % 3), snow ? p.snow_shadow : p.grass_light);
    }

    pine(pixels, 6, 85, 28, &p, snow);
    pine(pixels, 17, 81, 25, &p, snow);
    pine(pixels, 111, 82, 21, &p, snow);
    pine(pixels, 119, 85, 28, &p, snow);
    cottage(pixels, &p, snow, night || twilight || storm);
    leafy_tree(pixels, 31, 90, &p, snow);

    rectangle(pixels, 42, 87, 1, 5, p.wall_shadow);
    rectangle(pixels, 49, 86, 1, 5, p.wall_shadow);
    span(pixels, 41, 88, 11, p.wall_shadow);
    span(pixels, 39, 91, 16, p.trunk);
    span(pixels, 42, 92, 16, p.wall_shadow);
    rectangle(pixels, 44, 93, 1, 3, p.trunk);
    rectangle(pixels, 55, 93, 1, 3, p.trunk);

    for (int i = 0; i < 7; ++i) {
        int x = 3 + i * 2;
        int y = 108 + (i * 7) % 10;
        rectangle(pixels, x, y, 1, WIDTH - y, p.tree);
        pixel(pixels, x - 1, y + 3, p.tree_light);
        pixel(pixels, x + 1, y + 5, p.tree_light);
        rectangle(pixels, 116 - i * 2, y + 2, 1, WIDTH - y, p.tree);
    }

    if (fog) {
        for (int y = 55; y < 94; ++y) {
            int density = (y >= 64 && y <= 69) ? 90 : ((y >= 80 && y <= 83) ? 65 : 0);
            if (!density) continue;
            for (int x = 0; x < WIDTH; ++x)
                pixels[y * WIDTH + x] = blend(pixels[y * WIDTH + x], p.cloud, (unsigned)density);
        }
    }

    if (rain || snow) {
        unsigned motion = elapsed_ms / (snow ? 160u : 55u);
        unsigned count = storm ? 42 : (snow ? 34 : 27);
        uint16_t precipitation = snow ? p.snow : blend(p.cloud, p.lake_top, 110);
        for (unsigned i = 0; i < count; ++i) {
            unsigned seed = random_bits(i + 993);
            int y = (int)(((seed >> 7) + motion * (snow ? 1u : 3u)) % 132) - 6;
            int x = (int)((seed + motion / (snow ? 4u : 2u) + (unsigned)(y + 6) / 7) % WIDTH);
            if (x > 38 && x < 81 && y < 34) continue;
            pixel(pixels, x, y, precipitation);
            if (snow) {
                if (i % 5 == 0) {
                    pixel(pixels, x + 1, y, precipitation);
                    pixel(pixels, x, y + 1, precipitation);
                }
            } else {
                pixel(pixels, x, y + 1, precipitation);
                if (i % 3 == 0) pixel(pixels, x - 1, y - 1, precipitation);
            }
        }
    }
}
