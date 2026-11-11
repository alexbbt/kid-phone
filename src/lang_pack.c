#include "lang_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "storage.h"

static const char *TAG = "lang_pack";
static const char *NS = "kidphone";
static const char *NVS_BASE = "lang_base";

static char s_base[160];

static void progress(lang_pack_progress_fn fn, void *user, const char *line)
{
    if (fn) {
        fn(line, user);
    }
}

static void load_base_url(void)
{
    if (s_base[0]) {
        return;
    }
    strlcpy(s_base, APP_LANG_PACK_BASE_DEFAULT, sizeof(s_base));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_base);
    if (nvs_get_str(h, NVS_BASE, s_base, &len) != ESP_OK || !s_base[0]) {
        strlcpy(s_base, APP_LANG_PACK_BASE_DEFAULT, sizeof(s_base));
    }
    nvs_close(h);
}

void lang_pack_get_base_url(char *out, size_t len)
{
    load_base_url();
    if (out && len) {
        strlcpy(out, s_base, len);
    }
}

esp_err_t lang_pack_set_base_url(const char *url)
{
    if (!url) {
        return ESP_ERR_INVALID_ARG;
    }
    while (*url == ' ' || *url == '\t') {
        url++;
    }
    size_t n = strnlen(url, sizeof(s_base));
    while (n > 0 && (url[n - 1] == '/' || url[n - 1] == ' ')) {
        n--;
    }
    if (n >= sizeof(s_base)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char tmp[sizeof(s_base)];
    memcpy(tmp, url, n);
    tmp[n] = '\0';
    err = n == 0 ? nvs_erase_key(h, NVS_BASE) : nvs_set_str(h, NVS_BASE, tmp);
    if (err == ESP_ERR_NVS_NOT_FOUND && n == 0) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        if (n == 0) {
            strlcpy(s_base, APP_LANG_PACK_BASE_DEFAULT, sizeof(s_base));
        } else {
            strlcpy(s_base, tmp, sizeof(s_base));
        }
    }
    return err;
}

static const char *clip_names[] = {
    "time_up.pcm", "unavailable.pcm", "no_answer.pcm", "no_wifi.pcm", "not_ready.pcm",
    "cant_call.pcm", "sleep_time.pcm", "secret_start.pcm", "secret_sent.pcm", "no_answer.wav",
};

bool lang_pack_clip_path(prompt_lang_t lang, const char *clip_name, char *out, size_t out_len)
{
    if (!out || !clip_name || lang == PROMPT_LANG_EN || (unsigned)lang >= PROMPT_LANG_COUNT) {
        return false;
    }
    snprintf(out, out_len, LANG_PACK_DIR_FMT "/%s", prompt_lang_code(lang), clip_name);
    return true;
}

bool lang_pack_installed(prompt_lang_t lang)
{
    if (lang == PROMPT_LANG_EN) {
        return true;
    }
    if (!storage_ready() || (unsigned)lang >= PROMPT_LANG_COUNT) {
        return false;
    }
    char path[96];
    /* Require the marker + every spoken clip (wav is optional for local play). */
    snprintf(path, sizeof(path), LANG_PACK_DIR_FMT "/manifest.json", prompt_lang_code(lang));
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(clip_names) / sizeof(clip_names[0]); i++) {
        if (strcmp(clip_names[i], "no_answer.wav") == 0) {
            continue; /* Twilio-only; prompts still work without it */
        }
        snprintf(path, sizeof(path), LANG_PACK_DIR_FMT "/%s", prompt_lang_code(lang), clip_names[i]);
        if (stat(path, &st) != 0) {
            return false;
        }
    }
    return true;
}

static bool mkdir_p_lang(const char *code)
{
    struct stat st;
    if (stat("/storage/lang", &st) != 0 && mkdir("/storage/lang", 0755) != 0) {
        return false;
    }
    char dir[64];
    snprintf(dir, sizeof(dir), "/storage/lang/%s", code);
    if (stat(dir, &st) != 0 && mkdir(dir, 0755) != 0) {
        return false;
    }
    return true;
}

typedef struct {
    FILE *f;
    int status;
} dl_ctx_t;

static esp_err_t dl_event(esp_http_client_event_t *evt)
{
    dl_ctx_t *c = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 && c && c->f) {
        if (fwrite(evt->data, 1, evt->data_len, c->f) != (size_t)evt->data_len) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static bool http_download(const char *url, const char *dest_path)
{
    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", dest_path);
        return false;
    }
    dl_ctx_t ctx = {.f = f, .status = -1};
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .event_handler = dl_event,
        .user_data = &ctx,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(f);
        unlink(dest_path);
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    fclose(f);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET %s -> err=%s status=%d", url, esp_err_to_name(err), status);
        unlink(dest_path);
        return false;
    }
    return true;
}

typedef struct {
    char *buf;
    int len;
    int cap;
} http_mem_t;

static esp_err_t http_mem_event(esp_http_client_event_t *evt)
{
    http_mem_t *m = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 && m && m->buf) {
        if (m->len + evt->data_len < m->cap - 1) {
            memcpy(m->buf + m->len, evt->data, evt->data_len);
            m->len += evt->data_len;
            m->buf[m->len] = '\0';
        }
    }
    return ESP_OK;
}

static char *http_get_alloc(const char *url, int cap)
{
    char *buf = calloc(1, cap);
    if (!buf) {
        return NULL;
    }
    http_mem_t mem = {.buf = buf, .len = 0, .cap = cap};
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .event_handler = http_mem_event,
        .user_data = &mem,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf);
        return NULL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        free(buf);
        return NULL;
    }
    return buf;
}

bool lang_pack_install(prompt_lang_t lang, lang_pack_progress_fn cb, void *user)
{
    if (lang == PROMPT_LANG_EN) {
        progress(cb, user, "English is built in.");
        return true;
    }
    if (!storage_ready() || (unsigned)lang >= PROMPT_LANG_COUNT) {
        progress(cb, user, "Storage is not ready.");
        return false;
    }
    load_base_url();
    if (!s_base[0]) {
        progress(cb, user, "No language pack URL configured.");
        return false;
    }
    const char *code = prompt_lang_code(lang);
    if (!mkdir_p_lang(code)) {
        progress(cb, user, "Could not create the language folder.");
        return false;
    }

    char url[200], dest[96], line[96];
    snprintf(url, sizeof(url), "%s/%s/manifest.json", s_base, code);
    snprintf(line, sizeof(line), "Downloading %s manifest…", code);
    progress(cb, user, line);

    char *body = http_get_alloc(url, 8192);
    if (!body) {
        progress(cb, user, "Could not download the language manifest.");
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        progress(cb, user, "Language manifest was not valid JSON.");
        return false;
    }
    snprintf(dest, sizeof(dest), LANG_PACK_DIR_FMT "/manifest.json", code);
    /* Re-serialize a clean copy to disk. */
    char *raw = cJSON_PrintUnformatted(root);
    if (raw) {
        FILE *f = fopen(dest, "wb");
        if (f) {
            fputs(raw, f);
            fclose(f);
        }
        free(raw);
    }

    const cJSON *files = cJSON_GetObjectItem(root, "files");
    if (!cJSON_IsArray(files)) {
        cJSON_Delete(root);
        progress(cb, user, "Language manifest has no files list.");
        return false;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, files)
    {
        const cJSON *name = cJSON_GetObjectItem(it, "name");
        const cJSON *src = cJSON_GetObjectItem(it, "url");
        if (!cJSON_IsString(name) || !cJSON_IsString(src)) {
            continue;
        }
        snprintf(dest, sizeof(dest), LANG_PACK_DIR_FMT "/%s", code, name->valuestring);
        snprintf(line, sizeof(line), "Downloading %s…", name->valuestring);
        progress(cb, user, line);
        /* Relative urls are under the pack base. */
        if (strncmp(src->valuestring, "http://", 7) == 0 || strncmp(src->valuestring, "https://", 8) == 0) {
            strlcpy(url, src->valuestring, sizeof(url));
        } else {
            snprintf(url, sizeof(url), "%s/%s/%s", s_base, code, src->valuestring);
        }
        if (!http_download(url, dest)) {
            cJSON_Delete(root);
            progress(cb, user, "A language file failed to download.");
            return false;
        }
    }
    cJSON_Delete(root);
    if (!lang_pack_installed(lang)) {
        progress(cb, user, "Pack installed but some clips are missing.");
        return false;
    }
    progress(cb, user, "Language pack installed.");
    return true;
}

bool lang_pack_remove(prompt_lang_t lang)
{
    if (lang == PROMPT_LANG_EN || !storage_ready()) {
        return false;
    }
    const char *code = prompt_lang_code(lang);
    char path[96];
    for (size_t i = 0; i < sizeof(clip_names) / sizeof(clip_names[0]); i++) {
        snprintf(path, sizeof(path), LANG_PACK_DIR_FMT "/%s", code, clip_names[i]);
        unlink(path);
    }
    snprintf(path, sizeof(path), LANG_PACK_DIR_FMT "/manifest.json", code);
    unlink(path);
    snprintf(path, sizeof(path), "/storage/lang/%s", code);
    rmdir(path);
    return true;
}
