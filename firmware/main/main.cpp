/* WESR-STOCK 板端固件入口。
   Task 10：板级初始化 + UI 骨架（顶栏 / 状态条 / 4 个占位页面）。
   网络、行情、按键、传感器在 Task 11–16 挂上来。 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_init.h"
#include "lvgl_bsp.h"
#include "ui_root.h"

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    Board_Init();
    if (Lvgl_lock(-1)) {
        Ui_Init();
        Lvgl_unlock();
    }
    ESP_LOGI("wesr", "ui up, version %s", "0.1.0");

    /* 无屏验证通道：每隔 20 秒把显存 dump 成 ASCII 到串口（用 idf.py monitor 回读） */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        Board_DumpFb();
    }
}
