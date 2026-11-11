/*
 * Spoken prompts: short clips generated offline (scripts/gen_prompts.py,
 * Piper TTS). English is embedded in the firmware; other languages are
 * downloadable packs on /storage (see lang_pack.h).
 *
 * Any task may start a prompt; the audio task plays it (upsampled to the
 * earpiece rate) while app_mode_get() reports MODE_PROMPT, then the normal
 * idle sound (silence by default, see APP_IDLE_DIAL_TONE) comes back. A new
 * prompt replaces one that is playing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROMPT_RATE 16000

/* Language codes stored in NVS "lang". Numbers are stable: append only. */
typedef enum {
    PROMPT_LANG_EN = 0, /* embedded src/prompts/<name>.pcm */
    PROMPT_LANG_CA,     /* pack /storage/lang/ca/ */
    PROMPT_LANG_ES,
    PROMPT_LANG_FR,
    PROMPT_LANG_IT,
    PROMPT_LANG_NL,
    PROMPT_LANG_DE,
    PROMPT_LANG_VI,
    PROMPT_LANG_PT_BR, /* code "pt-br" */
    PROMPT_LANG_COUNT
} prompt_lang_t;

typedef enum {
    PROMPT_NONE = 0,
    PROMPT_TIME_UP,
    PROMPT_UNAVAILABLE,
    PROMPT_NO_ANSWER,
    PROMPT_NO_WIFI,
    PROMPT_NOT_READY,
    PROMPT_CANT_CALL,
    PROMPT_SLEEP_TIME,
    PROMPT_SECRET_START,
    PROMPT_SECRET_SENT,
    PROMPT_COUNT
} prompt_id_t;

void prompt_set_lang(prompt_lang_t lang);
prompt_lang_t prompt_lang(void);

/* "en", "ca", … "pt-br". Unknown / NULL → PROMPT_LANG_COUNT. */
const char *prompt_lang_code(prompt_lang_t lang);
prompt_lang_t prompt_lang_from_code(const char *code);

void prompt_play(prompt_id_t id);
bool prompt_active(void);
uint32_t prompt_duration_ms(prompt_id_t id);
const char *prompt_name(prompt_id_t id);

prompt_id_t prompt_poll(uint32_t *req);
void prompt_done(uint32_t req);

/* Clip data: n little-endian int16 samples at PROMPT_RATE. For pack languages
 * the buffer is in SPIRAM and stays valid until the next prompt_clip/play. */
const uint8_t *prompt_clip(prompt_id_t id, size_t *n);
