/* Over-the-air updates: a small HTTP server on port 80.
 *
 *   GET  /status  -> JSON: running build, slot, confirmation state, IP, uptime
 *   POST /ota     -> body = firmware.bin, Authorization: Bearer <api key>;
 *                    writes the other slot, replies, reboots into it
 *
 * The bootloader's rollback is enabled, so a new image runs "pending verify"
 * until ota_confirm() is called. main.c calls it after the first good answer
 * from the hub; if that has not happened within `confirm_timeout_s` the device marks
 * the image bad and reboots into the previous one. `tools/ota_push.py` (run as
 * `pio run -t ota`) does the upload and watches /status for the outcome. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t ota_start(const char *bearer, const char *hostname, uint32_t confirm_timeout_s);

/* Keep the running image (no-op unless it is pending verification). */
void ota_confirm(void);

/* The server ota_start() opened, for a project's own extra pages (NULL before ota_start),
 * and the bearer check those pages share with /ota. */
httpd_handle_t ota_httpd(void);
bool ota_authorised(httpd_req_t *req);
