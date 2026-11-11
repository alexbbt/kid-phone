/*
 * Home Assistant MQTT discovery for the kid phone.
 *
 * Parents edit contacts, schedules, bedtime and call length from HA, flip
 * Calls allowed (master outbound enable, both expose modes), and can place
 * a test call to each contact (same rules as the setup page Call button:
 * needs Calls allowed on, respects availability windows, skips bedtime).
 * The phone keeps NVS as the source of truth. Calling does not depend on
 * MQTT for the handset: if the broker is down, SIP/RTP keep working. Call and hang-up are
 * non-retained command buttons so a broker or HA restart cannot replay them.
 *
 * Broker host/user/password live in NVS (setup page). Empty host = off.
 * Twilio Auth Token, SIP password and API key secrets are never published.
 */
#pragma once

#include <stdbool.h>

/* Start the MQTT manager task (after wifi_start). Non-blocking. */
void mqtt_ha_start(void);

/* Broker settings changed on the setup page: reconnect with the new ones. */
void mqtt_ha_reconfigure(void);

/* True while connected to the broker and discovery has been published. */
bool mqtt_ha_connected(void);

/* Short status for /api/status: "off", "connecting", "online", "error". */
const char *mqtt_ha_state_name(void);
