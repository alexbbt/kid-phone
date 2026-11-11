/*
 * HTTPS firmware OTA into the inactive ota_ slot, then reboot.
 * Requires the dual-slot partition table (see partitions.csv).
 */
#pragma once

#include <stdbool.h>

typedef void (*ota_progress_fn)(const char *line, void *user);

/* Download firmware from url and switch boot partition. Reboots on success.
 * Returns false if the update failed (device keeps running). */
bool ota_update_from_url(const char *url, ota_progress_fn progress, void *user);
