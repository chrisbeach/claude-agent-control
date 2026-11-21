#include "fleet.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static const char *const STATUS_NAMES[AC_STATUS_COUNT] = {
    [AC_WORKING] = "working", [AC_WAITING] = "waiting", [AC_IDLE] = "idle",       [AC_RUNNING] = "running",
    [AC_DONE] = "done",       [AC_FAILED] = "failed",   [AC_STOPPED] = "stopped",
};

static const cJSON *get(const cJSON *obj, const char *key) { return cJSON_GetObjectItemCaseSensitive(obj, key); }

static void copy_string(char *dst, size_t dst_len, const cJSON *item) {
    snprintf(dst, dst_len, "%s", cJSON_IsString(item) && item->valuestring ? item->valuestring : "");
}

static int number(const cJSON *item, int fallback) { return cJSON_IsNumber(item) ? item->valueint : fallback; }

static int64_t epoch(const cJSON *item) { return cJSON_IsNumber(item) ? (int64_t)item->valuedouble : 0; }

static bool parse_status(const cJSON *item, ac_status_t *out) {
    if (!cJSON_IsString(item)) return false;
    for (int i = 0; i < AC_STATUS_COUNT; i++) {
        if (strcmp(item->valuestring, STATUS_NAMES[i]) == 0) {
            *out = (ac_status_t)i;
            return true;
        }
    }
    return false;
}

bool ac_fleet_parse(const char *json, size_t len, ac_model_t *out, char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        snprintf(err, err_len, "not JSON");
        return false;
    }
    const cJSON *sessions = get(root, "sessions"), *hosts = get(root, "hosts"), *counts = get(root, "counts");
    if (!cJSON_IsArray(sessions) || !cJSON_IsArray(hosts) || !cJSON_IsObject(counts) || !cJSON_IsNumber(get(root, "now"))) {
        snprintf(err, err_len, "missing now/counts/hosts/sessions");
        cJSON_Delete(root);
        return false;
    }
    copy_string(out->rev, sizeof(out->rev), get(root, "rev"));
    out->now = epoch(get(root, "now"));
    out->working = number(get(counts, "working"), 0);
    out->waiting = number(get(counts, "waiting"), 0);
    out->done = number(get(counts, "done"), 0);

    const cJSON *item;
    cJSON_ArrayForEach(item, get(root, "usage")) { /* absent or null: no blocks */
        if (out->n_usage == AC_MAX_USAGE) break;
        if (!cJSON_IsString(get(item, "label")) || !cJSON_IsNumber(get(item, "percent"))) continue;
        ac_usage_t *u = &out->usage[out->n_usage++];
        copy_string(u->label, sizeof(u->label), get(item, "label"));
        u->percent = number(get(item, "percent"), 0);
    }
    cJSON_ArrayForEach(item, hosts) {
        if (out->n_hosts == AC_MAX_HOSTS) break;
        ac_host_t *h = &out->hosts[out->n_hosts++];
        copy_string(h->name, sizeof(h->name), get(item, "name"));
        h->up = cJSON_IsTrue(get(item, "up"));
        h->sessions = number(get(item, "sessions"), 0);
        h->waiting = number(get(item, "waiting"), 0);
    }
    cJSON_ArrayForEach(item, sessions) {
        if (out->n_sessions == AC_MAX_SESSIONS) break;
        ac_session_t *s = &out->sessions[out->n_sessions];
        if (!parse_status(get(item, "status"), &s->status)) continue; /* a state this build does not know: leave the row out */
        copy_string(s->key, sizeof(s->key), get(item, "key"));
        copy_string(s->host, sizeof(s->host), get(item, "host"));
        copy_string(s->title, sizeof(s->title), get(item, "title"));
        copy_string(s->detail, sizeof(s->detail), get(item, "detail"));
        copy_string(s->todo, sizeof(s->todo), get(item, "todo"));
        s->quiet = cJSON_IsTrue(get(item, "quiet"));
        s->since = epoch(get(item, "since"));
        copy_string(s->tokens, sizeof(s->tokens), get(item, "tokens"));
        out->n_sessions++;
    }
    cJSON_Delete(root);
    return true;
}
