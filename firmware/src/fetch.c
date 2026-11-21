#include "fetch.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "fetch";

typedef struct {
    char *buf;
    size_t len, cap;
    bool overflow;
} sink_t;

static esp_err_t on_event(esp_http_client_event_t *evt) {
    sink_t *sink = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (sink->len + evt->data_len >= sink->cap) {
            sink->overflow = true;
            return ESP_FAIL;
        }
        memcpy(sink->buf + sink->len, evt->data, evt->data_len);
        sink->len += evt->data_len;
    }
    return ESP_OK;
}

static esp_err_t request(esp_http_client_method_t method, const char *url, const char *bearer, size_t max_body,
                         fetch_result_t *out) {
    memset(out, 0, sizeof(*out));
    sink_t sink = {.buf = malloc(max_body + 1), .cap = max_body + 1}; /* +1: room for the NUL after a full body */
    if (!sink.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000, /* a few hundred KB over TLS from a public host */
        .event_handler = on_event,
        .user_data = &sink,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach, /* https URLs verify against the built-in CA bundle */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(sink.buf);
        return ESP_FAIL;
    }
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", bearer);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_method(client, method);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        out->status = esp_http_client_get_status_code(client);
    } else if (sink.overflow) {
        ESP_LOGW(TAG, "body over %u bytes", (unsigned)max_body);
        err = ESP_ERR_INVALID_SIZE;
    } else {
        ESP_LOGW(TAG, "%s: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(sink.buf);
        return err;
    }
    sink.buf[sink.len] = '\0';
    out->body = sink.buf;
    out->length = sink.len;
    return ESP_OK;
}

esp_err_t fetch_get(const char *url, const char *bearer, size_t max_body, fetch_result_t *out) {
    return request(HTTP_METHOD_GET, url, bearer, max_body, out);
}

esp_err_t fetch_post(const char *url, const char *bearer, size_t max_body, fetch_result_t *out) {
    return request(HTTP_METHOD_POST, url, bearer, max_body, out);
}

/* --- persistent session ------------------------------------------------- */

static esp_err_t perform(esp_http_client_handle_t client, sink_t *sink, fetch_result_t *out) {
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        out->status = esp_http_client_get_status_code(client);
    } else if (sink->overflow) {
        ESP_LOGW(TAG, "body over %u bytes", (unsigned)(sink->cap - 1));
        err = ESP_ERR_INVALID_SIZE;
    } else {
        ESP_LOGW(TAG, "%s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t fetch_session_get(fetch_session_t *session, const char *url, size_t max_body, int timeout_ms,
                            fetch_result_t *out) {
    memset(out, 0, sizeof(*out));
    sink_t sink = {.buf = malloc(max_body + 1), .cap = max_body + 1};
    if (!sink.buf) return ESP_ERR_NO_MEM;

    esp_http_client_handle_t client = session->client;
    if (!client) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = timeout_ms,
            .event_handler = on_event,
            .user_data = &sink,
            .buffer_size = 4096,
            .keep_alive_enable = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        client = esp_http_client_init(&cfg);
        if (!client) {
            free(sink.buf);
            return ESP_FAIL;
        }
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", session->bearer);
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_header(client, "Accept", "application/json, application/octet-stream");
        session->client = client;
    } else {
        esp_http_client_set_url(client, url);
        esp_http_client_set_timeout_ms(client, timeout_ms);
    }
    /* the sink lives on this call's stack: point the client at it for this request only */
    esp_http_client_set_user_data(client, &sink);

    esp_err_t err = perform(client, &sink, out);
    if (err != ESP_OK) {
        fetch_session_close(session); /* whatever the state of that connection, start clean next time */
        free(sink.buf);
        return err;
    }
    sink.buf[sink.len] = '\0';
    out->body = sink.buf;
    out->length = sink.len;
    return ESP_OK;
}

void fetch_session_close(fetch_session_t *session) {
    if (session->client) {
        esp_http_client_cleanup(session->client);
        session->client = NULL;
    }
}
