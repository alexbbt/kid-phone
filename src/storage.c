#include "storage.h"

#include "esp_log.h"
#include "esp_littlefs.h"

static const char *TAG = "storage";
static bool s_ready;

void storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        s_ready = false;
        return;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "storage mounted: %u used / %u total", (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "storage mounted");
    }
    s_ready = true;
}

bool storage_ready(void)
{
    return s_ready;
}
