#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* Lights the optional LEDs (WS2812B on AC_LEDS_GPIO) and adds POST /leds to the given server. */
esp_err_t leds_start(httpd_handle_t server);

/* One colour per band, 0xRRGGBB as on the screen, left band first; 0 = dark. Scaled to the strip's cap.
 * Bit b of `pulsing` set: band b pulses as a pulsing pill does (ui.h). Does nothing until leds_start(). */
void leds_set(const uint32_t colours[3], uint8_t pulsing);
