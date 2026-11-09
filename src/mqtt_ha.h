/*
 * Home Assistant MQTT discovery for the kid phone.
 *
 * Parents edit contacts, schedules, bedtime and call length from HA. The
 * phone keeps NVS as the source of truth (same validation path as the setup
 * page). Calling does not depend on MQTT: if the broker is down, SIP/RTP
 * keep working. Hang-up is a non-retained command button so a broker or HA
 * restart cannot replay it.
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
