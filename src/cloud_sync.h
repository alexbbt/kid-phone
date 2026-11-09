/*
 * Bell-Towne app cloud sync: outbound HTTPS only (NAT-safe).
 *
 * Pairing code on the setup page → manager claims it in the app → phone
 * polls for access/refresh tokens (NVS). A background task pushes call-log
 * events and device health, then pulls schedule/bedtime commands and applies
 * them through config_set_contacts / config_set_bedtime (same path as local
 * UI / MQTT). Certificate validation is always on.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void cloud_sync_start(void);

/* Wake the sync task early (after pairing / unlink / queued log). */
void cloud_sync_kick(void);

/* Queue a call-log event (bounded ring; oldest dropped if app unreachable). */
void cloud_sync_log_event(int contact_slot, const char *contact_name, const char *outcome,
                          uint32_t duration_ms, int64_t started_ms, int64_t ended_ms);

/* Begin pairing: generate a code, register with the app, store code in NVS. */
esp_err_t cloud_sync_pair_begin(char *code_out, size_t code_len);

/* Clear cloud credentials (unlink). Calling still works locally. */
esp_err_t cloud_sync_unlink(void);

/* Short status for /api/status (no tokens). */
typedef struct {
    bool paired;
    bool pairing;
    char pair_code[8];
    char device_uuid[40];
    char base_url[96];
    char last_error[96];
    int64_t last_ok_ms;
} cloud_sync_status_t;

void cloud_sync_status(cloud_sync_status_t *out);
