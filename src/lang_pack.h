/*
 * Downloadable voice-language packs on /storage/lang/<code>/.
 * English stays embedded in the firmware; other languages are fetched over
 * HTTPS from a pack base URL (NVS or APP_LANG_PACK_BASE_DEFAULT).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "prompts.h"

#define LANG_PACK_DIR_FMT "/storage/lang/%s"

bool lang_pack_installed(prompt_lang_t lang);
bool lang_pack_clip_path(prompt_lang_t lang, const char *clip_name, char *out, size_t out_len);

typedef void (*lang_pack_progress_fn)(const char *line, void *user);
bool lang_pack_install(prompt_lang_t lang, lang_pack_progress_fn progress, void *user);
bool lang_pack_remove(prompt_lang_t lang);

void lang_pack_get_base_url(char *out, size_t len);
esp_err_t lang_pack_set_base_url(const char *url);
