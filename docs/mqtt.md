# Home Assistant MQTT

The kid phone can join Home Assistant over MQTT discovery. Parents edit
contacts, schedules, bedtime and call length from HA. Calling does **not**
depend on MQTT: if the broker is down, SIP and USB audio keep working.

Twilio Auth Token, SIP password and the restricted API key secret stay on the
phone. They are never published on MQTT.

## Setup (broker on the phone)

1. Open the phone’s setup page (`http://kidphone.local/`).
2. Open **Home Assistant (MQTT)**.
3. Enter the broker **host** (hostname or IPv4 of Home Assistant / Mosquitto),
   usually port **1883**, and optional username/password.
4. Press **Save**. Leave host empty to turn MQTT off.
5. In Home Assistant, enable MQTT discovery if it is not already on. The
   device **Kid Phone** should appear within a minute.

The password is stored in NVS and never shown again on the setup page (same
pattern as the secret-message SMTP password).

## Device identity

Stable id from the WiFi STA MAC (last three bytes):

| Item | Value |
|------|--------|
| Device id | `kidphone_<aabbcc>` |
| Base topic | `kidphone/<aabbcc>/` |
| Availability (LWT) | `kidphone/<aabbcc>/status` → `online` / `offline` (retained) |
| Discovery prefix | `homeassistant` |

On every MQTT reconnect the phone re-publishes discovery and state.

## Topic contract

Replace `<id>` with the three-byte hex (e.g. `a1b2c3`). State topics are
retained. **Command** topics (`…/set`) are never retained by the phone; the
hang-up button discovery also sets `retain: false` so HA does not store a
press. Retained hang-up payloads are ignored if they somehow appear.

### Status (read-only)

| State topic | Meaning |
|-------------|---------|
| `kidphone/<id>/status` | `online` / `offline` (LWT) |
| `kidphone/<id>/firmware` | Firmware version string |
| `kidphone/<id>/rssi` | WiFi RSSI (dBm) |
| `kidphone/<id>/call_state` | `idle`, `calling`, `ringing`, `in_call`, `ending` |
| `kidphone/<id>/last_error` | Last call / save error message, or empty |

### Phone settings

| State | Command | Notes |
|-------|---------|--------|
| `…/max_call_min` | `…/max_call_min/set` | 1–120 minutes |
| `…/bedtime/enabled` | `…/bedtime/enabled/set` | `ON` / `OFF` |
| `…/bedtime/start` | `…/bedtime/start/set` | `HH:MM` home time |
| `…/bedtime/end` | `…/bedtime/end/set` | `HH:MM` home time |
| `…/home_tz` | `…/home_tz/set` | POSIX TZ rule |
| `…/home_tz_name` | `…/home_tz_name/set` | IANA name (display) |

### Contacts (slots 1–N, N ≤ 5 used slots)

For each contact `N`:

| State | Command |
|-------|---------|
| `…/contact/N/name` | `…/contact/N/name/set` |
| `…/contact/N/number` | `…/contact/N/number/set` (E.164, `+…`; number changes sync Twilio ALLOWED like the setup page) |
| `…/contact/N/schedule/enabled` | `…/set` with `ON`/`OFF` |
| `…/contact/N/schedule/tz` | POSIX TZ |
| `…/contact/N/schedule/tz_name` | IANA name |
| `…/contact/N/schedule/window/W` | W = 1..4 |

**Window text format** (no raw JSON required):

```text
YNYYYYN 17:00-20:00
```

Seven characters, Sunday→Saturday: `Y`/`1` = day on, `N`/`0`/`-` = off, then
space, then `HH:MM-HH:MM`. Empty string clears that window. Example weekdays
5–8 pm: `NYYYYYN 17:00-20:00`.

### Hang up

| Command | Payload |
|---------|---------|
| `kidphone/<id>/hangup/set` | `PRESS` |

Non-retained. Ignored if the MQTT message has the retain flag set.

## Dashboard

A starter Lovelace dashboard that uses the text/switch/number entities (not
raw JSON) lives at [`ha/kidphone_dashboard.yaml`](../ha/kidphone_dashboard.yaml).
Replace `AABBCC` with your device’s three-byte id (shown in HA entity unique
ids as `kidphone_aabbcc_…`).

## Acceptance checks

- Edit a name, number, schedule window, bedtime or max call length in HA →
  same value on the setup page after refresh; survives reboot.
- Restart Mosquitto or Home Assistant → device rediscovers and comes online.
- Start a call, stop the broker → call continues; hang-up from the handset
  still works.
- After a broker restart with no one pressing Hang up, no spurious hang-up
  (command is not retained).
