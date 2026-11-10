# Bell-Towne cloud sync

Outbound HTTPS sync with `https://app.bell-towne.com` (configurable base URL in
NVS `cloud_url`). The phone always initiates TLS with certificate validation
on. No inbound ports.

Contract details: Bell-Towne app `docs/kid-phone.md`.

## Pairing

1. Setup page → **Bell-Towne cloud** → **Show pairing code**
2. Phone `POST /api/kid-phone/v1/pair/begin` with `{code, mac, firmware_version}`
3. Owner enters the code in the Bell-Towne Kid Phone app
4. Sync task polls `POST /api/kid-phone/v1/pair/complete` until
   `status: paired`, then stores in NVS: `cloud_uuid`, `cloud_dtok` (device
   token), optional `cloud_name` / updated `cloud_url`

Expired (`410`) or already-completed (`409`) codes clear the pending code so
the owner can start again.

**Unlink** clears the device token. SIP calling and local/HA config keep
working. A `401` on any authenticated call also clears credentials (revoked).

There is **no** access/refresh split and **no** `/token/refresh`.

## Sync loop

Every ~30 s (faster while pairing):

1. Push queued call-log events + health + contact roster (`POST …/events`)
2. Pull pending commands (`GET …/commands`) — primarily `set_contact_schedule`
3. Apply via `schedule_validate` / `config_set_contacts` — same path as the
   local UI
4. Ack with `{ok: true}` or `{ok: false, error: "…"}` (no
   `reported_config_generation`)

`config_revision` on command/event responses is informational only.

### `POST /api/kid-phone/v1/events` body

| Field | Purpose |
|-------|---------|
| `events` | Call-log rows (may be `[]`) |
| `status` | Health snapshot |
| `contacts` | Roster of used slots for the manage UI |

**Status:** `firmware_version`, `wifi_ssid`, `wifi_rssi`, `uptime_seconds`,
`last_sync_error`.

**Contacts** (only used slots — name and/or number and/or secret button):

| Field | Rule |
|-------|------|
| `slot` | `0 … CFG_MAX_CONTACTS-1` |
| `name` | Display name from NVS (may be `""`) |
| `number` | Full E.164 from NVS. Omitted when the slot has no number |

**Never sent:** Wi‑Fi passwords, Twilio Auth Token, SIP password, restricted
API secrets, secret-message emails. Schedules are not pushed here
(availability still flows cloud → phone via `set_contact_schedule`).

Call logs use a bounded ring buffer (`APP_CLOUD_LOG_CAPACITY`); oldest events
drop if the app is unreachable. Events are idempotent by `event_id` per device.

## Product model

- One Bell-Towne **owner** per device (co-owners deferred).
- Contact slot 0–23 is the join key to a callee in the app; roster sync
  includes names and phone numbers for the manage UI.
- Callee availability edits become pending `set_contact_schedule` commands.
