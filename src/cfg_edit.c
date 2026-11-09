#include "cfg_edit.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "schedule.h"
#include "twilio_provision.h"
#include "wifi.h"

static const char *TAG = "cfg_edit";

bool cfg_edit_normalize_number(const char *in, char *out, size_t len)
{
    size_t n = 0;
    const char *p = in;
    while (*p == ' ') {
        p++;
    }
    if (p[0] == '0' && p[1] == '0') {
        p += 2;
    } else if (*p == '+') {
        p++;
    } else {
        return false;
    }
    out[n++] = '+';
    for (; *p && n + 1 < len; p++) {
        if (isdigit((unsigned char)*p)) {
            out[n++] = *p;
        } else if (!strchr(" -.()/", *p)) {
            return false;
        }
    }
    out[n] = '\0';
    size_t digits = n - 1;
    return digits >= 8 && digits <= 15 && out[1] != '0';
}

static void trim_copy(const char *in, char *out, size_t len)
{
    while (*in && isspace((unsigned char)*in)) {
        in++;
    }
    strlcpy(out, in, len);
    size_t n = strlen(out);
    while (n && isspace((unsigned char)out[n - 1])) {
        out[--n] = '\0';
    }
}

static const char *json_str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int json_minutes(const cJSON *v)
{
    if (cJSON_IsNumber(v)) {
        return v->valueint;
    }
    if (cJSON_IsString(v)) {
        int h, m;
        if (sscanf(v->valuestring, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
            return h * 60 + m;
        }
    }
    return -1;
}

/* Plain email: one "@", a dot after it, no spaces or SMTP-hostile chars. */
static bool email_ok(const char *e)
{
    const size_t n = strlen(e);
    const char *at = strchr(e, '@');
    if (n < 6 || n > 63 || !at || at == e || strchr(at + 1, '@') || !strchr(at + 1, '.') || e[n - 1] == '.') {
        return false;
    }
    for (const char *p = e; *p; p++) {
        if ((unsigned char)*p <= ' ' || (unsigned char)*p >= 0x7f || strchr("<>()[]\\,;:\"", *p)) {
            return false;
        }
    }
    return true;
}

const char *cfg_edit_parse_contacts(const cJSON *arr, cfg_contact_t *out, char *err, size_t err_len)
{
    memset(out, 0, sizeof(cfg_contact_t) * CFG_MAX_CONTACTS);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > CFG_MAX_CONTACTS) {
        return "Bad contact list.";
    }
    cfg_pins_t pins;
    config_get_pins(&pins);
    const int nbuttons = pins.count;
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cfg_contact_t *ct = &out[i];
        const char *num = json_str(it, "number");
        trim_copy(json_str(it, "name"), ct->name, sizeof(ct->name));
        char trimmed[32];
        trim_copy(num, trimmed, sizeof(trimmed));
        if (trimmed[0] && !cfg_edit_normalize_number(trimmed, ct->number, sizeof(ct->number))) {
            snprintf(err, err_len,
                     "Button %d: write the phone number in international format, starting with + and the country "
                     "code, like +1 555 123 4567.",
                     i + 1);
            return err;
        }
        if (ct->number[0] && i >= nbuttons) {
            snprintf(err, err_len,
                     "Button %d has no pin on the phone. Add it under Buttons and lights first, or leave it empty.",
                     i + 1);
            return err;
        }
        const char *mode = json_str(it, "secret");
        if (!mode[0] || strcmp(mode, "off") == 0) {
            ct->secret = CFG_SECRET_OFF;
        } else if (strcmp(mode, "hold") == 0) {
            ct->secret = CFG_SECRET_HOLD;
        } else if (strcmp(mode, "dedicated") == 0) {
            ct->secret = CFG_SECRET_DEDICATED;
        } else {
            snprintf(err, err_len, "Button %d: unknown secret message setting.", i + 1);
            return err;
        }
        if (ct->secret != CFG_SECRET_OFF) {
            char em[128];
            trim_copy(json_str(it, "email"), em, sizeof(em));
            if (!email_ok(em)) {
                snprintf(err, err_len, "Button %d: %s", i + 1,
                         em[0] ? "that email address doesn't look right."
                               : "add the email address the secret messages go to.");
                return err;
            }
            strlcpy(ct->email, em, sizeof(ct->email));
            if (i >= nbuttons) {
                snprintf(err, err_len,
                         "Button %d has no pin on the phone. Add it under Buttons and lights first, or turn its "
                         "secret messages off.",
                         i + 1);
                return err;
            }
        }
        if (ct->secret == CFG_SECRET_HOLD && !ct->number[0]) {
            snprintf(err, err_len,
                     "Button %d: with Long press, a short press still calls, so it needs a phone number. Pick "
                     "Secret button only for a button that never calls.",
                     i + 1);
            return err;
        }
        if (ct->secret == CFG_SECRET_DEDICATED && ct->number[0]) {
            snprintf(err, err_len,
                     "Button %d is a secret button only, so it doesn't call anyone. Clear its phone number, or "
                     "pick Long press.",
                     i + 1);
            return err;
        }
        if (ct->number[0] && !ct->name[0]) {
            snprintf(ct->name, sizeof(ct->name), "Contact %d", i + 1);
        }
        const cJSON *js = cJSON_GetObjectItem(it, "schedule");
        cfg_schedule_t *sc = &ct->sched;
        if (cJSON_IsObject(js)) {
            sc->enabled = cJSON_IsTrue(cJSON_GetObjectItem(js, "enabled"));
            strlcpy(sc->tz, json_str(js, "tz"), sizeof(sc->tz));
            strlcpy(sc->tz_name, json_str(js, "tz_name"), sizeof(sc->tz_name));
            const cJSON *wins = cJSON_GetObjectItem(js, "windows");
            const cJSON *w;
            cJSON_ArrayForEach(w, wins) {
                if (sc->nwin == CFG_MAX_WINDOWS) {
                    break;
                }
                const cJSON *d = cJSON_GetObjectItem(w, "days");
                int start = json_minutes(cJSON_GetObjectItem(w, "start"));
                int end = json_minutes(cJSON_GetObjectItem(w, "end"));
                if (!cJSON_IsNumber(d) || start < 0 || start >= 1440 || end < 0 || end >= 1440) {
                    snprintf(err, err_len, "Button %d: a time is not valid.", i + 1);
                    return err;
                }
                sc->win[sc->nwin++] = (cfg_window_t){
                    .days = (uint8_t)(d->valueint & 0x7f), .start_min = (uint16_t)start, .end_min = (uint16_t)end};
            }
        }
        const char *why = ct->number[0] ? schedule_validate(sc) : NULL;
        if (why) {
            snprintf(err, err_len, "%s: %s.", ct->name, why);
            return err;
        }
        if (!ct->number[0]) {
            memset(sc, 0, sizeof(*sc));
        }
        i++;
    }
    if (!config_any_contact(out) && !config_any_dedicated(out)) {
        return "Add at least one person to call, with their phone number, or a secret button.";
    }
    return NULL;
}

bool cfg_edit_same_numbers(const cfg_contact_t *a, const cfg_contact_t *b)
{
    for (int pass = 0; pass < 2; pass++) {
        const cfg_contact_t *x = pass ? b : a, *y = pass ? a : b;
        for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
            if (!x[i].number[0]) {
                continue;
            }
            bool found = false;
            for (int j = 0; j < CFG_MAX_CONTACTS && !found; j++) {
                found = strcmp(x[i].number, y[j].number) == 0;
            }
            if (!found) {
                return false;
            }
        }
    }
    return true;
}

cfg_edit_result_t cfg_edit_save_contacts(const cfg_contact_t *ct, uint32_t *run_id, char *err, size_t err_len)
{
    if (run_id) {
        *run_id = 0;
    }
    if (twilio_provision_running()) {
        if (err && err_len) {
            strlcpy(err, "Setup is running. Try again when it has finished.", err_len);
        }
        return CFG_EDIT_BUSY;
    }
    app_cfg_t *c = malloc(sizeof(app_cfg_t));
    if (!c) {
        if (err && err_len) {
            strlcpy(err, "The phone is busy. Try again.", err_len);
        }
        return CFG_EDIT_SAVE_FAIL;
    }
    config_get(c);
    bool provisioned = c->provisioned;
    bool same = cfg_edit_same_numbers(ct, c->contacts);
    config_wipe(c);
    free(c);

    if (provisioned && !config_any_contact(ct)) {
        if (err && err_len) {
            strlcpy(err, "Keep at least one person to call while the phone is connected to Twilio.", err_len);
        }
        return CFG_EDIT_BAD_REQUEST;
    }
    if (provisioned && !same && config_has_twilio_key()) {
        if (!wifi_is_connected()) {
            if (err && err_len) {
                strlcpy(err, "The phone is not on WiFi.", err_len);
            }
            return CFG_EDIT_NO_WIFI;
        }
        if (!twilio_provision_start_with_key(ct)) {
            if (err && err_len) {
                strlcpy(err, "The update could not start. Try again.", err_len);
            }
            return CFG_EDIT_BUSY;
        }
        ESP_LOGI(TAG, "number update with the stored key started");
        if (run_id) {
            *run_id = twilio_provision_run_id();
        }
        return CFG_EDIT_UPDATING;
    }
    const bool secrets_only = !provisioned && !config_any_contact(ct);
    if (!secrets_only && (!provisioned || !same)) {
        if (err && err_len) {
            strlcpy(err,
                    provisioned ? "The phone numbers changed, so Twilio has to be told. Enter your Auth Token on "
                                  "the setup page."
                                : "Connect to Twilio first.",
                    err_len);
        }
        return CFG_EDIT_NEEDS_TOKEN;
    }
    if (config_set_contacts(ct) != ESP_OK) {
        if (err && err_len) {
            strlcpy(err, "Could not save. Try again.", err_len);
        }
        return CFG_EDIT_SAVE_FAIL;
    }
    ESP_LOGI(TAG, "contacts and schedules saved");
    return CFG_EDIT_OK;
}

esp_err_t cfg_edit_set_bedtime(const cfg_bedtime_t *b, char *err, size_t err_len)
{
    if (b->start_min >= 1440 || b->end_min >= 1440) {
        if (err && err_len) {
            strlcpy(err, "Fill in both bedtime times.", err_len);
        }
        return ESP_ERR_INVALID_ARG;
    }
    const char *why = schedule_bedtime_validate(b);
    if (why) {
        if (err && err_len) {
            snprintf(err, err_len, "Bedtime: %s.", why);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config_set_bedtime(b) != ESP_OK) {
        if (err && err_len) {
            strlcpy(err, "Could not save. Try again.", err_len);
        }
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "bedtime %s (%02u:%02u to %02u:%02u)", b->enabled ? "on" : "off", b->start_min / 60,
             b->start_min % 60, b->end_min / 60, b->end_min % 60);
    return ESP_OK;
}

esp_err_t cfg_edit_set_max_call_minutes(unsigned minutes, char *err, size_t err_len)
{
    if (minutes < APP_CALL_MAX_MIN_LO || minutes > APP_CALL_MAX_MIN_HI) {
        if (err && err_len) {
            snprintf(err, err_len, "Pick a call length from %d to %d minutes.", APP_CALL_MAX_MIN_LO,
                     APP_CALL_MAX_MIN_HI);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config_set_max_call_minutes(minutes) != ESP_OK) {
        if (err && err_len) {
            strlcpy(err, "Could not save. Try again.", err_len);
        }
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "max call length set to %u min", minutes);
    return ESP_OK;
}

int cfg_edit_contact_slots(void)
{
    /* Same rule as GET /api/status: wired buttons, plus any further slot that
     * still has a number or a secret mode. */
    app_cfg_t *c = malloc(sizeof(app_cfg_t));
    if (!c) {
        cfg_pins_t pins;
        config_get_pins(&pins);
        return pins.count > 0 ? pins.count : 1;
    }
    config_get(c);
    int nslots = c->pins.count;
    for (int i = nslots; i < CFG_MAX_CONTACTS; i++) {
        if (c->contacts[i].number[0] || c->contacts[i].secret != CFG_SECRET_OFF) {
            nslots = i + 1;
        }
    }
    config_wipe(c);
    free(c);
    return nslots > 0 ? nslots : 1;
}
