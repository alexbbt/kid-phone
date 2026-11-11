#include "prompts.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "prompt";

/* English clips only in this build (src/prompts/<name>.pcm). Other language
 * files remain in the tree for multi-language builds. */
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

static const clip_t k_clips[PROMPT_COUNT] = {
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
};

/* Set by the setup server or at boot, read by the audio task when a prompt
 * starts: one atomic int, no lock. Always English in this build. */
static _Atomic int s_lang = PROMPT_LANG_EN;

void prompt_set_lang(prompt_lang_t lang)
{
    (void)lang;
    if (atomic_exchange(&s_lang, PROMPT_LANG_EN) != PROMPT_LANG_EN) {
        ESP_LOGI(TAG, "language %s", k_lang_codes[PROMPT_LANG_EN]);
    }
}

prompt_lang_t prompt_lang(void)
{
    return PROMPT_LANG_EN;
}

const char *prompt_lang_code(prompt_lang_t lang)
{
    (void)lang;
    return k_lang_codes[PROMPT_LANG_EN];
}

prompt_lang_t prompt_lang_from_code(const char *code)
{
    if (code && strcmp(code, k_lang_codes[PROMPT_LANG_EN]) == 0) {
        return PROMPT_LANG_EN;
    }
    return PROMPT_LANG_COUNT;
}

/* Requests are numbered; the prompt is active until the audio task reports
 * the latest number done. The id is written before the number, so the audio
 * task never starts a request without its id. */
static _Atomic int s_req_id = PROMPT_NONE;
static _Atomic uint32_t s_req_seq;
static _Atomic uint32_t s_done_seq;

void prompt_play(prompt_id_t id)
{
    if (id <= PROMPT_NONE || id >= PROMPT_COUNT) {
        return;
    }
    ESP_LOGI(TAG, "prompt %s, %s (%" PRIu32 " ms)", k_clips[id].name, prompt_lang_code(PROMPT_LANG_EN),
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
    return (unsigned)id < PROMPT_COUNT ? k_clips[id].name : "?";
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

const uint8_t *prompt_clip(prompt_id_t id, size_t *n)
{
    if (id <= PROMPT_NONE || id >= PROMPT_COUNT) {
        *n = 0;
        return NULL;
    }
    const clip_t *c = &k_clips[id];
    *n = (size_t)(c->end - c->start) / 2;
    return c->start;
}
