# Firmware

ESP-IDF (via PlatformIO) firmware for the **Waveshare ESP32-S3-Touch-LCD-7**
(800×480 capacitive touch, ESP32-S3 N16R8). It long-polls the hub's `/api/fleet`,
so a change shows within a second, and draws the screen with LVGL 9.

## What it shows

- **Header**: the hub's clock and date; the number of sessions WORKING, needing
  input and DONE; up to three ring gauges for the Claude plan's allowance (when the
  hub has the figures); AGENTS.
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
  minutes after the last of those; a touch wakes it for a minute.
- **No link**: after 20 s without an answer from the hub the list is withdrawn
  rather than left stale.

## Build and flash

    cp secrets.ini.example secrets.ini    # Wi-Fi, hub URL, device key, hostname, time zone
    pio run                               # build
    pio run -t upload                     # first flash, over USB
    pio device monitor                    # logs at 115200 (Ctrl-C to quit)
    pio run -t ota                        # every later flash, over Wi-Fi

LVGL and Espressif's LCD, touch, mDNS and LED-strip components come from the ESP
component registry on the first build. Every setting is compiled in, so changing
`secrets.ini` means a rebuild and reflash.

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
| `GET /status` | Running build (git describe, date, ELF sha), slot, OTA state, IP, uptime, free heap, touch state. |
| `POST /ota` | Body = `firmware.bin`: written to the other slot, then the board reboots into it. |
| `GET /screenshot` | The screen as LVGL draws it, raw RGB565, 800×480. `python3 sim/render.py --raw shot.raw shot.png` makes a PNG. |
| `POST /tap?row=N`, `POST /tap?new=1` | What a finger on row N, or on the "+", does. These are real taps: a window comes forward, a session opens. |
| `POST /leds?c=RRGGBB[,RRGGBB,RRGGBB][&p=010]` | Sets the optional LEDs (left first; `p` = which pulse) until the next answer from the hub. |

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

## Simulator

    cd sim && make png        # out/<scene>.png and out/all-states.png

A headless desktop build of the same `shared/ui.c` and `shared/fleet.c`, drawn by
LVGL into a frame buffer: the screenshots are the real screen, not a mock-up. It
clones LVGL at the version the firmware pins and compiles cJSON from the ESP-IDF
package PlatformIO installed (override `CJSON_DIR` if yours is elsewhere); it needs
gcc, make and Pillow. Fixtures in `sim/fixtures/` are hub answers; scenes are listed
in `sim/render.py`. `build/sim <fixture> none out.raw` also prints whether that fleet
would hold the backlight on. The clock is shown in UTC; `TZ=... make png` for another
zone. After editing a header, `make clean`.

## Layout

- `src/`: the ESP-IDF application. `board.c` (RGB panel, CH422G expander, GT911
  touch, LVGL port), `wifi.c`, `fetch.c` (HTTP with the bearer key, a kept-alive
  long-poll session), `ota.c` (status and update server, rollback), `leds.c`,
  `main.c` (fetch loop, tap requests, backlight, `/screenshot` and `/tap`).
- `shared/`: compiled by both the firmware and the simulator. `fleet.c` (JSON to
  `ac_model_t`), `ui.c` (the LVGL screen), the embedded Antonio Bold TTF.
- `sim/`: the desktop renderer.
- `tools/ota_push.py`, `ota_target.py`: the `pio run -t ota` upload.

## Hardware notes

Pins, timings and expander writes follow Waveshare's own ESP-IDF demo for this board
(`08_lvgl_Porting`): RGB565 over a 16-bit bus at 16 MHz, sync widths 4 and porches 8,
pixel clock active-low; I2C on GPIO 8/9 shared by the GT911 (0x5D) and the CH422G
(mode register at 0x24, outputs at 0x38); EXIO1 = touch reset, EXIO2 = backlight,
EXIO3 = LCD reset. The backlight is on/off only (no dimming). LVGL draws in direct
mode into the panel's two PSRAM frame buffers with a 10-line bounce buffer, so Wi-Fi
traffic does not tear the picture. `rotate_180` instead draws into a canvas that is
copied, reversed, into the panel's hidden buffer each frame.

If touch stops working after a knock, the touch panel's ribbon cable may have come
loose. The firmware carries on without touch when the GT911 does not answer
(`/status` then says `"touch": false`); reseating the cable and restarting brings it
back.
