#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "amy_engine.h"

static const char *TAG = "Synthewi";

// C4  E4     G4     C5
static const uint8_t notes[] = {60, 64, 67, 72};

static void audio_loop_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800)); // let startup bleep finish
    for (;;) {
        for (int i = 0; i < 4; i++) {
            ESP_LOGI(TAG, "note_on %d", notes[i]);
            amy_engine_note_on(i, notes[i]); // pad i, note
            vTaskDelay(pdMS_TO_TICKS(500));
            amy_engine_note_off(i);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    amy_engine_init();
    ESP_LOGI(TAG, "AMY ready");
    xTaskCreate(audio_loop_task, "audio_loop", 4096, NULL, 5, NULL);
}
