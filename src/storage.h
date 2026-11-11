/*
 * LittleFS on the "storage" partition: language packs and other downloadable
 * assets. Not part of the OTA app image; survives firmware updates.
 */
#pragma once

#include <stdbool.h>

/* Mount /storage (format once if empty). Call early in app_main. */
void storage_init(void);

/* True after a successful mount. */
bool storage_ready(void);
