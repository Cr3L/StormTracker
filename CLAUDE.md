# Working on PandaDeath

The current default screen is the animated weather landscape. See
`docs/landscape.md` for its behavior and Windows host verification. Older zoo
and storm-screen notes below describe the project's development history.

Firmware for a BTT Knomi V1 (ESP32-WROVER-E, 240×240 round GC9A01 panel).
See [README.md](README.md) for hardware facts, pinout and panel quirks — this
file is about *how to work on it*, not what it is.

Michael is new to embedded development and is building fundamentals
deliberately, smallest verifiable step first. Explain what a thing *is*
alongside the command that does it — the build model, what a log line means,
why a peripheral needs a given setup. Do not just hand over commands.

## The two USB gestures — get this right

This board's CH340 has no working auto-reset, so **both** of these are manual
and they are different:

| Goal | Gesture |
|---|---|
| **Flash** firmware | unplug → **hold BOOT** → replug → hold 2–3 s → release |
| **Run** what's flashed | unplug → replug, **touching nothing** |

Always ask Michael to perform the gesture and wait for confirmation before
running `idf.py flash`. Never assume auto-reset works — it does not.

**`erase-flash` needs its own gesture.** It is tempting to reason that since the
closing "hard reset via RTS" does nothing on this board, the chip stays in
download mode and an erase can be chained straight into a flash on one gesture.
It cannot: the erase leaves the chip out of download mode, and the flash that
follows fails to connect. Budget two gestures, and note the window between them
is the one moment the board has no firmware at all — dark screen and silent
serial there is the erase having worked, not a brick.

Holding BOOT during what was meant to be a plain replug leaves the board in
download mode: **screen completely dark and serial completely silent.** That
combination reads like a crash but is not. Confirm with:

```sh
python -m esptool --chip esp32 -p /dev/ttyUSB0 --before no_reset chip_id
```

If it connects *without* the BOOT gesture, the board was parked in the
bootloader. This cost half an hour once; don't rediscover it.

## Build

```sh
. ~/esp/esp-idf/export.sh      # required in every new shell
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

### Prefer OTA — the wired flash is now the fallback, not the loop

The board updates itself over Wi-Fi. Michael starts a static file server
(this is the one part an agent cannot do — binding a listening port is
refused by the sandbox), and everything after it is agent-driven:

```sh
cd ~/Desktop/Babel/PandaDeath/build && python3 -m http.server 8000
```

**It outlives its tab too**, exactly like `idf.py monitor` — closing the
terminal leaves it holding port 8000, and the next start dies with
`OSError: [Errno 98] Address already in use`. That error means the server is
already running, not that something is broken; it serves from disk per request,
so it is already serving whatever was last built. Check before restarting:

```sh
curl -s -o /dev/null -w "%{size_download}\n" http://<fedora-ip>:8000/pandadeath.bin
```

```sh
python tools/console.py --timeout 180 "ota http://<fedora-ip>:8000/pandadeath.bin"
python tools/console.py "ota_status"     # next: shows an update waiting
python tools/console.py "reboot"
```

`--timeout` is not optional for `ota`: the default ceiling is 5 s against a
download of half a minute.

**Two USB gestures per session became zero.** Reserve the wired flash for
first flash of a board, and for recovery if both slots ever end up unbootable.

An installed image is on probation until it reaches the network — see
`ota.c`. If it never does, the next reset reverts it. That means **a bad
update costs a reboot, not a cable**, and it also means `ota_status` saying
"on probation" long after a reboot is a real signal, not noise.

`/dev/ttyUSB0` is the board. `/dev/ttyACM0` on this machine is the laptop's
fingerprint reader — not the board.

Changing `sdkconfig.defaults` does nothing on its own; `sdkconfig` already
exists and wins. Delete `sdkconfig` to pick up new defaults.

## Verifying changes

**Never push without asking first.** Committing locally is fine and does not
need permission. Publishing does: before `git push`, say what is in the
commits, what each change has actually been verified against — hardware, or
only a clean build — and what is still outstanding. Then wait. The distinction
between "verified on the device" and "compiles and reads correctly" is the
thing being checked for, so do not blur it.

**Every behavioural change gets flashed and checked on hardware before it is
committed.** Two of today's bugs — a mirrored display and a silently overridden
panel setting — compiled cleanly, reviewed cleanly, and were only caught on the
device.

## Do it yourself wherever the hardware allows

**Every step handed to Michael is a step that can go wrong in a way neither of
you can see.** Prefer the tool an agent can drive:

| Need | Use | Not |
|---|---|---|
| ask the running firmware something | `tools/console.py` | "type this at the monitor" |
| boot log across a replug | `tools/capture.py` | "paste what it printed" |
| is the board in the bootloader? | `esptool --before no_reset chip_id` | "does the screen look dark?" |

Reserve Michael for what genuinely cannot be automated: the two USB gestures,
colour and geometry judgements, and typing a real credential. Everything else
you should do and then *report*.

This is not politeness, it is diagnosis. A session spent on "nothing happens
when I press Enter" ended with the board having been parked in the bootloader
the whole time — invisible from a description, one command to confirm. When
something is wrong, the fastest question is always the one you can answer
yourself.

Say which machine a command is for. `$` is Fedora, `panda>` is the board, and
`panda>` is **printed by the board, not typed** — that has been misread as
something to type. Commands aimed at the wrong prompt produce `command not
found`, which reads as a broken tool rather than a wrong window.

### The port is exclusive, and monitors outlive their tabs

`idf.py monitor` locks `/dev/ttyUSB0`; nothing else — not `console.py`, not
`esptool`, not a second monitor — can touch the board while it runs. **Closing
the terminal tab does not end it. Only Ctrl-] does.** Stale monitors from
hours earlier have now blocked the port three times, presenting as
`[Errno 11] Could not exclusively lock port`.

Do not guess at which process is holding it:

```sh
fuser -v /dev/ttyUSB0
pkill -f "esp_idf_[m]onitor"   # brackets, or the pattern kills your own shell
```

Never start a monitor and an agent-side tool in the same breath — check the
port is free first, and prefer taking it back over asking Michael to close
something.

## Serial

`idf.py monitor` needs a TTY, so it must be run by Michael, not by an agent.
For scripted capture use `tools/capture.py`, which reopens the port across a
replug.

It imports pyserial, which exists **only in the IDF virtualenv** — a bare
`python3 tools/capture.py` dies on `ModuleNotFoundError: No module named
'serial'`. Either source `export.sh` first, or call the interpreter directly:

```sh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python tools/capture.py 30
```

Start it *before* asking for the replug. The argument counts seconds **from the
moment the board reappears**, not from launch, so there is no window for
Michael to miss — it waits open-endedly for the unplug first. It dings when it
wants the gesture and again when it is done; **if there is no reply, assume he
did not hear it rather than that he declined.** Three captures in a row came
back empty before this was understood.

It streams as it reads, so a live capture is visibly alive; do not pipe it
through `tail`, which buffers until the process exits and makes a working
capture look identical to a dead one. The app logs only during its first
seconds, and the port takes ~700 ms to re-enumerate, so the very first line or
two after boot is often missed — that is not a fault.

**Never diagnose geometry from a photo or video of a moving pattern.** Phone
rolling shutter smears a travelling shape along its axis of motion, which is
indistinguishable from the shape being the wrong size, and the apparent size
varies frame to frame so it reads as an intermittent bug. Use the static frame
in `selftest.c` (`BOOT_MODE_SELFTEST`, see below) and judge only that.

Cameras also blow out bright colours on a dark background (green photographs as
white, red as yellow) and LCD black leaks blue at full backlight. **Colour
judgements belong to Michael's eye, not to a photo.**

## Layering

```
display.c/h    panel hardware: pins, SPI, backlight, orientation, rect fills
ui.c/h         LVGL port bring-up; picks which screen gets built
ui_zoo.c/h     the zoo screen: four animals, cycling
ui_test.c/h    arc + counter, kept as a known-good LVGL reference
zoo_sprites.*  generated by tools/gen_sprites.py — never edit by hand
selftest.c/h   raw panel exercise, bypasses LVGL entirely
boot_mode.h    which of the three the build brings up
console.c/h    the REPL on the log UART; owns no commands, but owns registering them
storage.h      the shared NVS namespace; keys stay with the module that owns them
net.c/h        esp_netif + the default event loop; net_have_address()
wifi_cmd.c/h   the wifi_* console commands
wifi_creds.c/h Wi-Fi credentials in NVS — the only way one gets in
wifi_sta.c/h   the station: associates, reconnects with backoff; owns the status enum
time_sync.c/h  SNTP, and the timezone in NVS; starts on got-IP
time_cmd.c/h   the time and tz console commands
ota.c/h        firmware updates, and the rollback confirmation
ota_cmd.c/h    the ota, ota_status and reboot console commands
weather.c/h    NWS forecast, alerts and live observations: its own task,
               coordinates in NVS; owns the pressure history behind the trend
weather_cmd.c/h the wx table, weather_refresh and loc console commands
sun.c/h        sunrise and sunset from coordinates — no state, no network
main.c         boot path: NVS → commands → console → display → UI → fade → net → ota → time → wifi → weather
```

Console commands live with the module whose state they touch, not in
`console.c`. A console that `#include`d every feature it can drive would depend
on all of them to start one, and each new command group would edit a file that
owns none of what it changes. `main.c` registers them before starting the REPL,
which keeps that ordering visible in the boot path.

`BOOT_MODE` is one ordered choice, not a set of flags, because the three modes
are a ladder: the self-test drives the panel with no LVGL, the test screen adds
LVGL with no assets, the zoo adds the sprite tables. When something is wrong,
stepping down the ladder says which layer owns it. It was briefly two separate
booleans in two files, which made the meaningless combination representable.

Sprite art lives in `assets/` as PNG and is the source of truth; the generated
C is committed so the firmware build needs nothing beyond the IDF. Regenerate
with `python3 tools/gen_sprites.py` (needs Pillow, in the *system* python, not
the IDF virtualenv) rather than editing the output.

The storm screen carries live instrument readings under the headline —
dewpoint, pressure with a trend arrow, wind or gusts, the next sun event. It
used to rotate eight thunderstorm facts there; those are gone, because a fixed
list on a device that is on all day gets memorised and memorised text stops
being read.

There is nothing on the glass but the zoo. The Wi-Fi glyph that used to sit at
the top was removed deliberately; connection state is a console question now
(`wifi_status`), not a screen one. Deleting it took `wifi_status.h` and
`ui_palette.h` with it — both existed only to keep the UI and the radio from
including each other, and with no UI consumer a shared header is worse than a
local definition.

Anything that is a property of the physical glass — inversion, BGR order,
mirroring — belongs in the display layer, so that the LVGL-free self-test path
sees the same panel the UI does.

**But note:** `esp_lvgl_port` re-applies mirroring from its own rotation config
when it attaches, and zeroes there mean "mirror nothing", not "inherit". That is
why `PANEL_MIRROR_*` is public in `display.h` and read by both.

## Conventions

- Comments explain *why*, especially where a value was settled empirically or a
  tempting alternative is wrong. Do not narrate what the code plainly does.
- Commit messages explain the reasoning and what was rejected, not just the
  change. Existing history is the reference for depth.
- Prefer fixing the layer the problem lives in over adding a special case above
  it.
- **Nothing that logs on a timer logs above DEBUG.** The console shares UART0
  with the log, so a periodic line walks over whatever is being typed. This cost
  `ui_zoo.c` its per-animal INFO line; the next periodic log should start at
  DEBUG rather than rediscover it.
- Secrets never reach the log, a `printf`, or the line-editor history. A command
  taking one calls `console_forget_history()` — scrubbing its own buffers is not
  enough, because the REPL has already kept the whole line.

## Open items

Tracked in [docs/open-items.md](docs/open-items.md), one entry each with the
reason it waits and the condition that should bring it back. **Read that file
before planning work** — it is where "why hasn't this been done" is answered,
and it is deliberately not duplicated here.

The one that has no reason to wait, repeated here so it cannot be missed: **no
correctness pass has ever examined `display.c` and `ui.c` as a whole.**
`/code-review ultra` has run only against a cleanup diff, and the single real
bug found so far came from reading esp_lcd's source by hand.

## Not yet decided

What the device actually does. Wi-Fi is the prerequisite for every candidate
(clock + weather, Klipper/Moonraker monitor, MQTT display).

Wi-Fi itself is **done**. Credentials go in at the console and survive a power
cycle; the station reads them at boot, associates, reconnects with exponential
backoff, and reports on the glass. `wifi_set` reassociates a running station
without a reboot. All of it verified on hardware, including the failure paths.

**SNTP is done too.** The board syncs from pool.ntp.org a few seconds after it
gets an address, and the timezone is a console command (`tz`) stored in NVS
beside the credentials. It was deliberately kept out of the connect work so
that association and time sync fail separately rather than as one opaque
"nothing works" — which is also why `time_sync.c` listens to `IP_EVENT`
directly and knows nothing about `wifi_sta`.

A POSIX TZ string cannot be validated in software: `tzset` accepts anything and
silently falls back to UTC. `tz` therefore prints the resulting wall clock and
the check is Michael recognising it. Note the sign convention — the offset is
west-positive, so Eastern is `EST5EDT`, not `EST-5EDT`.

**Weather is the chosen purpose**, and it is done. The board fetches a
twelve-hourly forecast from api.weather.gov, works out how close the next
thunderstorm is, watches for active alerts, and reads the latest observation
from the nearest reporting station. Coordinates go in at the console (`loc`) and
live in NVS — never in the tree, for the same reason credentials do not.

`wx` is the one view command; it replaced `weather`, `obs` and `sun`, which were
three layouts for one question.

Six things about that module are load-bearing and non-obvious:

- **It is a two-request API.** `/points/{lat},{lon}` returns which grid square
  the coordinates fall in, not a forecast; the forecast is at a URL it hands
  back. The grid never moves, so that lookup is cached until `loc` changes.
- **TLS needs the clock.** A certificate is rejected when the board thinks it
  is 1970, so the poll task waits for `time_sync_synced()` as well as an
  address. Skipping that produces a TLS failure that reads as a network fault.
- **The station list is read as a prefix, not parsed.** It is 75 kB and only
  its head is wanted. That works because the list is ordered nearest-first,
  which the API documentation does not state — it was measured. The scan matches
  the key alone and skips whitespace to the value: NWS pretty-prints, and a
  literal `"stationIdentifier":"` appears nowhere in the document. That version
  built cleanly and found zero stations.
- **Observations are converted to US units at parse time.** The observations
  endpoint reports SI while the forecast reports Fahrenheit, and every numeric
  field needs a sentinel for "not reported" — nulls arrive individually and
  routinely, on ordinary days.
- **The pressure history is keyed on the station's own timestamp**, never on
  when the fetch happened, which is what makes the poll interval a free
  parameter. It lives in RAM, so it is blank for the first hours after every
  boot — including after every OTA. That is the accepted cost of not writing to
  flash on a schedule forever. A change of station discards it: station pressure
  depends on elevation, and splicing two baselines reports the step as a
  plummeting barometer.
- **The fetch never runs on the console task.** `weather_refresh` notifies the
  weather task and returns. A handshake against the full certificate bundle
  plus a 14 kB parse is more stack than a console command has, and with stack
  protection on that is a halt rather than a mystery.

What remains is the screen. The board knows the weather and still shows four
animals.
