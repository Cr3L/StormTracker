# StormTracker

[Download the one-page product sheet (PDF)](docs/StormTracker-Product-Sheet.pdf)

Custom firmware for the **BigTreeTech Knomi V1** — an ESP32-WROVER-E board with
a 240×240 round GC9A01 display, originally a Klipper printer monitor.

The stock Knomi firmware has been replaced and is not being kept.

It is an **animated window into the weather**: a small lakeside landscape whose
clouds, precipitation, and lighting follow local conditions and daylight.
The original storm countdown remains available as a separate boot mode.

Display, LVGL, a serial console, Wi-Fi, a network-set clock, firmware updates
over the air with automatic rollback, a National Weather Service client, and the
screen that shows all of it.

Built on ESP-IDF's native `esp_lcd` stack rather than Arduino/TFT_eSPI, so the
concepts carry over to other ESP32 boards. Only the pin numbers came from BTT.

## Animated landscape

The default screen is now `BOOT_MODE_LANDSCAPE`: hills, a lake, trees, and a
cottage, drawn as native pixel art for the round display. Clouds drift, water
ripples, rain falls, and snow covers the banks. Storms darken the sky without
flashing. Lighting transitions through dawn and dusk using the existing local
sunrise/sunset calculations; the cottage windows light up at night.

The clock and measured temperature sit above the scene. A short condition label
sits below it. Recent station observations take priority; when only a valid
current forecast is available, the scene is explicitly marked `Forecast`.
Missing or stale data never defaults to clear weather. Severe warnings still
take over the screen, and other alerts take the condition line.

The Windows firmware build and 19 native desktop scenarios pass. OTA
installation, boot, automatic image confirmation, Wi-Fi, clock sync, and live
weather fetching were also verified on the Knomi. The owner confirmed normal
colors, text fitting inside the round screen, and visible animation.
See [the landscape notes](docs/landscape.md)
for behavior, memory use, and the Windows build/preview commands.

## Existing hardware foundation

Verified on hardware:

- **Display** — colour, geometry, orientation and refresh all confirmed.
- **LVGL 9** on top of it via `esp_lvgl_port`.
- **The zoo** — four pixel-art animals walking in turn, from PNGs in `assets/`
  compiled to C sprite tables. Kept as a working screen and selectable through
  `BOOT_MODE`, but it is no longer what the board boots into.
- **A serial console** on the log UART, and **Wi-Fi credentials in NVS**
  entered through it. Confirmed surviving a power cycle.
- **Wi-Fi** — the station reads those credentials at boot, associates, and
  reconnects with exponential backoff. Failure paths confirmed too: wrong
  password, absent network, and credentials changed on a live station.
- **The clock** — SNTP from `pool.ntp.org` a few seconds after the address
  arrives, with the timezone stored in NVS through a `tz` command.
- **A partition table sized for OTA**, laid out before Wi-Fi on purpose — and
  now used: the board installs firmware over the network into whichever slot it
  is not running from. A new image is on probation until it reaches the network,
  and reverts on the next reset if it never does, so a bad update costs a reboot
  rather than a cable. Verified by installing an image built deliberately not to
  confirm itself and watching it roll back.
- **Stack smashing protection**, turned on when the HTTP client arrived and the
  reason to defer it expired.
- **The weather** — a twelve-hourly forecast from the US National Weather
  Service over TLS, reporting how close the next thunderstorm is. Coordinates
  are set at the console and stored in NVS.
- **Severe weather alerts** — active NWS watches and warnings for those
  coordinates. A *warning* takes over the whole screen in crimson; a *watch*
  gets a line. Verified against a live Flood Warning in Texas by pointing the
  board at it.
- **Live observations** — the latest reading from the nearest reporting
  station: temperature, dewpoint, humidity, wind and gusts, visibility and
  barometric pressure, converted from the service's SI to US units at parse
  time. Nearby stations are found by reading the head of a 75 kB list, which is
  ordered nearest-first.
- **A barometric trend the board computes itself** — pressure change over three
  hours, kept in RAM and keyed on each station's own reading time, so the poll
  interval does not affect what "three hours" means. The one figure here that is
  not read off a wire.
- **Sunrise and sunset** — NOAA's sunrise equation from the stored coordinates.
  No network, no failure mode, verified against the US Naval Observatory to
  within 42 seconds across the continental US.
- **The storm screen** — a rim dial whose fill is nearness times probability, so
  it is dramatic only when a storm is both close and likely, above the measured
  temperature, an ETA, and live readings: dewpoint, pressure with a trend arrow,
  wind or gusts, and the next sun event.

These services now also feed the landscape. This list records the established
foundation and original storm screen; the new landscape's verification is
described separately above.

## Original storm dial

Select `BOOT_MODE_STORM` to use the previous screen. Its central question is:
**is a thunderstorm coming, and how soon?** The following dial descriptions apply
to that mode, not to the animated landscape.

### How a storm is found

The forecast is the NWS twelve-hourly product — periods named "This Afternoon",
"Tonight", "Wednesday". Each carries a short text and a probability of
precipitation. A period counts as stormy when its text mentions *thunder* at
all; the substring match is deliberately loose, because the service writes
"Chance Showers And Thunderstorms", "Scattered Thunderstorms" and "Severe
Thunderstorms" and the board should not be a list of the phrasings it happens to
know.

The **first** such period within two days sets the state:

| State | Means | Colour |
|---|---|---|
| `none` | no thunder in the window | the device green |
| `possible` | thunder mentioned, probability under 30% | yellow |
| `likely` | thunder mentioned, 30% or more | deep orange |
| `now` | the *current* period is the stormy one | red |

Two days, not the full week the API returns: a storm on Thursday is a fact
about Thursday, not something a desk ornament should be lit up about.

### The dial

The rim arc fills with **nearness × probability**, not with either alone.

Nearness is linear over a 48-hour horizon and pinned to full once the storm
period has begun. Filling on nearness alone was the first version and it was
wrong across a room: a 19% chance five hours out filled almost the entire ring
and said *imminent* about something that would probably not happen. Multiplying
by probability means the ring is only dramatic when a storm is both close and
likely, and colour still carries probability by itself, so a distant certainty
is not mistaken for calm.

An unstated probability is treated as **certain**, not as zero. NWS omits the
field on some periods, and a period whose text says "thunderstorms" must not
read as a clear sky because a number was missing.

### Reading the face

Top to bottom, on a screen about eleven characters wide:

```
        ╭─────────────╮        ← the rim arc: nearness × probability
        │     2 h     │        ← headline, 28pt, in the storm colour
        │  Tonight 61%│        ← the period's own name, and probability of rain
        │             │
        │  Severe T…  │        ← watch line, only when one is active
        │  84°F dew 68│
        │  1013 mb ↓  │
        │  gust 24    │
        │   7:42 PM   │        ← the clock, small and deliberately dateless
        ╰─────────────╯
```

**The headline is a countdown, not a label.** `2 h` is the time until the storm
period begins; over a day out it becomes `1 d 6 h`. Two other values appear
there: `NOW` when the current period is the stormy one, and `CLEAR` when
nothing is in the window. `--` means no forecast has arrived yet — a cold boot
takes about a minute, and a blank middle would be indistinguishable from a
screen that has stopped updating.

**The subtitle is the other half of the same sentence.** The headline answers
*how long*; this says what the question was. It carries the period name NWS
itself uses — "Tonight", "Monday Night" — and the probability, and deliberately
never says the word "storm": the arc, the colour and the countdown have all said
that already.

**The percentage is a chance of rain, not of thunder.** It is
`probabilityOfPrecipitation` for a period whose text mentions thunder. See the
last subsection — the true thunder probability is not available at this size.

Then the three evidence lines. The headline claims a storm is coming; these are
why anyone should believe it, ordered as that argument:

| Line | | |
|---|---|---|
| `84°F dew 68` | **fuel** | the *measured* temperature, not the forecast's, and dewpoint |
| `1013 mb ↓` | **machinery** | station pressure, with the board's own three-hour trend |
| `gust 24` / `wind 12` | **machinery** | mph; a gust displaces the steady wind whenever one is reported |
| `7:42 PM` | **context** | the next sun event, or the clock |

Four things about those are worth knowing:

- **Dewpoint rather than humidity**, because 58% means nothing without a
  temperature beside it while a 68° dewpoint means "muggy" to anybody.
- **The temperature is the observation, not the forecast**, and the two differ
  by several degrees routinely — one is what the forecast office expects for
  this half of the day, the other is what an instrument recorded a few miles
  away twenty minutes ago.
- **The pressure arrow is a direction, not a figure.** The number is three
  characters this screen does not have, and the direction is the whole of what
  a falling barometer means. It appears only past ±0.5 mb over three hours, and
  not at all until two hours of readings exist — the history lives in RAM, so
  every restart and every OTA starts that clock over.
- **A gust displaces the wind** rather than joining it: a gust front ahead of a
  storm is exactly the moment this screen is worth looking at, and on a quiet
  day there is no gust to show.

**Anything the station did not report is simply absent**, never a dash. Fields
arrive individually null on ordinary days. The console's `wx` table has a fixed
shape so a gap is visibly a gap; the glass has eleven characters a line and
should spend them on what is known.

The readings, the clock and the sun are all drawn **before** the forecast is
checked, so they survive a forecast fetch that is still failing. The sun line in
particular needs no network at all and is the last thing left when everything
else has gone.

Those three lines replaced eight rotating thunderstorm facts. The facts were
charming for a day; on a device that is on all day they get memorised, and
memorised text stops being read.

### Alerts

Active NWS watches and warnings are polled every five minutes, against thirty
for the forecast — an alert half an hour late has missed the weather it was
warning about, and a forecast has not.

The two are treated differently on purpose, because they mean different things.
A **Warning** means it is happening: it takes over the whole screen in crimson.
A **Watch** means conditions are favourable hours out: it gets one line, and the
rest of the screen carries on.

### What it cannot do yet

The forecast text is prose, so "Chance Showers And Thunderstorms" yields a
*precipitation* probability and no separate thunder probability. NWS publishes
`probabilityOfThunder` as a real hourly field, but only in the raw gridpoint
document — 292 kB, which needs a streaming parser rather than the fetch-then-
parse this uses. It is [open item 12](docs/open-items.md).

## The weather service

Why `api.weather.gov` is worth naming: it is free, needs no
API key, and is run by the agency that issues the forecasts. The cost is that
it covers **the United States only**, and that it is a two-request API — the
`/points` endpoint returns which forecast grid a coordinate falls in, and the
forecast lives at a URL it hands back.

## Hardware

| | |
|---|---|
| MCU | ESP32-WROVER-E (ESP32-D0WD-V3, dual core, 8 MB PSRAM) |
| Flash | 16 MB |
| Display | GC9A01, 240×240 round, 4-wire SPI |
| USB serial | CH340 (`/dev/ttyUSB0` on Linux) |
| Buttons | BOOT only (the gold button next to USB-C is RESET) |

### Pinout

Taken from BigTreeTech's own V1 firmware
([`src/pinout_knomi_v1.h`](https://github.com/bigtreetech/KNOMI/blob/firmware/src/pinout_knomi_v1.h)),
not measured or guessed. These are V1-only — the V2 is an ESP32-S3 with a
different map.

| Signal | GPIO | Note |
|---|---|---|
| MOSI | 23 | native VSPI IOMUX |
| SCLK | 18 | native VSPI IOMUX |
| CS | 5 | native VSPI IOMUX |
| DC | 19 | |
| RST | 4 | |
| Backlight | 2 | PWM via LEDC; also a boot strapping pin |
| BOOT | 0 | |

SPI3 (VSPI) is used because those three pins are its IOMUX defaults; routing
through the GPIO matrix instead would cap the usable clock.

## Building

Requires ESP-IDF 5.5+. Source the environment in every new shell:

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32     # first time only
idf.py build
```

## Updating over the air

**This is the normal way to update the board.** Serve the build directory from
the machine that compiled it:

```sh
cd build && python3 -m http.server 8000
```

Then, from the console:

```
panda> ota http://192.168.1.50:8000/pandadeath.bin
panda> reboot
```

(`panda>` is printed by the board, not typed.) Or without a terminal:

```sh
python3 tools/console.py --timeout 180 "ota http://192.168.1.50:8000/pandadeath.bin"
```

`--timeout` is required — the default 5 s ceiling is shorter than the download.

The image is written to whichever of the two app slots is *not* running, so a
failure part way through costs nothing; the running firmware is never touched.
`ota_status` reports the running slot, and prints a `next:` line when an
installed update is waiting for a reboot.

### Rollback

A freshly installed image boots **on probation**. It confirms itself only once
it obtains an IP address; if it never does, the next reset reverts to the slot
it replaced.

That criterion is deliberate. The state this must never leave the board in is
"new image runs but cannot reach the network", because no further update can fix
it — that one costs the USB cable, which is the thing OTA exists to avoid. The
accepted cost is the mirror image: a good update installed while the router
happens to be down gets rolled back for no reason, reverting to firmware that
also cannot reach the network. Nothing is lost but the update.

## Flashing by wire — first boards and recovery

Needed for a board that has never run this firmware, and if both slots are ever
unbootable. Not part of the normal loop.

**This board has no working auto-reset.** DTR/RTS are not wired to EN/GPIO0, so
esptool cannot enter download mode on its own. Every flash fails with
`Failed to connect to ESP32: No serial data received` unless the board is put
into the bootloader by hand:

> **unplug USB → hold BOOT → replug while still holding → hold 2–3 s → release**

Then:

```sh
idf.py -p /dev/ttyUSB0 flash
```

Holding BOOT on a board that is already running does nothing useful — it has to
be held across an actual power-on. After flashing, **physically replug again** to
run the new app.

### Two different replugs

These are easy to conflate and the symptoms are confusing when you do:

| Goal | Gesture |
|---|---|
| **Flash** new firmware | unplug → **hold BOOT** → replug → hold 2–3 s → release |
| **Run** what is flashed | unplug → replug, **touching nothing** |

Holding BOOT during what was meant to be a plain replug leaves the board in
download mode: the app never starts, so the screen stays completely dark and
nothing is printed on serial. A powered board that is silent *and* dark is
almost always this, not a crash. Confirm with:

```sh
python -m esptool --chip esp32 -p /dev/ttyUSB0 --before no_reset chip_id
```

If that connects without the BOOT sequence, the board was already sitting in the
bootloader.

### Watching the serial log

`idf.py monitor` needs a TTY. For scripted capture use:

```sh
python3 tools/capture.py 60     # seconds
```

It reopens the port when the device reappears, because unplugging destroys the
tty and kills any handle held across it — a capture started before the replug
would otherwise just die.

Both `tools/capture.py` and `tools/console.py` import pyserial, which exists only
in the IDF virtualenv. Source `export.sh` first, or call that interpreter
directly.

## The console

The firmware runs an `esp_console` REPL on the same UART as the log, prompting
`panda>`. Open it with `idf.py monitor` and type `help`.

Sharing the UART with the log is a real constraint, not an accident: anything
that prints on a timer walks over whatever is being typed. Hence the rule that
**nothing periodic logs above `DEBUG`**, which is compiled out at the current
log level.

`console.c` owns the REPL and **no commands at all** — only the loop that
registers them. Commands live with the module whose state they touch: `wifi_*`
in `wifi_cmd.c` next to the credential store, `time`/`tz` in `time_cmd.c`,
`ota`/`ota_status`/`reboot` in `ota_cmd.c`. A console that `#include`d every
feature it can drive would depend on all of them just to start. `main.c`
registers commands before starting the REPL, so the prompt never offers one that
does not exist yet.

| Command | |
|---|---|
| `wifi_set` / `wifi_show` / `wifi_clear` | credentials, below |
| `wifi_status` | state, IP and signal strength |
| `time` | UTC, local time, zone, and when the clock was last set |
| `tz [<posix tz>]` | show or set the timezone, e.g. `tz EST5EDT,M3.2.0,M11.1.0` |
| `ota <url>` | install firmware into the inactive slot |
| `ota_status` | running slot, version, and whether it is confirmed |
| `reboot` | restart |
| `wx` | everything: readings, sun, forecast and alerts, in one table |
| `weather_refresh` | ask the weather task to fetch now |
| `loc [<lat> <lon>]` | show or set the forecast location |

Alerts are polled every five minutes and the forecast every thirty, on the same
task. An alert half an hour late has missed the weather it was warning about; a
forecast has not.

For scripted use without a terminal:

```sh
python3 tools/console.py wifi_show "wifi_set TestNetwork hunter2" wifi_show
```

Each argument is one command line. **Never pass a real credential this way** —
arguments land in shell history and in the process list. Real ones get typed by
a human at `idf.py monitor`.

## Wi-Fi credentials

| Command | |
|---|---|
| `wifi_set <ssid> [password]` | store; quote an SSID containing spaces |
| `wifi_show` | print the SSID, and the password's *length* only |
| `wifi_clear` | erase both |

They go to NVS under the `pandadeath` namespace and survive a power cycle. This
repo is public, so **there is deliberately no API anywhere in the tree that takes
a credential from a compiled-in constant** — the console is the only way one gets
in.

The password is never printed, logged or echoed. `wifi_set` also calls
`console_forget_history()`, because clearing the local buffer is not enough: the
REPL's line editor has already retained the whole line, so a later up-arrow would
hand the password back. That is the layer the secret actually lives in.

## Flash layout

[`partitions.csv`](partitions.csv) is sized for OTA — two 4 MB app slots against
a ~1.5 MB image, plus an 8 MB `storage` partition not yet mounted. The slack is
not speculation about growth; it is because this board is *awkward to flash by
wire*, so an over-the-air path removes the manual gesture from the normal loop.

It was laid out **before** Wi-Fi on purpose, two sessions before anything could
use it. Changing the map once `nvs` and `otadata` hold real data means erasing
the chip, which takes the credentials with it.

## Panel quirks

Two settings could not be resolved by reading code and are isolated at the top of
[`main/display.c`](main/display.c) as one-line changes:

- **`PANEL_INVERT`** — on here. BTT's TFT_eSPI setup drives the same panel with
  inversion *off*; starting from their value rendered an exact colour complement
  (black as white, green as magenta). The two stacks don't share a baseline.
- **`PANEL_BGR_ORDER`** — on. Flip if red and blue swap.

`LCD_PIXEL_CLOCK_HZ` is 40 MHz. BTT runs this panel at 80 MHz, so there is
headroom, but a marginal signal shows up as sparkle or a dead panel rather than a
clean error.

**Orientation** is `PANEL_MIRROR_X` / `PANEL_MIRROR_Y` in
[`main/display.h`](main/display.h). The panel's native scan order is a
*reflection*, not a rotation — so it needs an odd number of axis flips to
correct. Mirroring both axes looks like an obvious fix and is wrong: that is a
180° rotation, which leaves text upright but reading backwards, as if seen from
behind the screen.

These constants are public rather than private to `display.c` because two things
apply them and they must agree. `display_init()` mirrors the panel so the
LVGL-free self-test is oriented correctly, and `esp_lvgl_port` mirrors it again
from its own rotation config when LVGL attaches. Zeroing the latter does *not*
mean "inherit" — it actively mirrors nothing and silently undoes the driver.

## Verifying changes on hardware

`BOOT_MODE` in [`main/boot_mode.h`](main/boot_mode.h) picks what the build brings
up. The three values are rungs of one ladder, not independent options — each adds
a layer to the one below, so stepping *down* it is how you find which layer owns
a fault:

| Value | Brings up | Useful when |
|---|---|---|
| `BOOT_MODE_SELFTEST` | the panel alone, no LVGL | a wiring, SPI or panel-config fault |
| `BOOT_MODE_TEST_SCREEN` | LVGL, no assets | LVGL port or rendering fault |
| `BOOT_MODE_ZOO` | LVGL plus the sprite tables | a sprite or asset fault |
| `BOOT_MODE_STORM` | LVGL plus the network | the original storm dial |
| `BOOT_MODE_LANDSCAPE` | LVGL plus the network | the default animated landscape |

`STORM` is a peer of `ZOO`, not a rung above it — worth stating because the
ladder's whole value is that stepping down changes one thing. `ZOO` adds sprites
to LVGL; `STORM` adds the network and uses no sprites at all, so going from one
to the other swaps two variables and bisects nothing. To isolate a fault under
the storm screen, step to `TEST_SCREEN`, which is genuinely below both.

The self-test is a colour walk, a **static** geometry frame held 20 s, then a
motion loop, talking straight to the panel with no LVGL involved. It is how a
second board should be brought up.

The static frame exists because photographing a *moving* pattern is misleading —
phone rolling shutter smears a travelling shape along its axis of motion, which
looks exactly like the shape being the wrong size. Judge geometry only from the
static frame. Note it is symmetric about both axes, so it cannot catch a
mirroring fault; add an asymmetric element if that is what you are chasing.

Cameras also blow out bright colours against a dark background (green reads
white, red reads yellow) and LCD black leaks blue at full backlight, so **colour
correctness is an eyeball judgement, not a photo judgement.**

## Layout

```
main/
  display.c/h    panel hardware: pins, SPI, backlight, orientation, rect fills
  ui.c/h         LVGL port bring-up; picks which screen gets built
  ui_storm.c/h   the storm screen: the dial, the countdown and the readings
  ui_landscape.c/h animated landscape, live captions and alert takeover
  landscape.c/h portable weather freshness, condition and daylight rules
  landscape_art.c/h native RGB565 pixel-art scene and animation
  ui_zoo.c/h     the zoo screen: four animals, cycling
  ui_test.c/h    arc + counter, kept as a known-good LVGL reference
  zoo_sprites.*  generated by tools/gen_sprites.py — never edit by hand
  selftest.c/h   raw panel exercise, bypasses LVGL entirely
  boot_mode.h    which of the three the build brings up
  console.c/h    the REPL on the log UART; owns no commands, only registration
  storage.h      the shared NVS namespace; keys stay with the module that owns them
  net.c/h        esp_netif + the default event loop; "is there an address yet"
  wifi_cmd.c/h   the wifi_* console commands
  wifi_creds.c/h Wi-Fi credentials in NVS — the only way one gets in
  wifi_sta.c/h   the station: associates, reconnects with backoff
  time_sync.c/h  SNTP, and the timezone in NVS; starts on got-IP
  time_cmd.c/h   the time and tz console commands
  ota.c/h        firmware updates, and the rollback confirmation
  ota_cmd.c/h    the ota, ota_status and reboot console commands
  weather.c/h    NWS forecast, alerts and observations; coordinates in NVS
  weather_cmd.c/h the wx table, weather_refresh and loc commands
  sun.c/h        sunrise and sunset from coordinates; no state, no network
  main.c         boot path: NVS -> commands -> console -> display -> UI -> fade
                 -> net -> ota -> time -> wifi -> weather
assets/          sprite PNGs; the source of truth for the art
tools/
  capture.py     serial capture that survives a USB replug
  console.py     drives the console without a terminal
  gen_sprites.py assets/*.png -> zoo_sprites.c (needs Pillow, system python)
docs/
  open-items.md  work deferred deliberately, with the reason and the trigger
```

The console comes up **before** the display. It owns no hardware the display
wants, and it is the only way to repair a board whose credentials are wrong — a
UI failure should not take the repair path down with it.

The three network modules — `ota`, `time_sync`, `wifi_sta` — can be started in
any order. Each one that waits on an address also checks for an address that
already exists, so none of them depends on running before or after the radio.
That is why `net.c` exists: `esp_netif_init()` and the default event loop belong
to no feature, and whichever module created them would silently have become the
one the others had to follow.

`ui.c` touches no pin, clock or panel command — it depends on `display.h` only
for the panel dimensions and handles. That seam is what should carry to a second
board; the screen *layouts* will not, since they are tuned for a 240×240 round
panel.

Anything that is a property of the physical glass — inversion, BGR order,
mirroring — belongs in the display layer, so the LVGL-free self-test sees the
same panel the UI does.

The generated sprite C is committed so that a firmware build needs nothing beyond
the IDF. The PNGs remain the source of truth; regenerate rather than editing the
output.

Working on this? [CLAUDE.md](CLAUDE.md) covers *how* — the build model, the
verification rules, the conventions. This file covers what the thing is.

## What is deliberately unfinished

[`docs/open-items.md`](docs/open-items.md) carries each deferred item with the
reason it waits and the condition that should bring it back. **Read it before
planning work** — it is where "why hasn't this been done yet" is answered.
