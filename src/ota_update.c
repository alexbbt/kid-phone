#include "ota_update.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

static void progress(ota_progress_fn fn, void *user, const char *line)
{
    if (fn) {
        fn(line, user);
    }
}

bool ota_update_from_url(const char *url, ota_progress_fn cb, void *user)
{
    if (!url || !url[0]) {
        progress(cb, user, "Missing firmware URL.");
        return false;
    }
    if (strncmp(url, "https://", 8) != 0) {
        progress(cb, user, "Firmware URL must be https://");
        return false;
    }
    progress(cb, user, "Starting firmware download…");

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 60000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin: %s", esp_err_to_name(err));
        progress(cb, user, "Could not start the firmware download.");
        return false;
    }

    int last_pct = -1;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int total = esp_https_ota_get_image_size(handle);
        int read = esp_https_ota_get_image_len_read(handle);
        if (total > 0) {
            int pct = (int)((read * 100LL) / total);
            if (pct != last_pct && pct % 5 == 0) {
                last_pct = pct;
                char line[48];
                snprintf(line, sizeof(line), "Downloading firmware… %d%%", pct);
                progress(cb, user, line);
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota perform: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        progress(cb, user, "Firmware download failed.");
        return false;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        progress(cb, user, "Firmware download was incomplete.");
        return false;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota finish: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            progress(cb, user, "Firmware image failed validation.");
        } else {
            progress(cb, user, "Could not finalize the firmware update.");
        }
        return false;
    }

    progress(cb, user, "Update installed. Rebooting…");
    ESP_LOGI(TAG, "OTA ok, rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true; /* not reached */
}
