# Open items

Work deferred deliberately, each with the reason and the condition that should
bring it back. Nothing here is forgotten; if an item no longer has a reason to
wait, it has become work rather than an open item.

**Read this before planning work.** [CLAUDE.md](../CLAUDE.md) covers how to
build, flash and verify; this file covers what is knowingly unfinished.

When an item closes, delete it and say so in the commit that closed it. Do not
leave a struck-through record — git already has that, and a list of dead entries
is what stops people reading the live ones.

---

## 1. The whole-tree correctness pass was a hand read, not a tool

Every source file, plus the build config and `tools/capture.py`, has now been
read end to end against the esp_lcd and LEDC sources. That produced eight
findings, all fixed. So the gap this item described is largely closed — by one
reading, which is as repeatable as the reader.

`/code-review ultra` cannot substitute, and it is worth recording why so the
next person does not spend a run learning it: the command is diff-scoped
against the branch upstream. Passing "review the whole project" as an argument
is recorded as a note and does not change the scope — two runs have now
returned zero findings on narrow diffs, which reads as a clean bill of health
for the project and is not one. Forcing whole-tree scope means constructing a
PR against an empty base, which is a contrivance rather than a workflow.

**Why it waits:** the cheap version is done. What remains is a second
*independent* read, and a second pass by the same reader is worth much less
than the first.

**Trigger:** a reviewer who is not me — a human, or ultra pointed at a diff
that genuinely contains the files. Otherwise this closes on its own as each
future change gets reviewed on the way in.

## 2. Sprite pixels are committed as 2 MB of C text

`main/zoo_sprites.c` is 2 MB of `0x00, ` ASCII holding 324 kB of actual pixels —
a 6x expansion of the bytes and a 100x expansion of the 20 kB of PNGs they come
from. Every regeneration rewrites all of it, so a one-pixel art tweak produces a
multi-megabyte diff no review can read.

The fix is not build-time generation: that would put Pillow in the path of every
clean build, on a project whose CLAUDE.md already documents two Python
environments as a live source of confusion. Committing the artifact is right.
Committing it *as C text* is not — the generator should emit a packed binary
blob plus a small descriptor table, pulled in with IDF's `EMBED_FILES`. Git then
stores the pixels as pixels, and the reviewable part shrinks to a few dozen
lines.

**Why it waits:** the art is settled and the current form works, verified on
hardware. Doing it now would mean rewriting the asset pipeline in the same
breath as landing the feature, and the feature is the thing that was tested.

**Trigger:** the first art change. That is when the unreadable diff stops being
theoretical. Fold in the removal of `zoo_animal_t` at the same time — the struct
exists to serve one log line and the blob rework replaces its layout anyway.

## 3. Nothing can tear a screen down

`ui_zoo_screen_build()` creates an `lv_timer` and builds objects on
`lv_screen_active()`; neither can be released. With compile-time `BOOT_MODE`
selection exactly one screen is ever built and never destroyed, so this costs
nothing today.

**Why it waits:** a `screen_t { build, destroy }` interface with two screens,
one of which is never dispatched on, is indirection that nothing uses. Screen
count is not the trigger — three more `void f(void)` builders cost nothing.

**Trigger:** the first time two screens must coexist or swap at runtime. That is
when `destroy` acquires a job, and the interface can be extracted then from two
concrete examples rather than guessed at now from none.

## 4. "Don't mix drawing paths" is prose, not code

Nothing stops a caller using `display_fill*` after LVGL owns the panel.
`fill_buf_wait_idle()` does not help: it protects the fill buffer from the
previous *fill*, not from LVGL.

**Why it waits:** no caller does this today, and the enforcement worth having
depends on what the second drawing path turns out to be.

**Trigger:** the first time something draws a splash before LVGL starts.

## 5. PSRAM disabled

8 MB unused.

**Why it waits:** nothing cold needs to live there yet, and enabling it costs
DRAM for the mapping.

**Trigger:** something large and rarely touched — a font set, a bitmap cache.

**Do not** move LVGL draw buffers into it: the software renderer does per-pixel
read-modify-write and WROVER PSRAM is roughly 10x slower, which would push
render time past flush time.

## 6. The initial black clear paints an invisible frame

`display_init()` clears to black while the backlight is still at zero, costing
~23 ms and 115 kB of SPI at boot. It is also the only reason the 9.6 kB DMA
fill buffer is allocated during init at all.

**Why it waits:** kept deliberately as defence for a future splash path, and as
insurance against showing power-up noise if the fade ever moves earlier.

**Trigger:** if boot latency ever matters, this is 23 ms sitting in plain sight.

## 7. `CONFIG_FREERTOS_HZ=1000`

Buys 1 ms scheduling granularity that nothing currently needs — the LVGL tick is
10 ms and the shortest delay in the tree is 30 ms. Costs ~900 timer interrupts a
second.

**Why it waits:** harmless today, and 100 Hz would be a regression if something
later does want fine delays.

**Trigger:** if the tick ISR shows up competing with Wi-Fi.

## 8. Only one of `display_init()`'s six failure exits has executed

The unwind was verified by injecting a failure at the last and most
resource-heavy point, confirming on hardware that a second `display_init()`
then succeeds. Earlier failures take shorter paths through the same label and
remain reasoned-about rather than run.

**Why it waits:** each additional exit costs a flash cycle for progressively
less information, and the whole path is unreachable while `app_main` wraps init
in `ESP_ERROR_CHECK`.

**Trigger:** the first caller that wants to survive a failed `display_init()`.

## 9. OTA is plain HTTP

`ota <url>` fetches over unencrypted HTTP, enabled deliberately with
`CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`. On a LAN, initiated by a human typing a URL
at a console, the exposure is that someone already inside the network could
answer at that address with their own image.

**Why it waits:** the certificate bundle is ~50 kB and TLS would have landed in
the same change as the feature being tested, so a failure could have been either.
It is genuinely unblocked now — certificate validation needs a correct clock, and
SNTP supplies one.

**Trigger:** the first update that comes from anywhere but this LAN. Also worth
doing sooner if the board is ever exposed to a network with guests on it.

## 10. `built:` in `ota_status` does not track source changes

`esp_app_desc.c` is compiled once and not rebuilt when other sources change, so
two images with different code can report the same build timestamp — observed
directly during OTA testing, where two functionally different images both said
`20:43:47`. The version string is `git describe`, so it only moves on a commit
and reads `-dirty` in between.

**Why it waits:** slot alternation and observed behaviour distinguished the
images perfectly well during testing, so nothing was blocked.

**Trigger:** the first time an update has to be identified after the fact rather
than during a session that just built it — which is the first update that
matters. A `version.txt` read by IDF as `PROJECT_VER` is the small fix.

## 11. The forecast is twelve-hourly, not hourly

`wx` answers "storms tonight", not "storms in three hours". The hourly
endpoint exists and gives exactly that — 156 periods with the same fields — but
its document is **92 kB against 14 kB**, measured, and a cJSON tree built from
it is several times that again. That does not fit in DRAM beside Wi-Fi and TLS.

This is item 5's trigger, and the two should be done together: enabling PSRAM
gives 8 MB, and cJSON can be pointed at it with `cJSON_InitHooks` so the parse
tree lands there rather than in internal RAM. The response buffer would go the
same way.

**Why it waits:** twelve-hourly resolution is genuinely useful for a desk
ornament — "storm tonight, 40%" is the whole question most days — and doing it
now would mean enabling PSRAM, rehoming an allocator, and landing a feature in
one change, with three ways to fail and one test.

**Trigger:** the first time twelve hours proves too coarse to act on. Watching
it for a week is the honest way to find out. Note the standing warning in item
5 still applies — PSRAM is for the JSON, never for LVGL draw buffers.

## 12. `probabilityOfThunder` is not used, and the dial infers instead

The storm state on the dial comes from string-matching `shortForecast` for the
word "thunderstorm", with `probabilityOfPrecipitation` borrowed as a stand-in
for how likely the storm is. That stand-in is wrong in a specific way: an 84%
chance of *rain* in a period that mentions storms is not an 84% chance of
*storms*.

NWS publishes the real figure. `/gridpoints/{office}/{x},{y}` carries
`probabilityOfThunder` as a first-class element, hourly — measured at 38 values
for one grid square, alongside 58 others.

**Why it waits:** that document is **292 kB**, measured, and the API offers no
field filtering on it. It is twenty times the twelve-hourly forecast and three
times the hourly one, so it does not fit even with PSRAM without a streaming
parser that walks the body and keeps one element while discarding the rest.
That parser is the work, and it is the same work item 11 needs.

Note also that several elements of that document are populated by some forecast
offices and not others — `lightningActivityLevel`, `hazards` and `pressure` all
came back empty for the test grid square while `probabilityOfThunder` and
`skyCover` were full. Anything built on the empty ones would work in one part of
the country and silently show nothing in another, which is the worst failure
mode on a screen with no error channel.

**Trigger:** doing item 11. The streaming parser serves both, and the dial's
central claim stops being an inference the moment it lands.

## 13. Pressure trend tests are not yet in the host test setup

The landscape added the second piece of portable logic and a native GCC/LVGL
test setup (`tools/test_landscape.ps1`). It compiles production sources directly
and covers weather classification, freshness, daylight, and screen rendering.

The pressure trend's earlier twelve-case extracted harness is still not
committed. Moving the history calculations into a portable module would let
the new setup test them without copying functions out of `weather.c`.

**Why it waits:** the landscape does not change pressure calculations, and
extracting them now would broaden the behavioral change being verified.

**Trigger:** the next pressure-history change. Add those cases to the existing
host runner rather than creating another temporary extracted harness.

## 14. A failed first SNTP sync is not retried promptly, and it stalls everything

Seen once, on hardware, during this session. The board associated, obtained an
address, and never set its clock: `time` reported "needs a connection" while
`wifi_status` showed a live association at -36 dBm. The boot before it had
logged `disconnected, reason 8` — the association dropped, and SNTP's first
attempt went with it.

That is worse than a missing clock. TLS rejects a certificate when the board
thinks it is 1970, so the weather task waits on `time_sync_synced()` and never
fetches: one silent failure at second four takes down the forecast, the alerts
and the observations, and the console's own advice points at Wi-Fi, which is
working.

It cleared on the next reboot and has not recurred, which is exactly why it is
written down rather than chased — a fault seen once and not reproduced is a note,
not a bug hunt.

**Why it waits:** the fix is not obvious. `time_sync.c` starts SNTP on
`IP_EVENT_STA_GOT_IP`, which is the right trigger and fires again on a
reconnect, so the case that failed here should have recovered on its own. Until
it is understood whether the event did not fire, or fired and the sync still did
not happen, any change is a guess dressed as a fix.

**Trigger:** the second occurrence — or, sooner, a deliberate look at whether
`esp_netif_sntp` retries a failed initial sync at all, which is a question the
IDF source can answer without waiting for the fault.
