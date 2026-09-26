/* WESR-STOCK 板端固件入口。
   Task 1 的版本只验证工程能编译、能启动；Task 10 起把 UI / 网络 / 按键任务挂上来。 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI("wesr", "boot ok, version %s", "0.1.0");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
