#include "prompts.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lang_pack.h"

static const char *TAG = "prompt";

#define CLIP(name)                                                          \
    extern const uint8_t name##_start[] asm("_binary_" #name "_pcm_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_pcm_end");
CLIP(time_up)
CLIP(unavailable)
CLIP(no_answer)
CLIP(no_wifi)
CLIP(not_ready)
CLIP(cant_call)
CLIP(sleep_time)
CLIP(secret_start)
CLIP(secret_sent)

typedef struct {
    const char *name;
    const uint8_t *start, *end;
} clip_t;

#define ROW(id, name) [id] = {#name, name##_start, name##_end}

static const clip_t k_en[PROMPT_COUNT] = {
    [PROMPT_NONE] = {"none", NULL, NULL},
    ROW(PROMPT_TIME_UP, time_up),
    ROW(PROMPT_UNAVAILABLE, unavailable),
    ROW(PROMPT_NO_ANSWER, no_answer),
    ROW(PROMPT_NO_WIFI, no_wifi),
    ROW(PROMPT_NOT_READY, not_ready),
    ROW(PROMPT_CANT_CALL, cant_call),
    ROW(PROMPT_SLEEP_TIME, sleep_time),
    ROW(PROMPT_SECRET_START, secret_start),
    ROW(PROMPT_SECRET_SENT, secret_sent),
};

static const char *const k_lang_codes[PROMPT_LANG_COUNT] = {
    [PROMPT_LANG_EN] = "en",
    [PROMPT_LANG_CA] = "ca",
    [PROMPT_LANG_ES] = "es",
    [PROMPT_LANG_FR] = "fr",
    [PROMPT_LANG_IT] = "it",
    [PROMPT_LANG_NL] = "nl",
    [PROMPT_LANG_DE] = "de",
    [PROMPT_LANG_VI] = "vi",
    [PROMPT_LANG_PT_BR] = "pt-br",
};

static _Atomic int s_lang = PROMPT_LANG_EN;
static uint8_t *s_pack_buf;
static size_t s_pack_bytes;

static void free_pack_buf(void)
{
    if (s_pack_buf) {
        heap_caps_free(s_pack_buf);
        s_pack_buf = NULL;
        s_pack_bytes = 0;
    }
}

void prompt_set_lang(prompt_lang_t lang)
{
    if ((unsigned)lang >= PROMPT_LANG_COUNT) {
        lang = PROMPT_LANG_EN;
    }
    if (lang != PROMPT_LANG_EN && !lang_pack_installed(lang)) {
        ESP_LOGW(TAG, "language %s not installed, using English", k_lang_codes[lang]);
        lang = PROMPT_LANG_EN;
    }
    if (atomic_exchange(&s_lang, lang) != (int)lang) {
        ESP_LOGI(TAG, "language %s", k_lang_codes[lang]);
        free_pack_buf();
    }
}

prompt_lang_t prompt_lang(void)
{
    int l = atomic_load(&s_lang);
    return (unsigned)l < PROMPT_LANG_COUNT ? (prompt_lang_t)l : PROMPT_LANG_EN;
}

const char *prompt_lang_code(prompt_lang_t lang)
{
    return (unsigned)lang < PROMPT_LANG_COUNT ? k_lang_codes[lang] : k_lang_codes[PROMPT_LANG_EN];
}

prompt_lang_t prompt_lang_from_code(const char *code)
{
    for (int i = 0; code && i < PROMPT_LANG_COUNT; i++) {
        if (strcmp(code, k_lang_codes[i]) == 0) {
            return (prompt_lang_t)i;
        }
    }
    return PROMPT_LANG_COUNT;
}

static _Atomic int s_req_id = PROMPT_NONE;
static _Atomic uint32_t s_req_seq;
static _Atomic uint32_t s_done_seq;

void prompt_play(prompt_id_t id)
{
    if (id <= PROMPT_NONE || id >= PROMPT_COUNT) {
        return;
    }
    ESP_LOGI(TAG, "prompt %s, %s (%" PRIu32 " ms)", k_en[id].name, prompt_lang_code(prompt_lang()),
             prompt_duration_ms(id));
    atomic_store(&s_req_id, id);
    atomic_fetch_add(&s_req_seq, 1);
}

bool prompt_active(void)
{
    return atomic_load(&s_req_seq) != atomic_load(&s_done_seq);
}

uint32_t prompt_duration_ms(prompt_id_t id)
{
    size_t n = 0;
    prompt_clip(id, &n);
    return (uint32_t)(n * 1000 / PROMPT_RATE);
}

const char *prompt_name(prompt_id_t id)
{
    return (unsigned)id < PROMPT_COUNT ? k_en[id].name : "?";
}

prompt_id_t prompt_poll(uint32_t *req)
{
    uint32_t seq = atomic_load(&s_req_seq);
    if (seq == *req) {
        return PROMPT_NONE;
    }
    *req = seq;
    return (prompt_id_t)atomic_load(&s_req_id);
}

void prompt_done(uint32_t req)
{
    atomic_store(&s_done_seq, req);
}

static const uint8_t *load_pack_clip(prompt_id_t id, size_t *n)
{
    char path[96];
    char fname[32];
    snprintf(fname, sizeof(fname), "%s.pcm", k_en[id].name);
    if (!lang_pack_clip_path(prompt_lang(), fname, path, sizeof(path))) {
        *n = 0;
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "missing pack clip %s", path);
        *n = 0;
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        *n = 0;
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || (sz & 1)) {
        fclose(f);
        *n = 0;
        return NULL;
    }
    rewind(f);
    free_pack_buf();
    s_pack_buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pack_buf) {
        s_pack_buf = malloc((size_t)sz);
    }
    if (!s_pack_buf) {
        fclose(f);
        *n = 0;
        return NULL;
    }
    if (fread(s_pack_buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free_pack_buf();
        *n = 0;
        return NULL;
    }
    fclose(f);
    s_pack_bytes = (size_t)sz;
    *n = s_pack_bytes / 2;
    return s_pack_buf;
}

const uint8_t *prompt_clip(prompt_id_t id, size_t *n)
{
    if (id <= PROMPT_NONE || id >= PROMPT_COUNT) {
        *n = 0;
        return NULL;
    }
    prompt_lang_t lang = prompt_lang();
    if (lang != PROMPT_LANG_EN && lang_pack_installed(lang)) {
        const uint8_t *p = load_pack_clip(id, n);
        if (p) {
            return p;
        }
        /* Fall through to English if a clip is missing. */
    }
    const clip_t *c = &k_en[id];
    *n = (size_t)(c->end - c->start) / 2;
    return c->start;
}
