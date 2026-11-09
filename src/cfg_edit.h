/*
 * Shared contact / bedtime / settings edits for the setup page and MQTT.
 * Validation and Twilio number sync live here so both paths behave the same;
 * NVS (config_store) is authoritative after a confirmed save.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "config_store.h"
#include "esp_err.h"

/* Parse contacts JSON the setup page / MQTT use. Returns NULL if fine, else
 * a message in err (or a static string). out = CFG_MAX_CONTACTS slots. */
const char *cfg_edit_parse_contacts(const cJSON *arr, cfg_contact_t *out, char *err, size_t err_len);

/* Same set of numbers (order and duplicates ignored)? */
bool cfg_edit_same_numbers(const cfg_contact_t *a, const cfg_contact_t *b);

typedef enum {
    CFG_EDIT_OK = 0,
    CFG_EDIT_BAD_REQUEST,   /* err filled */
    CFG_EDIT_BUSY,          /* Twilio setup / update already running */
    CFG_EDIT_NO_WIFI,
    CFG_EDIT_NEEDS_TOKEN,   /* numbers changed and no restricted key */
    CFG_EDIT_UPDATING,      /* Twilio number sync started; *run_id set */
    CFG_EDIT_SAVE_FAIL,
} cfg_edit_result_t;

/* Save contacts. When the set of numbers changed and the phone is
 * provisioned, starts a Twilio ALLOWED update (with the stored key) instead
 * of writing NVS here; the provision task saves when Twilio accepts.
 * run_id (may be NULL) is set on CFG_EDIT_UPDATING. err (may be NULL) gets a
 * parent-facing message on CFG_EDIT_BAD_REQUEST / NEEDS_TOKEN. */
cfg_edit_result_t cfg_edit_save_contacts(const cfg_contact_t *ct, uint32_t *run_id, char *err, size_t err_len);

/* Normalize one E.164 number (same rules as the setup page). */
bool cfg_edit_normalize_number(const char *in, char *out, size_t len);

/* Apply bedtime after the caller filled b (times, tz). Validates. */
esp_err_t cfg_edit_set_bedtime(const cfg_bedtime_t *b, char *err, size_t err_len);

/* Apply longest call in minutes. */
esp_err_t cfg_edit_set_max_call_minutes(unsigned minutes, char *err, size_t err_len);

/* How many contact slots the UI / MQTT should expose (wired buttons, and any
 * further slot that still has a number or secret mode). */
int cfg_edit_contact_slots(void);
