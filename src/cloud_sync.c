#include "cloud_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"

#include "app_config.h"
#include "config_store.h"
#include "schedule.h"
#include "wifi.h"

/* Same floor as schedule.c: reject unset/1970 clocks for call-log ISO times. */
#define CLOUD_TIME_PLAUSIBLE 1735689600 /* 2025-01-01 UTC */

static const char *TAG = "cloud_sync";

typedef struct {
    char event_id[40];
    int contact_slot;
    char contact_name[32];
    char outcome[16];
    uint32_t duration_ms;
    int64_t started_ms;
    int64_t ended_ms;
    bool used;
} log_event_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_wake;
static log_event_t s_log[APP_CLOUD_LOG_CAPACITY];
static int s_log_head;
static int s_log_count;
static char s_last_error[96];
static int64_t s_last_ok_ms;
static bool s_started;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Wall-clock ms for call logs. Uptime (esp_timer) must never go in ISO timestamps. */
static int64_t wall_ms_now(void)
{
    time_t t = time(NULL);
    if (t > CLOUD_TIME_PLAUSIBLE) {
        return (int64_t)t * 1000;
    }
    return 0;
}

static void set_err(const char *msg)
{
    if (!msg) {
        s_last_error[0] = '\0';
        return;
    }
    strlcpy(s_last_error, msg, sizeof(s_last_error));
}

static void mac_str(char *out, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void make_pair_code(char *out, size_t len)
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    if (len < 7) {
        if (len) {
            out[0] = '\0';
        }
        return;
    }
    uint32_t r = esp_random();
    for (int i = 0; i < 6; i++) {
        out[i] = alphabet[(r + esp_random() + i * 17) % (sizeof(alphabet) - 1)];
        r = (r >> 5) ^ esp_random();
    }
    out[6] = '\0';
}

typedef struct {
    char *buf;
    int len;
    int cap;
    int status;
} http_resp_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_resp_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 && r && r->buf) {
        int need = r->len + evt->data_len;
        if (need < r->cap) {
            memcpy(r->buf + r->len, evt->data, evt->data_len);
            r->len += evt->data_len;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool http_json(const char *method, const char *url, const char *bearer, const char *body,
                      char *resp, int resp_cap, int *status_out)
{
    if (resp && resp_cap > 0) {
        resp[0] = '\0';
    }
    http_resp_t hr = {.buf = resp, .len = 0, .cap = resp_cap, .status = -1};
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = APP_CLOUD_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &hr,
        /* Roster + a few call events + status fit under 6 KiB. */
        .buffer_size = 2048,
        .buffer_size_tx = 6144,
    };
    if (strcmp(method, "GET") == 0) {
        cfg.method = HTTP_METHOD_GET;
    } else if (strcmp(method, "POST") == 0) {
        cfg.method = HTTP_METHOD_POST;
    } else {
        return false;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_http_client_set_header(c, "User-Agent", APP_SIP_USER_AGENT);
    if (bearer && bearer[0]) {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", auth);
        mbedtls_platform_zeroize(auth, sizeof(auth));
    }
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, strlen(body));
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (status_out) {
        *status_out = (err == ESP_OK) ? status : -1;
    }
    return err == ESP_OK && status >= 200 && status < 300;
}

static void iso_utc(int64_t ms, char *out, size_t len)
{
    if (ms <= 0) {
        out[0] = '\0';
        return;
    }
    time_t sec = (time_t)(ms / 1000);
    struct tm tm;
    if (!gmtime_r(&sec, &tm)) {
        out[0] = '\0';
        return;
    }
    strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* 401 = revoked/invalid device token → clear credentials; local calling stays up. */
static void on_auth_rejected(cfg_cloud_t *cloud)
{
    set_err("cloud auth rejected (re-pair)");
    config_clear_cloud();
    if (cloud) {
        mbedtls_platform_zeroize(cloud->device_token, sizeof(cloud->device_token));
        cloud->device_uuid[0] = '\0';
        cloud->device_name[0] = '\0';
        cloud->pair_code[0] = '\0';
    }
}

static bool apply_schedule_cmd(const cJSON *payload)
{
    const cJSON *slot_j = cJSON_GetObjectItem(payload, "contact_slot");
    const cJSON *sched = cJSON_GetObjectItem(payload, "schedule");
    if (!cJSON_IsNumber(slot_j) || !cJSON_IsObject(sched)) {
        return false;
    }
    int slot = slot_j->valueint;
    if (slot < 0 || slot >= CFG_MAX_CONTACTS) {
        return false;
    }
    app_cfg_t *cfg = malloc(sizeof(app_cfg_t));
    if (!cfg) {
        return false;
    }
    config_get(cfg);
    cfg_schedule_t *s = &cfg->contacts[slot].sched;
    memset(s, 0, sizeof(*s));
    const cJSON *en = cJSON_GetObjectItem(sched, "enabled");
    s->enabled = cJSON_IsTrue(en);
    const cJSON *tz = cJSON_GetObjectItem(sched, "tz");
    const cJSON *tzn = cJSON_GetObjectItem(sched, "tz_name");
    if (cJSON_IsString(tz)) {
        strlcpy(s->tz, tz->valuestring, sizeof(s->tz));
    }
    if (cJSON_IsString(tzn)) {
        strlcpy(s->tz_name, tzn->valuestring, sizeof(s->tz_name));
    }
    const cJSON *wins = cJSON_GetObjectItem(sched, "windows");
    if (cJSON_IsArray(wins)) {
        int n = cJSON_GetArraySize(wins);
        if (n > CFG_MAX_WINDOWS) {
            n = CFG_MAX_WINDOWS;
        }
        s->nwin = (uint8_t)n;
        for (int i = 0; i < n; i++) {
            const cJSON *w = cJSON_GetArrayItem(wins, i);
            const cJSON *days = cJSON_GetObjectItem(w, "days");
            /* App sends start_min/end_min; accept start/end as aliases. */
            const cJSON *sm = cJSON_GetObjectItem(w, "start_min");
            if (!cJSON_IsNumber(sm)) {
                sm = cJSON_GetObjectItem(w, "start");
            }
            const cJSON *em = cJSON_GetObjectItem(w, "end_min");
            if (!cJSON_IsNumber(em)) {
                em = cJSON_GetObjectItem(w, "end");
            }
            if (cJSON_IsNumber(days)) {
                s->win[i].days = (uint8_t)(days->valueint & 0x7f);
            }
            if (cJSON_IsNumber(sm)) {
                s->win[i].start_min = (uint16_t)(sm->valueint % 1440);
            }
            if (cJSON_IsNumber(em)) {
                s->win[i].end_min = (uint16_t)(em->valueint % 1440);
            }
        }
    }
    const char *why = schedule_validate(s);
    bool ok = false;
    if (!why) {
        ok = config_set_contacts(cfg->contacts) == ESP_OK;
        /* MQTT HA watches config_generation() and republishes Call titles. */
    }
    config_wipe(cfg);
    free(cfg);
    return ok;
}

static void ack_command(cfg_cloud_t *cloud, int id, bool ok, const char *error)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/api/kid-phone/v1/commands/%d/ack", cloud->base_url, id);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddBoolToObject(req, "ok", ok);
    if (error) {
        cJSON_AddStringToObject(req, "error", error);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return;
    }
    char resp[256];
    int status = 0;
    if (!http_json("POST", url, cloud->device_token, body, resp, sizeof(resp), &status)) {
        if (status == 401) {
            on_auth_rejected(cloud);
        }
    }
    free(body);
}

static void pull_commands(cfg_cloud_t *cloud)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/api/kid-phone/v1/commands", cloud->base_url);
    char resp[4096];
    int status = 0;
    if (!http_json("GET", url, cloud->device_token, NULL, resp, sizeof(resp), &status)) {
        if (status == 401) {
            on_auth_rejected(cloud);
        } else {
            set_err("commands pull failed");
        }
        return;
    }
    cJSON *j = cJSON_Parse(resp);
    if (!j) {
        set_err("bad commands JSON");
        return;
    }
    /* config_revision on the response is informational only. */
    const cJSON *cmds = cJSON_GetObjectItem(j, "commands");
    if (cJSON_IsArray(cmds)) {
        const int n = cJSON_GetArraySize(cmds);
        for (int i = 0; i < n; i++) {
            const cJSON *cmd = cJSON_GetArrayItem(cmds, i);
            const cJSON *id_j = cJSON_GetObjectItem(cmd, "id");
            const cJSON *type = cJSON_GetObjectItem(cmd, "type");
            const cJSON *payload = cJSON_GetObjectItem(cmd, "payload");
            if (!cJSON_IsNumber(id_j) || !cJSON_IsString(type) || !cJSON_IsObject(payload)) {
                continue;
            }
            bool ok = false;
            const char *err = NULL;
            if (strcmp(type->valuestring, "set_contact_schedule") == 0) {
                ok = apply_schedule_cmd(payload);
                if (!ok) {
                    err = "schedule_apply_failed";
                }
            } else {
                err = "unknown_command";
            }
            if (!cloud->device_token[0]) {
                break; /* revoked mid-loop */
            }
            ack_command(cloud, id_j->valueint, ok, err);
        }
    }
    cJSON_Delete(j);
}

static void add_status_object(cJSON *parent, const char *key)
{
    cJSON *st = key ? cJSON_AddObjectToObject(parent, key) : parent;
    cJSON_AddStringToObject(st, "firmware_version", esp_app_get_description()->version);
    char ssid[33];
    int rssi = 0;
    if (wifi_current(ssid, sizeof(ssid), &rssi)) {
        cJSON_AddStringToObject(st, "wifi_ssid", ssid);
        cJSON_AddNumberToObject(st, "wifi_rssi", rssi);
    }
    cJSON_AddNumberToObject(st, "uptime_seconds", (double)(esp_timer_get_time() / 1000000ULL));
    if (s_last_error[0]) {
        cJSON_AddStringToObject(st, "last_sync_error", s_last_error);
    }
}

/* Slot roster for the Bell-Towne manage UI (names + numbers). */
static void add_contacts_array(cJSON *root)
{
    cJSON *arr = cJSON_AddArrayToObject(root, "contacts");
    if (!arr) {
        return;
    }
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        cfg_contact_t c;
        (void)config_get_contact(i, &c);
        const bool used = c.number[0] || c.name[0] || c.secret != CFG_SECRET_OFF;
        if (!used) {
            mbedtls_platform_zeroize(&c, sizeof(c));
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i);
        if (c.name[0]) {
            cJSON_AddStringToObject(o, "name", c.name);
        } else {
            cJSON_AddStringToObject(o, "name", "");
        }
        if (c.number[0]) {
            cJSON_AddStringToObject(o, "number", c.number);
        }
        cJSON_AddItemToArray(arr, o);
        mbedtls_platform_zeroize(&c, sizeof(c));
    }
}

static void push_events(cfg_cloud_t *cloud)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    char kept_ids[APP_CLOUD_LOG_CAPACITY][40];
    int kept = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_log_count; i++) {
        int idx = (s_log_head + i) % APP_CLOUD_LOG_CAPACITY;
        log_event_t *e = &s_log[idx];
        if (!e->used) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "event_id", e->event_id);
        cJSON_AddNumberToObject(o, "contact_slot", e->contact_slot);
        cJSON_AddStringToObject(o, "contact_name", e->contact_name);
        cJSON_AddStringToObject(o, "outcome", e->outcome);
        cJSON_AddNumberToObject(o, "duration_ms", e->duration_ms);
        char t0[32], t1[32];
        iso_utc(e->started_ms, t0, sizeof(t0));
        iso_utc(e->ended_ms, t1, sizeof(t1));
        if (t0[0]) {
            cJSON_AddStringToObject(o, "started_at", t0);
        }
        if (t1[0]) {
            cJSON_AddStringToObject(o, "ended_at", t1);
        }
        cJSON_AddItemToArray(events, o);
        strlcpy(kept_ids[kept++], e->event_id, sizeof(kept_ids[0]));
    }
    xSemaphoreGive(s_lock);

    add_status_object(root, "status");
    add_contacts_array(root);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return;
    }

    char url[160];
    snprintf(url, sizeof(url), "%s/api/kid-phone/v1/events", cloud->base_url);
    char resp[512];
    int status = 0;
    bool ok = http_json("POST", url, cloud->device_token, body, resp, sizeof(resp), &status);
    free(body);
    if (!ok) {
        if (status == 401) {
            on_auth_rejected(cloud);
        } else {
            set_err("events push failed");
        }
        return;
    }

    /* Drop accepted events from the ring. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < kept; k++) {
        for (int i = 0; i < APP_CLOUD_LOG_CAPACITY; i++) {
            if (s_log[i].used && strcmp(s_log[i].event_id, kept_ids[k]) == 0) {
                s_log[i].used = false;
            }
        }
    }
    log_event_t tmp[APP_CLOUD_LOG_CAPACITY];
    int n = 0;
    for (int i = 0; i < APP_CLOUD_LOG_CAPACITY; i++) {
        int idx = (s_log_head + i) % APP_CLOUD_LOG_CAPACITY;
        if (s_log[idx].used) {
            tmp[n++] = s_log[idx];
        }
    }
    memset(s_log, 0, sizeof(s_log));
    memcpy(s_log, tmp, sizeof(log_event_t) * n);
    s_log_head = 0;
    s_log_count = n;
    xSemaphoreGive(s_lock);

    set_err(NULL);
    s_last_ok_ms = now_ms();
}

static void poll_pairing(cfg_cloud_t *cloud)
{
    if (!cloud->pair_code[0] || cloud->device_uuid[0]) {
        return;
    }
    char url[160];
    snprintf(url, sizeof(url), "%s/api/kid-phone/v1/pair/complete", cloud->base_url);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "code", cloud->pair_code);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return;
    }
    char resp[1536];
    int status = 0;
    bool ok = http_json("POST", url, NULL, body, resp, sizeof(resp), &status);
    free(body);
    if (!ok) {
        if (status == 410) {
            set_err("pairing code expired");
            config_set_cloud_pairing_code("");
        } else if (status == 409) {
            set_err("pairing already finished; show a new code");
            config_set_cloud_pairing_code("");
        }
        return;
    }
    cJSON *j = cJSON_Parse(resp);
    if (!j) {
        return;
    }
    const cJSON *st = cJSON_GetObjectItem(j, "status");
    if (cJSON_IsString(st) && strcmp(st->valuestring, "waiting") == 0) {
        /* Owner has not claimed the code yet. */
    } else if (cJSON_IsString(st) && strcmp(st->valuestring, "paired") == 0) {
        const cJSON *uuid = cJSON_GetObjectItem(j, "device_uuid");
        const cJSON *tok = cJSON_GetObjectItem(j, "device_token");
        const cJSON *base = cJSON_GetObjectItem(j, "base_url");
        const cJSON *name = cJSON_GetObjectItem(j, "device_name");
        if (cJSON_IsString(uuid) && cJSON_IsString(tok) && uuid->valuestring[0] && tok->valuestring[0]) {
            const char *url_in = (cJSON_IsString(base) && base->valuestring[0]) ? base->valuestring : NULL;
            const char *name_in = (cJSON_IsString(name) && name->valuestring[0]) ? name->valuestring : NULL;
            if (config_set_cloud_device(uuid->valuestring, tok->valuestring, url_in, name_in) == ESP_OK) {
                ESP_LOGI(TAG, "paired with Bell-Towne app");
                set_err(NULL);
                s_last_ok_ms = now_ms();
            } else {
                set_err("could not store device token");
            }
        }
    }
    cJSON_Delete(j);
}

static void sync_once(void)
{
    if (!wifi_is_connected()) {
        return;
    }
    cfg_cloud_t cloud;
    config_get_cloud(&cloud);
    if (cloud.pair_code[0] && !cloud.device_uuid[0]) {
        poll_pairing(&cloud);
        config_get_cloud(&cloud);
    }
    if (!cloud.device_uuid[0] || !cloud.device_token[0]) {
        mbedtls_platform_zeroize(&cloud, sizeof(cloud));
        return;
    }
    push_events(&cloud);
    if (cloud.device_token[0]) {
        pull_commands(&cloud);
    }
    mbedtls_platform_zeroize(&cloud, sizeof(cloud));
}

static void sync_task(void *arg)
{
    (void)arg;
    while (1) {
        sync_once();
        TickType_t wait = pdMS_TO_TICKS(APP_CLOUD_SYNC_MS);
        cfg_cloud_t cloud;
        config_get_cloud(&cloud);
        if (cloud.pair_code[0] && !cloud.device_uuid[0]) {
            wait = pdMS_TO_TICKS(APP_CLOUD_PAIR_POLL_MS);
        }
        mbedtls_platform_zeroize(&cloud, sizeof(cloud));
        xSemaphoreTake(s_wake, wait);
    }
}

void cloud_sync_start(void)
{
    if (s_started) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    s_started = true;
    xTaskCreatePinnedToCore(sync_task, "cloud_sync", APP_CLOUD_SYNC_STACK, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "cloud sync task started (default %s)", APP_CLOUD_BASE_URL_DEFAULT);
}

void cloud_sync_kick(void)
{
    if (s_wake) {
        xSemaphoreGive(s_wake);
    }
}

void cloud_sync_log_event(int contact_slot, const char *contact_name, const char *outcome,
                          uint32_t duration_ms, int64_t started_ms, int64_t ended_ms)
{
    if (!s_lock || !outcome) {
        return;
    }
    /* Callers historically passed esp_timer uptime; only keep values that
     * already look like Unix epoch ms. Otherwise stamp from NTP wall clock. */
    const int64_t wall_floor = (int64_t)CLOUD_TIME_PLAUSIBLE * 1000;
    if (ended_ms < wall_floor) {
        ended_ms = wall_ms_now();
        if (ended_ms > 0) {
            started_ms = ended_ms - (int64_t)duration_ms;
            if (started_ms <= 0 || started_ms > ended_ms) {
                started_ms = ended_ms;
            }
        } else {
            started_ms = 0;
        }
    } else if (started_ms < wall_floor || started_ms > ended_ms) {
        started_ms = ended_ms - (int64_t)duration_ms;
        if (started_ms < wall_floor) {
            started_ms = ended_ms;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_log_count >= APP_CLOUD_LOG_CAPACITY) {
        s_log[s_log_head].used = false;
        s_log_head = (s_log_head + 1) % APP_CLOUD_LOG_CAPACITY;
        s_log_count--;
    }
    int idx = (s_log_head + s_log_count) % APP_CLOUD_LOG_CAPACITY;
    log_event_t *e = &s_log[idx];
    memset(e, 0, sizeof(*e));
    snprintf(e->event_id, sizeof(e->event_id), "%08lx-%04x",
             (unsigned long)((ended_ms > 0 ? ended_ms : now_ms()) & 0xffffffff),
             (unsigned)(esp_random() & 0xffff));
    e->contact_slot = contact_slot;
    if (contact_name) {
        strlcpy(e->contact_name, contact_name, sizeof(e->contact_name));
    }
    strlcpy(e->outcome, outcome, sizeof(e->outcome));
    e->duration_ms = duration_ms;
    e->started_ms = started_ms;
    e->ended_ms = ended_ms;
    e->used = true;
    s_log_count++;
    xSemaphoreGive(s_lock);
    cloud_sync_kick();
}

esp_err_t cloud_sync_pair_begin(char *code_out, size_t code_len)
{
    if (!wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    cfg_cloud_t cloud;
    config_get_cloud(&cloud);
    char code[8];
    make_pair_code(code, sizeof(code));
    char url[160];
    snprintf(url, sizeof(url), "%s/api/kid-phone/v1/pair/begin", cloud.base_url);
    char mac[24];
    mac_str(mac, sizeof(mac));
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "code", code);
    cJSON_AddStringToObject(req, "mac", mac);
    cJSON_AddStringToObject(req, "firmware_version", esp_app_get_description()->version);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        mbedtls_platform_zeroize(&cloud, sizeof(cloud));
        return ESP_ERR_NO_MEM;
    }
    char resp[512];
    int status = 0;
    bool ok = http_json("POST", url, NULL, body, resp, sizeof(resp), &status);
    free(body);
    const bool was_paired = cloud.device_uuid[0] && cloud.device_token[0];
    mbedtls_platform_zeroize(&cloud, sizeof(cloud));
    if (!ok) {
        set_err("pair begin failed");
        return ESP_FAIL;
    }
    /* Re-pair: drop the old device token so poll_pairing can complete and the
     * setup page shows Waiting + the code (not "still Paired"). */
    if (was_paired) {
        (void)config_clear_cloud();
    }
    if (config_set_cloud_pairing_code(code) != ESP_OK) {
        return ESP_FAIL;
    }
    if (code_out && code_len) {
        strlcpy(code_out, code, code_len);
    }
    set_err(NULL);
    cloud_sync_kick();
    return ESP_OK;
}

esp_err_t cloud_sync_unlink(void)
{
    esp_err_t err = config_clear_cloud();
    set_err(NULL);
    cloud_sync_kick();
    return err;
}

void cloud_sync_status(cloud_sync_status_t *out)
{
    memset(out, 0, sizeof(*out));
    cfg_cloud_t cloud;
    config_get_cloud(&cloud);
    out->paired = cloud.device_uuid[0] && cloud.device_token[0];
    out->pairing = cloud.pair_code[0] && !out->paired;
    strlcpy(out->pair_code, cloud.pair_code, sizeof(out->pair_code));
    strlcpy(out->device_uuid, cloud.device_uuid, sizeof(out->device_uuid));
    strlcpy(out->base_url, cloud.base_url, sizeof(out->base_url));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    out->last_ok_ms = s_last_ok_ms;
    mbedtls_platform_zeroize(&cloud, sizeof(cloud));
}
