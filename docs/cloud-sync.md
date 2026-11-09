# Bell-Towne cloud sync

Outbound HTTPS sync with `https://app.bell-towne.com` (configurable base URL in
NVS `cloud_url`). The phone always initiates TLS with certificate validation
on. No inbound ports.

## Pairing

1. Setup page → **Bell-Towne cloud** → **Show pairing code**
2. Phone `POST /api/cloud/pair` → registers code with the app
3. Manager enters the code in the Bell-Towne Kid Phone app
4. Sync task polls until tokens are stored in NVS (`cloud_uuid`, `cloud_atok`,
   `cloud_rtok`)

**Unlink** clears tokens. SIP calling and local/HA config keep working.

## Sync loop

Every ~30 s (faster while pairing):

1. Push queued call-log events + health snapshot
2. Pull pending commands (`set_contact_schedule`, `set_bedtime`)
3. Apply via `schedule_validate` / `config_set_contacts` (or bedtime) — same
   path as the local UI
4. Ack the command; MQTT HA republishes when `config_generation()` changes

Call logs use a bounded ring buffer (`APP_CLOUD_LOG_CAPACITY`); oldest events
drop if the app is unreachable.

## Secrets

Never sent to the cloud: Wi‑Fi passwords, Twilio Auth Token, SIP password,
restricted API key secret, SMTP/MQTT passwords.
