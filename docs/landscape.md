# Animated weather landscape

The default boot mode is `BOOT_MODE_LANDSCAPE`. Wi-Fi credentials, coordinates,
timezone, the weather service, serial commands, and OTA retain their existing
configuration. `BOOT_MODE_STORM` still selects the original dial.

## What drives the picture

- Station descriptions no more than two hours old take priority.
- If there is no recognizable recent observation, the first forecast period
  can drive the scene only while its start/end interval contains the current
  time and the fetch is no more than three hours old. The caption says
  `Forecast`: rain mentioned in a forecast is not a measured rainfall report.
- An expired report produces `Weather unavailable`; a cold boot waits for
  weather. Neither state draws a sun or precipitation to suggest known weather.
- The temperature is a recent measured temperature, never the forecast high or
  low. An absent or old measurement is hidden. Wind affects cloud drift speed.
- Daylight is independent of weather availability. Colors fade over the half
  hour on each side of sunrise and sunset. This is an illustrative transition,
  not a measurement of cloud cover or astronomical twilight.
- With no synced clock, coordinates, or available sunrise/sunset pair (including
  polar dates), lighting stays neutral. The moon is decorative, not a phase
  calculation, and the landscape does not depict the user's actual location.
- Supported illustrations are clear, partly cloudy, cloudy, rain, snow/sleet,
  thunderstorms, and fog/haze. Mixed descriptions use the more significant
  condition. Thunderstorms have rain and darker clouds, without flashing.

Severe or extreme warnings retain the existing full-screen takeover policy.
Other alerts replace the condition caption. A cached alert remains visible if
the network fails, with `Update overdue` once the last successful check is over
15 minutes old. A successful clear alert response restores the scene.

## Rendering

`landscape_art.c` renders a 120 by 120 RGB565 image in a static 28,800-byte
buffer. LVGL scales it exactly 2x with nearest-neighbor sampling to fill the
240-pixel screen. The artwork is code-native; no generated C pixel tables,
external assets, filesystem, or PSRAM are needed for this mode.

Animation runs at eight frames per second. The weather snapshot is refreshed
every ten seconds; network polling retains the existing service's intervals.
The animation callback performs no networking, logging, allocation, or NVS
access. It pauses during a warning. Warning text reuses the scene's labels to
fit the existing 16 KB LVGL allocator.

## Desktop verification on Windows

Requires native GCC, Python with Pillow, and the pinned LVGL 9.5.0 source:

```powershell
git clone --branch v9.5.0 --depth 1 https://github.com/lvgl/lvgl.git build/host/lvgl
powershell -ExecutionPolicy Bypass -File tools/test_landscape.ps1 -Compiler C:/msys64/ucrt64/bin/gcc.exe
python tools/preview_landscape.py
```

`-Compiler` can also name any native GCC on PATH. The script builds the actual
production model, artwork, and LVGL screen against host stubs for the weather,
clock, and sunrise service. It tests condition parsing, data age, forecast
validity, daylight boundaries, animated frame changes, warning pauses, and
recovery. It uses a 16 KB LVGL arena and checks allocator integrity after a
simulated minute. The desktop has 64-bit pointers, so its heap overhead differs
from the ESP32's.

Outputs live under ignored `build/host/`: per-state PPM/PNG frames,
`landscape-preview.png`, and `landscape-rain.gif`. These are renders from the
firmware's actual LVGL screen, not a separate web mockup.

## Build and device verification

The isolated Windows toolchain for this checkout lives under ignored
`build/toolchains/`. Build without flashing from PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_windows.ps1
```

The helper also accepts `-IdfPath` and `-ToolsPath` for another installation.
Its output directory is `build/firmware`, separate from the toolchain and host
previews. No system-wide PATH changes are needed.

The Windows ESP-IDF 5.5 build passed on 2026-09-20. Its application image is
1,390,944 bytes, leaving 67% of the 4 MB OTA slot free. The 19 host scenarios
also passed, including longer alerts, missing temperatures, and data aging
while the screen is running.

The image was installed over Wi-Fi on the Knomi on the same date. The board
booted into the new OTA slot, confirmed the image, synced its clock, and fetched
the forecast and current station observations over TLS. A 45-second boot
capture showed no assertions or panics, and a subsequent console check
confirmed the image and live weather. The owner also confirmed normal colors,
text fitting inside the round screen, and visible animation. Eight frames per
second is the configured rate, not a measured hardware benchmark.

For serial commands on Windows, use the ESP-IDF Python environment with
`tools/console.py --port COM4 ota_status` (choose the actual port if different).
`tools/serve_ota.py` can serve one binary to one specified board for an approved
OTA update. It has no directory listing and exits after the completed transfer
or when its transfer window expires.

Build with ESP-IDF 5.5 and the committed component lock. Existing generated
`sdkconfig` files must enable `CONFIG_LV_FONT_MONTSERRAT_20=y` as well as the
existing 28-point font; new configurations inherit both from `sdkconfig.defaults`.
The framebuffer adds internal RAM usage, so check free heap during TLS polling
and OTA, as well as frame rate, round-screen clipping, and colors on the device.
The established manual USB gestures and pre-commit hardware checks still apply.
