# Firmware

ESP-IDF (via PlatformIO) firmware for the
**[Waveshare ESP32-S3-Touch-LCD-7](https://link.amazon/B098TS2q7)** (800×480
capacitive touch, ESP32-S3 N16R8). It long-polls the hub's `/api/fleet`, so a change
shows within a second, and draws the screen with LVGL 9.

<sub>The board link is an Amazon affiliate link: as an Amazon Associate I earn from
qualifying purchases.</sub>

## What it shows

- **Header**: the hub's clock and date; the number of sessions WORKING, needing
  input and DONE, each drawn at 40% of its colour while it is zero; up to three ring
  gauges for the Claude plan's allowance (when the hub has the figures), labelled
  WEEK, FABLE and 5H by default and spaced to fit the widest label; AGENTS.
- **List**: a row per session: a state pill, how long it has been in that state and
  its token cost; its title and machine; for a finished session, a line of what is
  left for you to do. Needs-input first (its pill pulses), then DONE, then working,
  then IDLE (a session nobody has used yet). Rows with nothing for you to do (still
  working, or finished with nothing asked) are dimmed. About five rows show at once;
  the list scrolls.
- **Elbows**: dusty rose, amber while any session needs input. A green pip in the
  upper bar while a DONE session has something for you; a blue pip in the lower bar
  while anything is working.
- **Leg**: a block per machine with its live count (amber while one of its sessions
  needs input, OFF when the hub cannot reach it). Tap one to list only that machine,
  tap it again for all.
- **Tap a row**: the hub raises that session's window (if a focus command is set up
  on the hub). The row is highlighted and shows what happened for a few seconds.
- **"+"**: the hub opens a new Claude Code session on the configured desktop.
- **Backlight**: on while anything is working, needs input or is DONE, and for 30
  minutes after the last of those; a touch wakes it for a minute. `POST /screen?on=0`
  switches the panel off whatever the sessions do (see
  [the board's own pages](#over-the-air-updates-and-the-boards-own-pages)).
- **No link**: after 20 s without an answer from the hub the list is withdrawn
  rather than left stale.

## Build and flash

`../install.sh firmware` does all of this: it installs PlatformIO, writes
`secrets.ini` from your answers, builds, and flashes a board plugged in over USB.
Its PlatformIO is in a directory of its own (`~/.local/share/agent-control/platformio`)
and `./pio` here runs it: `./pio run -t ota`, `./pio device monitor`. By hand, with a
PlatformIO of your own (core 6.2 or later):

    cp secrets.ini.example secrets.ini    # Wi-Fi, hub URL, device key, hostname, time zone
    pio run                               # build
    pio run -t upload                     # first flash, over USB
    pio device monitor                    # logs at 115200 (Ctrl-C to quit)
    pio run -t ota                        # every later flash, over Wi-Fi

`platformio.ini` names the platform by its release: pioarduino's build of the
Espressif platform, 55.03.313, which has this board's 16 MB flash, 8 MB PSRAM variant
(PlatformIO's own `espressif32` does not) and brings ESP-IDF 5.5.6. It needs `git`,
Python 3.10 to 3.14 (under 3.15, some of its Python packages are not ready-built yet
and want a C compiler) and PlatformIO core 6.2 or later, PlatformIO's own
(`pip install platformio`) or pioarduino's (`pip install pioarduino`); under an older
core, PlatformIO says the platform is not compatible with it. The platform keeps its
Python packages, pioarduino's core among them, in `penv` in PlatformIO's directory
(`~/.platformio/penv`): a PlatformIO that itself lives there, as one from PlatformIO's
installer script or its VS Code extension does, becomes pioarduino's build of
it. LVGL and Espressif's LCD, touch, mDNS and LED-strip
components come from the ESP component registry on the first build, at the versions
`src/idf_component.yml` names.

Every setting is compiled in, so changing `secrets.ini` means a rebuild and reflash.
Before each build, `settings_header.py` reads `secrets.ini`, checks it (a missing
setting, a misspelt one, a Wi-Fi password too short to be one: the build stops and
says which line) and writes it out as `ac_settings.h` in the build directory, which
the sources include. A value reaches the firmware exactly as it is typed in the file,
whatever characters are in it: `secrets.ini.example` describes the format, and
`tools/board_settings.py check` runs the same check without a build.

The first build also writes the full ESP-IDF configuration, from
`sdkconfig.defaults`, to `sdkconfig.waveshare7`, and later builds keep that file,
except when `sdkconfig.defaults` has changed since (a `git pull`): then it is made
again from the defaults, and says so.

After a USB flash the chip can sit in ROM download mode (backlight on, screen black,
nothing on the network): unplug the board and plug it back in once. From then on
restarts and OTA updates behave. To boot it without replugging, for one boot only:

    esptool --chip esp32s3 --port /dev/ttyACM0 --before no-reset --after no-reset \
        load-ram .pio/build/waveshare7/bootloader.bin

## Over-the-air updates and the board's own pages

The board runs a small HTTP server on port 80, announced over mDNS as
`<hostname>.local`. All but `/status` need `Authorization: Bearer <device key>`.

| | |
|---|---|
| `GET /status` | Running build (git describe, date, ELF sha), slot, OTA state, IP, uptime, free heap and free internal RAM, LVGL's heap (see [Hardware notes](#hardware-notes)), whether the backlight is lit right now (`"backlight"`), touch state. |
| `POST /ota` | Body = `firmware.bin`: written to the other slot, then the board reboots into it. |
| `GET /screenshot` | The screen as LVGL draws it, raw RGB565, 800×480. `python3 sim/render.py --raw shot.raw shot.png` makes a PNG. |
| `POST /tap?row=N`, `POST /tap?new=1` | What a finger on row N, or on the "+", does. These are real taps: a window comes forward, a session opens. |
| `POST /leds?c=RRGGBB[,RRGGBB,RRGGBB][&p=010]` | Sets the optional LEDs (left first; `p` = which pulse) until the next answer from the hub. |
| `POST /screen?on=0`, `POST /screen?on=1` | Switches the panel off, or back on. Off: the backlight and the optional LEDs are dark whatever the sessions do, and a touch on the glass does nothing. On: the backlight goes back to its usual rule, lit for a minute first, as after a touch. Answers `on` or `off`; `GET /screen` only says which. |

`/screen` is for a "screens off" button or script that darkens a room's monitors, so
that it can darken this panel with them:

    curl -X POST -H "Authorization: Bearer <device key>" "http://agent-display.local/screen?on=0"

The board goes on following the fleet while it is switched off, so the list is current
the moment it is lit again. A finger on the dark panel cannot raise a window or open a
session (`/tap` still does). Off is not kept across a restart: the board always starts
switched on.

`pio run -t ota` (added by `ota_target.py`) builds, then runs `tools/ota_push.py`:
it skips the upload when the board already runs that exact build, pushes it, waits
for the board to come back on the new build, then for the build to confirm itself.
Set `OTA_HOST=<ip>` when `<hostname>.local` does not resolve.

The partition table has two 6 MB app slots, and the bootloader's rollback is on: a
new image runs "pending verify" until it has fetched the fleet from the hub
(`ota_confirm()` in `main.c`). If that has not happened within 10 minutes, or the
board resets first, the bootloader goes back to the previous image, and the push
script reports the rollback.

## Optional status LEDs

Three WS2812B LEDs (a cut piece of strip) can mirror the header's three counts:
each lights in its state's colour, fully saturated, while that count is above zero,
and the NEEDS INPUT one pulses like its pill. The DONE light only counts sessions
that still have something for you to do. Wire the strip's data line to GPIO 6 (the
`AD` pin of the board's Sensor header, the one GPIO the board leaves free) and power
it from that header's 3V3 and GND; brightness is capped at 96/255 so three LEDs draw
about 70 mA. Then set `leds_gpio = 6` (and `leds_reversed = 1` if the strip's first
LED is under the right-hand count) in `secrets.ini`. The counts are drawn 86 px
(16.5 mm) apart around the screen's centre line, so LEDs mounted above the glass at
that spacing each sit over their own count.

`POST /screen?on=0` turns the LEDs off along with the backlight, whatever the counts
are; `on=1` brings them back in the colours the counts then call for.

## Simulator

    cd sim && make png        # out/<scene>.png and out/all-states.png
    cd sim && make gif        # out/<demo>.gif: the README's animations

A headless desktop build of the same `shared/ui.c` and `shared/fleet.c`, drawn by
LVGL into a frame buffer: the screenshots are the real screen, not a mock-up. It
clones LVGL at the version the firmware pins and compiles cJSON from the ESP-IDF
package PlatformIO installed, `../install.sh`'s or else your own in `~/.platformio`
(override `CJSON_DIR` if yours is elsewhere); it needs
gcc, make and Pillow. Fixtures in `sim/fixtures/` are hub answers; scenes are listed
in `sim/render.py`. `build/sim <fixture> none out.raw` also prints whether that fleet
would hold the backlight on. The demos for `make gif`, also in `render.py`, are steps
played through a simulated touch screen (`wait:MS`, `tap:row:N`, `tap:host:NAME`,
`tap:plus`, and the hub's answers `raised`, `refused`, `opened`; see `sim_main.c`), so
a tap reaches the UI as a press and release, as on the board; a circle is drawn where
the finger touched. The clock is shown in UTC; `TZ=... make png` for another
zone. After editing a header, `make clean`.

## Layout

- `src/`: the ESP-IDF application. `board.c` (RGB panel, CH422G expander, GT911
  touch, LVGL port), `lv_mem_psram.c` (LVGL's heap, in PSRAM), `wifi.c` (no modem
  sleep: the radio stays awake, so a tap's answer does not wait for the access point's
  next beacon), `fetch.c` (HTTP with the bearer key, a kept-alive long-poll session),
  `ota.c` (status and update server, rollback), `leds.c`, `main.c` (fetch loop, tap
  requests, backlight, `/screenshot`, `/tap` and `/screen`).
- `shared/`: compiled by both the firmware and the simulator. `fleet.c` (JSON to
  `ac_model_t`), `ui.c` (the LVGL screen), the embedded Antonio Bold TTF.
- `sim/`: the desktop renderer.
- `tools/board_settings.py`, `settings_header.py`: `secrets.ini`, checked and made
  into the header the firmware is built with (`python3 -m unittest test_board_settings`
  in `tools/` tests it).
- `tools/ota_push.py`, `ota_target.py`: the `pio run -t ota` upload.
- `pio`: runs the PlatformIO that `../install.sh` set up.

## Hardware notes

Pins, timings and expander writes follow Waveshare's own ESP-IDF demo for this board
(`08_lvgl_Porting`): RGB565 over a 16-bit bus at 16 MHz, sync widths 4 and porches 8,
pixel clock active-low; I2C on GPIO 8/9 shared by the GT911 (0x5D) and the CH422G
(mode register at 0x24, outputs at 0x38); EXIO1 = touch reset, EXIO2 = backlight,
EXIO3 = LCD reset. The backlight is on/off only (no dimming). LVGL draws in direct
mode into the panel's two PSRAM frame buffers with a 10-line bounce buffer, so Wi-Fi
traffic does not tear the picture. `rotate_180` instead draws into a canvas that is
copied, reversed, into the panel's hidden buffer each frame.

LVGL's own heap (objects, label text, the fonts' glyph caches) is in PSRAM too, through
`src/lv_mem_psram.c`. On plain malloc its small blocks went to internal RAM, which
Wi-Fi and lwIP depend on, and the glyph caches, which keep filling as new characters
reach the screen, left them a few KB. `free_heap` in `/status` counts the 8 MB of
PSRAM, so it cannot show that; `free_internal`, `largest_internal` (the largest free
block) and `min_free_internal` (the low point since boot) can, and are the figures to
check if Wi-Fi or the board's pages start failing after it has run a while. A healthy
board keeps about 150 KB of internal RAM free, largest block about 70 KB. `lvgl_heap`
is what LVGL holds in PSRAM (it grows as the glyph caches fill, then levels off);
`lvgl_heap_fallbacks` counts blocks that had to go to internal RAM because PSRAM was
full, and is 0 on a healthy board.

If touch stops working after a knock, the touch panel's ribbon cable may have come
loose. The firmware carries on without touch when the GT911 does not answer
(`/status` then says `"touch": false`); reseating the cable and restarting brings it
back.
