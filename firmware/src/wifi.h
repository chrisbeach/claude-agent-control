#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Joins the configured network and keeps re-joining whenever it drops.
 * `hostname` is what the router (DHCP) and mDNS see. */
esp_err_t wifi_start(const char *ssid, const char *password, const char *hostname);
bool wifi_connected(void);
