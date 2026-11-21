#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct {
    int status;      /* HTTP status, 0 when the request never completed */
    char *body;      /* NUL-terminated, heap-allocated; free() it */
    size_t length;
} fetch_result_t;

/* GET `url` with a bearer token on a fresh connection. Bodies over `max_body` bytes fail. */
esp_err_t fetch_get(const char *url, const char *bearer, size_t max_body, fetch_result_t *out);

/* The same with POST and no request body: an action named entirely by its URL. */
esp_err_t fetch_post(const char *url, const char *bearer, size_t max_body, fetch_result_t *out);

/* A session keeps one connection (and its TLS handshake) open across requests to
 * the same host, which is what makes long polling cheap: the chip sleeps on the
 * socket between answers. Zero-initialise, then call fetch_session_get repeatedly;
 * on any transport error the connection is dropped and the next call reconnects. */
typedef struct {
    void *client;      /* esp_http_client_handle_t, NULL until first use */
    const char *bearer;
} fetch_session_t;

esp_err_t fetch_session_get(fetch_session_t *session, const char *url, size_t max_body, int timeout_ms,
                            fetch_result_t *out);
void fetch_session_close(fetch_session_t *session);
