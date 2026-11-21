#pragma once

#include "model.h"

/* Parses the hub's /api/fleet answer. On failure returns false with a short reason in `err`. */
bool ac_fleet_parse(const char *json, size_t len, ac_model_t *out, char *err, size_t err_len);
