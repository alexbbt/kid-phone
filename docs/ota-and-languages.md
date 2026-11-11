# OTA firmware and downloadable languages

This layout keeps the firmware image small (English prompts embedded) so two
OTA slots fit on the 16 MB flash, and puts other languages on a LittleFS
`storage` partition that survives updates.

## Partition table (`partitions.csv`)

| Name | Size | Role |
|------|------|------|
| `nvs` / `phy_init` | 24 KB + 4 KB | Settings / RF cal (unchanged) |
| `otadata` | 8 KB | Which OTA slot boots |
| `ota_0` / `ota_1` | 4 MB each | Dual firmware slots |
| `storage` | ~7.9 MB | LittleFS — language packs |

The app binary is ~2.4 MB with English-only embeds, so 4 MB slots leave headroom.

**Migration:** phones that still have the old single `factory` table need **one
USB flash** (or browser installer) with this partition table. After that,
`POST /api/ota` can update over WiFi.

## Language packs

- English: embedded in firmware (always available offline).
- Other languages: downloaded to `/storage/lang/<code>/` via
  `POST /api/lang-pack` `{ "lang": "ca" }`.
- Pack index URL: `APP_LANG_PACK_BASE_DEFAULT` or NVS override
  (`lang_pack_base` on the setup page status). Default points at
  `lang-packs/<lang>/manifest.json` on this branch.

Generate manifests (URLs point at existing `src/prompts/*` and `twilio/*`):

```sh
scripts/build_lang_packs.py
# or:
scripts/build_lang_packs.py --base https://raw.githubusercontent.com/<org>/<repo>/<branch>
```

Commit `lang-packs/*/manifest.json` so phones can fetch them. Clip bytes stay
in `src/prompts` / `twilio` (no duplicate blobs in git).

After installing a non-English pack, run **Run setup again** once so Twilio
gets that language’s `no-answer` asset (or extend provisioning to upload
wavs found under `/storage/lang`).

## Firmware OTA

Setup page **Firmware update (WiFi)** or:

```http
POST /api/ota
{"url":"https://example.com/firmware.bin"}
```

Uses `esp_https_ota` into the inactive slot, validates, reboots. URL must be
`https://`. Host a release `.bin` (app image only, not a merged
bootloader+partition flash).

## Contributing upstream

Suggested split for a PR to AllanBinder/kid-phone:

1. Partition table + LittleFS mount + English-only embeds (size win).
2. Language pack download + setup UI.
3. HTTPS OTA endpoint + setup UI.
4. `scripts/build_lang_packs.py` + `docs/ota-and-languages.md`.
5. Host manifests (GitHub raw or release) and set `APP_LANG_PACK_BASE_DEFAULT`.
