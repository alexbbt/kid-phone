#include "mqtt_ha.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "mbedtls/platform_util.h"

#include "app_config.h"
#include "cfg_edit.h"
#include "config_store.h"
#include "schedule.h"
#include "secret.h"
#include "sip.h"
#include "twilio_provision.h"
#include "wifi.h"

static const char *TAG = "mqtt_ha";

/* Modest buffers: USB audio + SIP/RTP + HTTP already use sockets/heap.
 * Discovery JSON is built one entity at a time and freed before the next. */
#define MQTT_BUF_SIZE     2048
#define MQTT_OUT_SIZE     2048
#define MQTT_TASK_STACK   6144
#define MQTT_TASK_PRIO    5
#define MQTT_MGR_STACK    4096
#define DISCOVERY_PREFIX  "homeassistant"
/* Cap how many contact slots we advertise (parents rarely wire more; keeps
 * discovery and subscription counts bounded). Used slots still win if fewer. */
#define MQTT_MAX_SLOTS    5

typedef enum {
    MQTT_ST_OFF = 0,
    MQTT_ST_CONNECTING,
    MQTT_ST_ONLINE,
    MQTT_ST_ERROR,
} mqtt_st_t;

static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static char s_device_id[24];   /* kidphone_a1b2c3 */
static char s_base[48];       /* kidphone/a1b2c3 */
static char s_avail[64];      /* kidphone/a1b2c3/status */
static mqtt_st_t s_state = MQTT_ST_OFF;
static bool s_want_run;       /* host configured */
static bool s_discovery_done;
static uint32_t s_cfg_gen_seen;
static char s_last_err[96];
static int s_slots;           /* contact slots last published */
static volatile bool s_reconfigure;

static void set_err(const char *msg)
{
    if (msg && msg[0]) {
        strlcpy(s_last_err, msg, sizeof(s_last_err));
    }
}

static void device_id_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "kidphone_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_base, sizeof(s_base), "kidphone/%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_avail, sizeof(s_avail), "%s/status", s_base);
}

static cJSON *device_json(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(d, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_device_id));
    cJSON_AddStringToObject(d, "name", APP_DEVICE_NAME);
    cJSON_AddStringToObject(d, "mf", "kid_phone");
    cJSON_AddStringToObject(d, "mdl", APP_FIRMWARE_NAME);
    cJSON_AddStringToObject(d, "sw", esp_app_get_description()->version);
    cJSON_AddStringToObject(d, "cu", "http://" APP_HOSTNAME ".local/");
    return d;
}

static void pub(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !topic || !payload) {
        return;
    }
    esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
}

static void pub_discovery(const char *component, const char *object_id, cJSON *cfg)
{
    cJSON_AddItemToObject(cfg, "device", device_json());
    cJSON_AddStringToObject(cfg, "avty_t", s_avail);
    cJSON_AddStringToObject(cfg, "pl_avail", "online");
    cJSON_AddStringToObject(cfg, "pl_not_avail", "offline");
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", DISCOVERY_PREFIX, component, s_device_id, object_id);
    char *s = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    if (s) {
        pub(topic, s, 1, true);
        cJSON_free(s);
    }
    /* Yield so discovery bursts do not starve audio / SIP. */
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void add_unique(cJSON *cfg, const char *suffix)
{
    char uid[48];
    snprintf(uid, sizeof(uid), "%s_%s", s_device_id, suffix);
    cJSON_AddStringToObject(cfg, "uniq_id", uid);
}

static void discover_sensor(const char *object_id, const char *name, const char *stat_t, const char *unit,
                            const char *dev_cla, const char *state_cla)
{
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", name);
    add_unique(cfg, object_id);
    cJSON_AddStringToObject(cfg, "stat_t", stat_t);
    cJSON_AddBoolToObject(cfg, "ret", true);
    if (unit) {
        cJSON_AddStringToObject(cfg, "unit_of_meas", unit);
    }
    if (dev_cla) {
        cJSON_AddStringToObject(cfg, "dev_cla", dev_cla);
    }
    if (state_cla) {
        cJSON_AddStringToObject(cfg, "stat_cla", state_cla);
    }
    pub_discovery("sensor", object_id, cfg);
}

static void discover_text(const char *object_id, const char *name, const char *stat_t, const char *cmd_t, int max)
{
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", name);
    add_unique(cfg, object_id);
    cJSON_AddStringToObject(cfg, "stat_t", stat_t);
    cJSON_AddStringToObject(cfg, "cmd_t", cmd_t);
    cJSON_AddBoolToObject(cfg, "ret", false); /* commands never retained */
    cJSON_AddNumberToObject(cfg, "max", max);
    cJSON_AddStringToObject(cfg, "mode", "text");
    pub_discovery("text", object_id, cfg);
}

static void discover_switch(const char *object_id, const char *name, const char *stat_t, const char *cmd_t)
{
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", name);
    add_unique(cfg, object_id);
    cJSON_AddStringToObject(cfg, "stat_t", stat_t);
    cJSON_AddStringToObject(cfg, "cmd_t", cmd_t);
    cJSON_AddStringToObject(cfg, "pl_on", "ON");
    cJSON_AddStringToObject(cfg, "pl_off", "OFF");
    cJSON_AddBoolToObject(cfg, "ret", false);
    pub_discovery("switch", object_id, cfg);
}

static void discover_number(const char *object_id, const char *name, const char *stat_t, const char *cmd_t, int lo,
                            int hi, const char *unit)
{
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", name);
    add_unique(cfg, object_id);
    cJSON_AddStringToObject(cfg, "stat_t", stat_t);
    cJSON_AddStringToObject(cfg, "cmd_t", cmd_t);
    cJSON_AddNumberToObject(cfg, "min", lo);
    cJSON_AddNumberToObject(cfg, "max", hi);
    cJSON_AddNumberToObject(cfg, "step", 1);
    cJSON_AddStringToObject(cfg, "mode", "box");
    if (unit) {
        cJSON_AddStringToObject(cfg, "unit_of_meas", unit);
    }
    cJSON_AddBoolToObject(cfg, "ret", false);
    pub_discovery("number", object_id, cfg);
}

static void discover_button(const char *object_id, const char *name, const char *cmd_t)
{
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", name);
    add_unique(cfg, object_id);
    cJSON_AddStringToObject(cfg, "cmd_t", cmd_t);
    cJSON_AddStringToObject(cfg, "payload_press", "PRESS");
    /* Explicit: hang-up must never be retained (acceptance: no replay). */
    cJSON_AddBoolToObject(cfg, "ret", false);
    pub_discovery("button", object_id, cfg);
}

static void topic_state(char *out, size_t len, const char *leaf)
{
    snprintf(out, len, "%s/%s", s_base, leaf);
}

static void topic_cmd(char *out, size_t len, const char *leaf)
{
    snprintf(out, len, "%s/%s/set", s_base, leaf);
}

static void load_contacts(cfg_contact_t *ct);
static void handle_call_contact(int idx, bool retain);

/* Empty retained discovery config → HA removes the entity. */
static void undiscover(const char *component, const char *object_id)
{
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", DISCOVERY_PREFIX, component, s_device_id, object_id);
    pub(topic, "", 1, true);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void undiscover_full_config(int slots)
{
    undiscover("number", "max_call_min");
    undiscover("switch", "bedtime_on");
    undiscover("text", "bedtime_start");
    undiscover("text", "bedtime_end");
    undiscover("text", "home_tz");
    undiscover("text", "home_tz_name");
    char oid[40];
    for (int i = 0; i < slots && i < MQTT_MAX_SLOTS; i++) {
        const int n = i + 1;
        snprintf(oid, sizeof(oid), "c%d_name", n);
        undiscover("text", oid);
        snprintf(oid, sizeof(oid), "c%d_number", n);
        undiscover("text", oid);
        snprintf(oid, sizeof(oid), "c%d_sched", n);
        undiscover("switch", oid);
        snprintf(oid, sizeof(oid), "c%d_tz", n);
        undiscover("text", oid);
        snprintf(oid, sizeof(oid), "c%d_tzname", n);
        undiscover("text", oid);
        for (int w = 0; w < CFG_MAX_WINDOWS; w++) {
            snprintf(oid, sizeof(oid), "c%d_w%d", n, w + 1);
            undiscover("text", oid);
        }
    }
}

static uint8_t mqtt_expose_mode(void)
{
    cfg_mqtt_t m;
    config_get_mqtt(&m);
    uint8_t ex = m.expose;
    mbedtls_platform_zeroize(&m, sizeof(m));
    return ex <= CFG_MQTT_EXPOSE_FULL ? ex : CFG_MQTT_EXPOSE_FULL;
}

static void publish_discovery(void)
{
    s_slots = cfg_edit_contact_slots();
    if (s_slots > MQTT_MAX_SLOTS) {
        s_slots = MQTT_MAX_SLOTS;
    }
    const uint8_t expose = mqtt_expose_mode();
    const bool full = expose == CFG_MQTT_EXPOSE_FULL;

    char st[128], cmd[136], name[48], oid[40];

    /* Diagnostics (both modes). */
    topic_state(st, sizeof(st), "firmware");
    discover_sensor("firmware", "Firmware", st, NULL, NULL, NULL);

    topic_state(st, sizeof(st), "rssi");
    discover_sensor("rssi", "WiFi signal", st, "dBm", "signal_strength", "measurement");

    topic_state(st, sizeof(st), "call_state");
    discover_sensor("call_state", "Call state", st, NULL, NULL, NULL);

    topic_state(st, sizeof(st), "last_error");
    discover_sensor("last_error", "Last error", st, NULL, NULL, NULL);

    topic_cmd(cmd, sizeof(cmd), "hangup");
    discover_button("hangup", "Hang up", cmd);

    /* Call buttons: unique_id stays cN_call; friendly name follows the person
     * ("Call Grandma") and is refreshed whenever discovery is republished. */
    cfg_contact_t *ct = calloc(CFG_MAX_CONTACTS, sizeof(cfg_contact_t));
    if (ct) {
        load_contacts(ct);
    }
    for (int i = 0; i < s_slots; i++) {
        const int n = i + 1;
        const char *who = (ct && ct[i].name[0]) ? ct[i].name : NULL;
        snprintf(oid, sizeof(oid), "c%d_call", n);
        if (who) {
            snprintf(name, sizeof(name), "Call %s", who);
        } else {
            snprintf(name, sizeof(name), "Call contact %d", n);
        }
        snprintf(cmd, sizeof(cmd), "%s/contact/%d/call/set", s_base, n);
        discover_button(oid, name, cmd);
    }

    if (full) {
        topic_state(st, sizeof(st), "max_call_min");
        topic_cmd(cmd, sizeof(cmd), "max_call_min");
        discover_number("max_call_min", "Max call length", st, cmd, APP_CALL_MAX_MIN_LO, APP_CALL_MAX_MIN_HI, "min");

        topic_state(st, sizeof(st), "bedtime/enabled");
        topic_cmd(cmd, sizeof(cmd), "bedtime/enabled");
        discover_switch("bedtime_on", "Bedtime", st, cmd);

        topic_state(st, sizeof(st), "bedtime/start");
        topic_cmd(cmd, sizeof(cmd), "bedtime/start");
        discover_text("bedtime_start", "Bedtime starts", st, cmd, 5);

        topic_state(st, sizeof(st), "bedtime/end");
        topic_cmd(cmd, sizeof(cmd), "bedtime/end");
        discover_text("bedtime_end", "Bedtime ends", st, cmd, 5);

        topic_state(st, sizeof(st), "home_tz");
        topic_cmd(cmd, sizeof(cmd), "home_tz");
        discover_text("home_tz", "Home time zone", st, cmd, 47);

        topic_state(st, sizeof(st), "home_tz_name");
        topic_cmd(cmd, sizeof(cmd), "home_tz_name");
        discover_text("home_tz_name", "Home time zone name", st, cmd, 39);

        /* Config entities keep stable Contact N titles (not the person's name). */
        for (int i = 0; i < s_slots; i++) {
            const int n = i + 1;

            snprintf(oid, sizeof(oid), "c%d_name", n);
            snprintf(name, sizeof(name), "Contact %d name", n);
            snprintf(st, sizeof(st), "%s/contact/%d/name", s_base, n);
            snprintf(cmd, sizeof(cmd), "%s/contact/%d/name/set", s_base, n);
            discover_text(oid, name, st, cmd, 31);

            snprintf(oid, sizeof(oid), "c%d_number", n);
            snprintf(name, sizeof(name), "Contact %d number", n);
            snprintf(st, sizeof(st), "%s/contact/%d/number", s_base, n);
            snprintf(cmd, sizeof(cmd), "%s/contact/%d/number/set", s_base, n);
            discover_text(oid, name, st, cmd, 19);

            snprintf(oid, sizeof(oid), "c%d_sched", n);
            snprintf(name, sizeof(name), "Contact %d schedule", n);
            snprintf(st, sizeof(st), "%s/contact/%d/schedule/enabled", s_base, n);
            snprintf(cmd, sizeof(cmd), "%s/contact/%d/schedule/enabled/set", s_base, n);
            discover_switch(oid, name, st, cmd);

            snprintf(oid, sizeof(oid), "c%d_tz", n);
            snprintf(name, sizeof(name), "Contact %d time zone", n);
            snprintf(st, sizeof(st), "%s/contact/%d/schedule/tz", s_base, n);
            snprintf(cmd, sizeof(cmd), "%s/contact/%d/schedule/tz/set", s_base, n);
            discover_text(oid, name, st, cmd, 47);

            snprintf(oid, sizeof(oid), "c%d_tzname", n);
            snprintf(name, sizeof(name), "Contact %d time zone name", n);
            snprintf(st, sizeof(st), "%s/contact/%d/schedule/tz_name", s_base, n);
            snprintf(cmd, sizeof(cmd), "%s/contact/%d/schedule/tz_name/set", s_base, n);
            discover_text(oid, name, st, cmd, 39);

            for (int w = 0; w < CFG_MAX_WINDOWS; w++) {
                snprintf(oid, sizeof(oid), "c%d_w%d", n, w + 1);
                snprintf(name, sizeof(name), "Contact %d window %d", n, w + 1);
                snprintf(st, sizeof(st), "%s/contact/%d/schedule/window/%d", s_base, n, w + 1);
                snprintf(cmd, sizeof(cmd), "%s/contact/%d/schedule/window/%d/set", s_base, n, w + 1);
                discover_text(oid, name, st, cmd, 24);
            }
        }
    } else {
        /* Leaving full mode: remove config entities from HA. */
        undiscover_full_config(MQTT_MAX_SLOTS);
    }

    if (ct) {
        mbedtls_platform_zeroize(ct, sizeof(cfg_contact_t) * CFG_MAX_CONTACTS);
        free(ct);
    }

    s_discovery_done = true;
    ESP_LOGI(TAG, "HA discovery published (%d slots, expose=%s)", s_slots, full ? "full" : "controls");
}

static void fmt_hhmm(char *out, size_t len, unsigned minute)
{
    minute %= 1440u;
    snprintf(out, len, "%02u:%02u", minute / 60u, minute % 60u);
}

static void fmt_window(char *out, size_t len, const cfg_window_t *w)
{
    char days[8];
    for (int d = 0; d < 7; d++) {
        days[d] = (w->days & (1u << d)) ? 'Y' : 'N';
    }
    days[7] = '\0';
    char a[8], b[8];
    fmt_hhmm(a, sizeof(a), w->start_min);
    fmt_hhmm(b, sizeof(b), w->end_min);
    snprintf(out, len, "%s %s-%s", days, a, b);
}

static void publish_states(void)
{
    if (!s_client) {
        return;
    }
    char topic[128], buf[48];
    const bool full = mqtt_expose_mode() == CFG_MQTT_EXPOSE_FULL;

    pub(s_avail, "online", 1, true);

    topic_state(topic, sizeof(topic), "firmware");
    pub(topic, esp_app_get_description()->version, 1, true);

    char ssid[33];
    int rssi = 0;
    if (wifi_current(ssid, sizeof(ssid), &rssi)) {
        snprintf(buf, sizeof(buf), "%d", rssi);
    } else {
        strlcpy(buf, "0", sizeof(buf));
    }
    topic_state(topic, sizeof(topic), "rssi");
    pub(topic, buf, 1, true);

    topic_state(topic, sizeof(topic), "call_state");
    pub(topic, sip_call_state_name(sip_call_state()), 1, true);

    sip_result_t r;
    sip_last_result(&r);
    const char *err = s_last_err[0] ? s_last_err : (r.failed && r.message[0] ? r.message : "");
    topic_state(topic, sizeof(topic), "last_error");
    pub(topic, err, 1, true);

    if (!full) {
        s_cfg_gen_seen = config_generation();
        return;
    }

    snprintf(buf, sizeof(buf), "%u", (unsigned)config_max_call_minutes());
    topic_state(topic, sizeof(topic), "max_call_min");
    pub(topic, buf, 1, true);

    cfg_bedtime_t bed;
    config_get_bedtime(&bed);
    topic_state(topic, sizeof(topic), "bedtime/enabled");
    pub(topic, bed.enabled ? "ON" : "OFF", 1, true);
    fmt_hhmm(buf, sizeof(buf), bed.start_min);
    topic_state(topic, sizeof(topic), "bedtime/start");
    pub(topic, buf, 1, true);
    fmt_hhmm(buf, sizeof(buf), bed.end_min);
    topic_state(topic, sizeof(topic), "bedtime/end");
    pub(topic, buf, 1, true);
    topic_state(topic, sizeof(topic), "home_tz");
    pub(topic, bed.tz, 1, true);
    topic_state(topic, sizeof(topic), "home_tz_name");
    pub(topic, bed.tz_name, 1, true);

    app_cfg_t *c = malloc(sizeof(app_cfg_t));
    if (!c) {
        return;
    }
    config_get(c);
    const int slots = s_slots > 0 ? s_slots : cfg_edit_contact_slots();
    for (int i = 0; i < slots && i < MQTT_MAX_SLOTS; i++) {
        const int n = i + 1;
        const cfg_contact_t *ct = &c->contacts[i];
        snprintf(topic, sizeof(topic), "%s/contact/%d/name", s_base, n);
        pub(topic, ct->name, 1, true);
        snprintf(topic, sizeof(topic), "%s/contact/%d/number", s_base, n);
        pub(topic, ct->number, 1, true);
        snprintf(topic, sizeof(topic), "%s/contact/%d/schedule/enabled", s_base, n);
        pub(topic, ct->sched.enabled ? "ON" : "OFF", 1, true);
        snprintf(topic, sizeof(topic), "%s/contact/%d/schedule/tz", s_base, n);
        pub(topic, ct->sched.tz, 1, true);
        snprintf(topic, sizeof(topic), "%s/contact/%d/schedule/tz_name", s_base, n);
        pub(topic, ct->sched.tz_name, 1, true);
        for (int w = 0; w < CFG_MAX_WINDOWS; w++) {
            snprintf(topic, sizeof(topic), "%s/contact/%d/schedule/window/%d", s_base, n, w + 1);
            if (w < ct->sched.nwin) {
                fmt_window(buf, sizeof(buf), &ct->sched.win[w]);
                pub(topic, buf, 1, true);
            } else {
                pub(topic, "", 1, true);
            }
        }
    }
    config_wipe(c);
    free(c);
    s_cfg_gen_seen = config_generation();
}

static void subscribe_commands(void)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/+/set", s_base);
    esp_mqtt_client_subscribe(s_client, topic, 0);
    snprintf(topic, sizeof(topic), "%s/+/+/set", s_base);
    esp_mqtt_client_subscribe(s_client, topic, 0);
    snprintf(topic, sizeof(topic), "%s/contact/+/+/set", s_base);
    esp_mqtt_client_subscribe(s_client, topic, 0);
    snprintf(topic, sizeof(topic), "%s/contact/+/+/+/set", s_base);
    esp_mqtt_client_subscribe(s_client, topic, 0);
    snprintf(topic, sizeof(topic), "%s/contact/+/schedule/window/+/set", s_base);
    esp_mqtt_client_subscribe(s_client, topic, 0);
}

/* ---- command handlers (shared validation via cfg_edit / config_store) ---- */

static bool parse_hhmm(const char *in, uint16_t *out)
{
    int h, m;
    if (sscanf(in, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    *out = (uint16_t)(h * 60 + m);
    return true;
}

/* "YYYYYYY HH:MM-HH:MM" Sun..Sat; empty clears the window slot when saving. */
static bool parse_window(const char *in, cfg_window_t *out, bool *clear)
{
    while (*in && isspace((unsigned char)*in)) {
        in++;
    }
    if (!in[0]) {
        *clear = true;
        return true;
    }
    *clear = false;
    if (strlen(in) < 15) {
        return false;
    }
    uint8_t days = 0;
    for (int d = 0; d < 7; d++) {
        char c = (char)toupper((unsigned char)in[d]);
        if (c == 'Y' || c == '1') {
            days |= (uint8_t)(1u << d);
        } else if (c != 'N' && c != '0' && c != '-') {
            return false;
        }
    }
    const char *p = in + 7;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    int sh, sm, eh, em;
    if (sscanf(p, "%d:%d-%d:%d", &sh, &sm, &eh, &em) != 4 || sh < 0 || sh > 23 || sm < 0 || sm > 59 || eh < 0 ||
        eh > 23 || em < 0 || em > 59) {
        return false;
    }
    out->days = days;
    out->start_min = (uint16_t)(sh * 60 + sm);
    out->end_min = (uint16_t)(eh * 60 + em);
    return true;
}

static cfg_edit_result_t save_contacts_copy(cfg_contact_t *ct, char *err, size_t err_len)
{
    uint32_t run = 0;
    cfg_edit_result_t r = cfg_edit_save_contacts(ct, &run, err, err_len);
    if (r == CFG_EDIT_OK || r == CFG_EDIT_UPDATING) {
        s_last_err[0] = '\0';
    } else if (err && err[0]) {
        set_err(err);
    }
    return r;
}

static void load_contacts(cfg_contact_t *ct)
{
    app_cfg_t *c = malloc(sizeof(app_cfg_t));
    if (!c) {
        memset(ct, 0, sizeof(cfg_contact_t) * CFG_MAX_CONTACTS);
        return;
    }
    config_get(c);
    memcpy(ct, c->contacts, sizeof(cfg_contact_t) * CFG_MAX_CONTACTS);
    config_wipe(c);
    free(c);
}

/* Same rules as POST /api/call: availability windows apply; bedtime does not
 * (parent testing from HA). Never honor a retained press. */
static void handle_call_contact(int idx, bool retain)
{
    if (retain) {
        ESP_LOGW(TAG, "ignoring retained call command (would replay after broker restart)");
        return;
    }
    cfg_pins_t pins;
    config_get_pins(&pins);
    if (idx < 0 || idx >= CFG_MAX_CONTACTS || idx >= pins.count) {
        set_err("No such button.");
        publish_states();
        return;
    }
    if (!wifi_is_connected() || !config_is_provisioned() || twilio_provision_running()) {
        set_err("The phone is not ready to call yet.");
        publish_states();
        return;
    }
    if (sip_call_state() != CALL_IDLE) {
        set_err("A call is already in progress.");
        publish_states();
        return;
    }
    if (secret_active()) {
        set_err("The phone is recording a secret message right now.");
        publish_states();
        return;
    }
    cfg_contact_t ct;
    if (!config_get_contact(idx, &ct) || !ct.number[0]) {
        set_err("Nobody is set up on this button.");
        publish_states();
        return;
    }
    sched_state_t st;
    schedule_eval(&ct.sched, &st);
    if (!st.available) {
        set_err("They are outside their available hours.");
        publish_states();
        return;
    }
    ESP_LOGI(TAG, "call contact %d from Home Assistant", idx);
    s_last_err[0] = '\0';
    sip_call_contact(idx);
    publish_states();
}

static void handle_contact_cmd(int idx, const char *field, const char *payload)
{
    if (idx < 0 || idx >= CFG_MAX_CONTACTS) {
        return;
    }
    cfg_contact_t *ct = calloc(CFG_MAX_CONTACTS, sizeof(cfg_contact_t));
    if (!ct) {
        set_err("The phone is busy. Try again.");
        return;
    }
    load_contacts(ct);
    char err[160];
    err[0] = '\0';

    if (strcmp(field, "name") == 0) {
        strlcpy(ct[idx].name, payload, sizeof(ct[idx].name));
    } else if (strcmp(field, "number") == 0) {
        char trimmed[32];
        while (*payload && isspace((unsigned char)*payload)) {
            payload++;
        }
        strlcpy(trimmed, payload, sizeof(trimmed));
        size_t n = strlen(trimmed);
        while (n && isspace((unsigned char)trimmed[n - 1])) {
            trimmed[--n] = '\0';
        }
        if (!trimmed[0]) {
            ct[idx].number[0] = '\0';
            memset(&ct[idx].sched, 0, sizeof(ct[idx].sched));
        } else if (!cfg_edit_normalize_number(trimmed, ct[idx].number, sizeof(ct[idx].number))) {
            set_err("Write the phone number in international format (+country code).");
            free(ct);
            return;
        }
        if (ct[idx].number[0] && !ct[idx].name[0]) {
            snprintf(ct[idx].name, sizeof(ct[idx].name), "Contact %d", idx + 1);
        }
    } else if (strcmp(field, "schedule/enabled") == 0) {
        ct[idx].sched.enabled = strcmp(payload, "ON") == 0 || strcmp(payload, "on") == 0 || strcmp(payload, "1") == 0;
    } else if (strcmp(field, "schedule/tz") == 0) {
        strlcpy(ct[idx].sched.tz, payload, sizeof(ct[idx].sched.tz));
    } else if (strcmp(field, "schedule/tz_name") == 0) {
        strlcpy(ct[idx].sched.tz_name, payload, sizeof(ct[idx].sched.tz_name));
    } else if (strncmp(field, "schedule/window/", 16) == 0) {
        int w = atoi(field + 16) - 1;
        if (w < 0 || w >= CFG_MAX_WINDOWS) {
            free(ct);
            return;
        }
        cfg_window_t win = {0};
        bool clear = false;
        if (!parse_window(payload, &win, &clear)) {
            set_err("Window format: YNYYYYN HH:MM-HH:MM (Sun..Sat), or empty to clear.");
            free(ct);
            return;
        }
        if (clear) {
            cfg_window_t tmp[CFG_MAX_WINDOWS];
            int n = 0;
            for (int i = 0; i < ct[idx].sched.nwin; i++) {
                if (i != w) {
                    tmp[n++] = ct[idx].sched.win[i];
                }
            }
            memset(ct[idx].sched.win, 0, sizeof(ct[idx].sched.win));
            memcpy(ct[idx].sched.win, tmp, sizeof(tmp[0]) * (size_t)n);
            ct[idx].sched.nwin = (uint8_t)n;
        } else {
            if (w >= ct[idx].sched.nwin) {
                for (int i = ct[idx].sched.nwin; i < w; i++) {
                    ct[idx].sched.win[i] = (cfg_window_t){0};
                }
                ct[idx].sched.nwin = (uint8_t)(w + 1);
            }
            ct[idx].sched.win[w] = win;
        }
        if (ct[idx].number[0]) {
            const char *why = schedule_validate(&ct[idx].sched);
            if (why) {
                snprintf(err, sizeof(err), "%s", why);
                set_err(err);
                free(ct);
                return;
            }
        }
    } else {
        free(ct);
        return;
    }

    /* Re-validate the edited contact through the shared parser path: build a
     * JSON array and parse, so MQTT and the setup page share one ruleset. */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", ct[i].name);
        cJSON_AddStringToObject(o, "number", ct[i].number);
        static const char *modes[] = {"off", "hold", "dedicated"};
        cJSON_AddStringToObject(o, "secret", ct[i].secret <= CFG_SECRET_DEDICATED ? modes[ct[i].secret] : "off");
        cJSON_AddStringToObject(o, "email", ct[i].email);
        cJSON *js = cJSON_AddObjectToObject(o, "schedule");
        cJSON_AddBoolToObject(js, "enabled", ct[i].sched.enabled);
        cJSON_AddStringToObject(js, "tz", ct[i].sched.tz);
        cJSON_AddStringToObject(js, "tz_name", ct[i].sched.tz_name);
        cJSON *wins = cJSON_AddArrayToObject(js, "windows");
        for (int w = 0; w < ct[i].sched.nwin; w++) {
            cJSON *jw = cJSON_CreateObject();
            cJSON_AddNumberToObject(jw, "days", ct[i].sched.win[w].days);
            cJSON_AddNumberToObject(jw, "start", ct[i].sched.win[w].start_min);
            cJSON_AddNumberToObject(jw, "end", ct[i].sched.win[w].end_min);
            cJSON_AddItemToArray(wins, jw);
        }
        cJSON_AddItemToArray(arr, o);
        /* Stop after the last used slot we care about, but keep trailing
         * empties out of the array only if nothing follows — parser accepts
         * a shorter array (remaining slots stay empty). */
        bool more = false;
        for (int j = i + 1; j < CFG_MAX_CONTACTS; j++) {
            if (ct[j].number[0] || ct[j].secret != CFG_SECRET_OFF || ct[j].name[0]) {
                more = true;
                break;
            }
        }
        if (!more) {
            break;
        }
    }
    cfg_contact_t *parsed = calloc(CFG_MAX_CONTACTS, sizeof(cfg_contact_t));
    if (!parsed) {
        cJSON_Delete(arr);
        free(ct);
        set_err("The phone is busy. Try again.");
        return;
    }
    const char *perr = cfg_edit_parse_contacts(arr, parsed, err, sizeof(err));
    cJSON_Delete(arr);
    free(ct);
    if (perr) {
        set_err(perr);
        free(parsed);
        publish_states(); /* revert HA UI to NVS */
        return;
    }
    save_contacts_copy(parsed, err, sizeof(err));
    free(parsed);
    /* Name may have changed → refresh Call {name} discovery titles. */
    publish_discovery();
    publish_states();
}

static void handle_bedtime_cmd(const char *field, const char *payload)
{
    cfg_bedtime_t b;
    config_get_bedtime(&b);
    char err[96];
    if (strcmp(field, "enabled") == 0) {
        b.enabled = strcmp(payload, "ON") == 0 || strcmp(payload, "on") == 0 || strcmp(payload, "1") == 0;
    } else if (strcmp(field, "start") == 0) {
        if (!parse_hhmm(payload, &b.start_min)) {
            set_err("Bedtime start must be HH:MM.");
            publish_states();
            return;
        }
    } else if (strcmp(field, "end") == 0) {
        if (!parse_hhmm(payload, &b.end_min)) {
            set_err("Bedtime end must be HH:MM.");
            publish_states();
            return;
        }
    } else {
        return;
    }
    if (cfg_edit_set_bedtime(&b, err, sizeof(err)) != ESP_OK) {
        set_err(err);
    } else {
        s_last_err[0] = '\0';
    }
    publish_states();
}

static void on_command(const char *topic, int topic_len, const char *data, int data_len, bool retain)
{
    /* Copy topic/payload: the MQTT client reuses its buffer. */
    char tbuf[160], pbuf[96];
    if (topic_len <= 0 || topic_len >= (int)sizeof(tbuf) || data_len < 0 || data_len >= (int)sizeof(pbuf)) {
        return;
    }
    memcpy(tbuf, topic, (size_t)topic_len);
    tbuf[topic_len] = '\0';
    memcpy(pbuf, data, (size_t)data_len);
    pbuf[data_len] = '\0';

    const char *prefix = s_base;
    size_t plen = strlen(prefix);
    if (strncmp(tbuf, prefix, plen) != 0 || tbuf[plen] != '/') {
        return;
    }
    const char *rest = tbuf + plen + 1;
    /* Expect .../set at the end. */
    size_t rlen = strlen(rest);
    if (rlen < 4 || strcmp(rest + rlen - 4, "/set") != 0) {
        return;
    }
    char path[96];
    strlcpy(path, rest, sizeof(path));
    path[rlen - 4] = '\0'; /* strip /set */

    /* Hang-up: never honor a retained command (broker restart replay). */
    if (strcmp(path, "hangup") == 0) {
        if (retain) {
            ESP_LOGW(TAG, "ignoring retained hang-up (would replay after broker restart)");
            return;
        }
        ESP_LOGI(TAG, "hang up from Home Assistant");
        sip_call_hangup();
        return;
    }

    if (strncmp(path, "contact/", 8) == 0) {
        int n = 0;
        const char *p = path + 8;
        while (isdigit((unsigned char)*p)) {
            n = n * 10 + (*p - '0');
            p++;
        }
        if (n < 1 || *p != '/') {
            return;
        }
        const char *field = p + 1;
        if (strcmp(field, "call") == 0) {
            handle_call_contact(n - 1, retain);
            return;
        }
        /* Contact config only in full expose mode. */
        if (mqtt_expose_mode() != CFG_MQTT_EXPOSE_FULL) {
            return;
        }
        handle_contact_cmd(n - 1, field, pbuf);
        return;
    }

    /* Phone settings (bedtime, call length, TZ) only in full expose mode. */
    if (mqtt_expose_mode() != CFG_MQTT_EXPOSE_FULL) {
        return;
    }

    if (strcmp(path, "max_call_min") == 0) {
        char err[80];
        unsigned m = (unsigned)atoi(pbuf);
        if (cfg_edit_set_max_call_minutes(m, err, sizeof(err)) != ESP_OK) {
            set_err(err);
        } else {
            s_last_err[0] = '\0';
        }
        publish_states();
        return;
    }

    if (strncmp(path, "bedtime/", 8) == 0) {
        handle_bedtime_cmd(path + 8, pbuf);
        return;
    }
    if (strcmp(path, "home_tz") == 0 || strcmp(path, "home_tz_name") == 0) {
        cfg_bedtime_t b;
        config_get_bedtime(&b);
        if (strcmp(path, "home_tz") == 0) {
            strlcpy(b.tz, pbuf, sizeof(b.tz));
        } else {
            strlcpy(b.tz_name, pbuf, sizeof(b.tz_name));
        }
        char err[96];
        if (cfg_edit_set_bedtime(&b, err, sizeof(err)) != ESP_OK) {
            set_err(err);
        } else {
            s_last_err[0] = '\0';
        }
        publish_states();
        return;
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state = MQTT_ST_ONLINE;
        xSemaphoreGive(s_lock);
        subscribe_commands();
        publish_discovery();
        publish_states();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_want_run) {
            s_state = MQTT_ST_CONNECTING;
        }
        s_discovery_done = false;
        xSemaphoreGive(s_lock);
        break;
    case MQTT_EVENT_DATA:
        on_command(e->topic, e->topic_len, e->data, e->data_len, e->retain != 0);
        break;
    case MQTT_EVENT_ERROR:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state = MQTT_ST_ERROR;
        set_err("Broker connection error");
        xSemaphoreGive(s_lock);
        /* Do not log credentials. */
        ESP_LOGW(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

/* URI / login kept for the life of s_client (init may only store pointers
 * until start copies them; never log s_mqtt_pass). */
static char s_mqtt_uri[128];
static char s_mqtt_user[64];
static char s_mqtt_pass[64];

static void wipe_mqtt_creds(void)
{
    mbedtls_platform_zeroize(s_mqtt_uri, sizeof(s_mqtt_uri));
    mbedtls_platform_zeroize(s_mqtt_user, sizeof(s_mqtt_user));
    mbedtls_platform_zeroize(s_mqtt_pass, sizeof(s_mqtt_pass));
}

static void stop_client(void)
{
    if (!s_client) {
        wipe_mqtt_creds();
        return;
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_discovery_done = false;
    wipe_mqtt_creds();
}

static bool start_client(void)
{
    cfg_mqtt_t m;
    config_get_mqtt(&m);
    if (!m.host[0]) {
        mbedtls_platform_zeroize(&m, sizeof(m));
        return false;
    }

    wipe_mqtt_creds();
    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtt://%s:%u", m.host, (unsigned)m.port);
    strlcpy(s_mqtt_user, m.user, sizeof(s_mqtt_user));
    strlcpy(s_mqtt_pass, m.pass, sizeof(s_mqtt_pass));
    mbedtls_platform_zeroize(&m, sizeof(m));

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_mqtt_uri,
        .credentials.client_id = s_device_id,
        .session.last_will.topic = s_avail,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .buffer.size = MQTT_BUF_SIZE,
        .buffer.out_size = MQTT_OUT_SIZE,
        .task.stack_size = MQTT_TASK_STACK,
        .task.priority = MQTT_TASK_PRIO,
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 10000,
    };
    if (s_mqtt_user[0]) {
        cfg.credentials.username = s_mqtt_user;
    }
    if (s_mqtt_pass[0]) {
        cfg.credentials.authentication.password = s_mqtt_pass;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        wipe_mqtt_creds();
        set_err("Could not start MQTT client");
        s_state = MQTT_ST_ERROR;
        return false;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt start failed: %s", esp_err_to_name(err));
        stop_client();
        set_err("Could not connect to broker");
        s_state = MQTT_ST_ERROR;
        return false;
    }
    /* Log host:port only (uri after "mqtt://"), never user/password. */
    ESP_LOGI(TAG, "connecting to %s (client %s)", s_mqtt_uri, s_device_id);
    s_state = MQTT_ST_CONNECTING;
    return true;
}

static void manager_task(void *arg)
{
    (void)arg;
    call_state_t last_call = CALL_IDLE;
    uint32_t last_result = sip_result_seq();
    int tick = 0;
    while (1) {
        if (s_reconfigure) {
            s_reconfigure = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            stop_client();
            s_want_run = config_mqtt_enabled();
            if (!s_want_run) {
                s_state = MQTT_ST_OFF;
                s_last_err[0] = '\0';
                ESP_LOGI(TAG, "MQTT off (no broker host)");
            }
            xSemaphoreGive(s_lock);
        }

        const bool want = config_mqtt_enabled();
        const bool up = wifi_is_connected();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_want_run = want;
        if (!want) {
            if (s_client) {
                stop_client();
            }
            s_state = MQTT_ST_OFF;
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!up) {
            /* Stay quiet while offline; do not tear down mid-call media. */
            if (s_client) {
                stop_client();
            }
            s_state = MQTT_ST_CONNECTING;
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!s_client) {
            xSemaphoreGive(s_lock);
            start_client();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        xSemaphoreGive(s_lock);

        /* Publish live call state / errors without waiting for config. */
        call_state_t cs = sip_call_state();
        uint32_t rs = sip_result_seq();
        if (s_discovery_done && (cs != last_call || rs != last_result)) {
            last_call = cs;
            last_result = rs;
            char topic[80];
            topic_state(topic, sizeof(topic), "call_state");
            pub(topic, sip_call_state_name(cs), 1, true);
            sip_result_t r;
            sip_last_result(&r);
            const char *err = s_last_err[0] ? s_last_err : (r.failed && r.message[0] ? r.message : "");
            topic_state(topic, sizeof(topic), "last_error");
            pub(topic, err, 1, true);
        }

        /* Local UI / cloud saved something: refresh Call {name} titles + states. */
        if (s_discovery_done && config_generation() != s_cfg_gen_seen) {
            publish_discovery();
            publish_states();
        }

        /* RSSI every ~30 s while online. */
        if (s_discovery_done && ++tick >= 30) {
            tick = 0;
            char topic[80], buf[16], ssid[33];
            int rssi = 0;
            if (wifi_current(ssid, sizeof(ssid), &rssi)) {
                snprintf(buf, sizeof(buf), "%d", rssi);
                topic_state(topic, sizeof(topic), "rssi");
                pub(topic, buf, 1, true);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void mqtt_ha_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    device_id_init();
    s_want_run = config_mqtt_enabled();
    xTaskCreatePinnedToCore(manager_task, "mqtt_ha", MQTT_MGR_STACK, NULL, MQTT_TASK_PRIO, NULL, 0);
    ESP_LOGI(TAG, "MQTT manager started (device %s, %s)", s_device_id, s_want_run ? "broker configured" : "off");
}

void mqtt_ha_reconfigure(void)
{
    s_reconfigure = true;
}

bool mqtt_ha_connected(void)
{
    return s_state == MQTT_ST_ONLINE && s_discovery_done;
}

const char *mqtt_ha_state_name(void)
{
    switch (s_state) {
    case MQTT_ST_OFF:
        return "off";
    case MQTT_ST_CONNECTING:
        return "connecting";
    case MQTT_ST_ONLINE:
        return s_discovery_done ? "online" : "connecting";
    case MQTT_ST_ERROR:
        return "error";
    default:
        return "off";
    }
}
