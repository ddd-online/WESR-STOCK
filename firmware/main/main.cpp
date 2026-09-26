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

    /* 无屏验证通道：
       - 每 20 秒半分辨率 dump 一次（看版式）
       - 串口收到 'd' 时全分辨率 dump 一次（看文字/斜纹细节） */
    uint32_t last_dump_ms = 0;
    uint32_t dump_no = 0;
    while (true) {
        int c = Board_PollKey();
        if (c == 'd' || c == 'D') Board_DumpFbFull();
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_dump_ms >= 20000) {
            last_dump_ms = now_ms;
            /* 每第 6 次（≈2 分钟）来一次全分辨率，用于核对文字与斜纹细节 */
            if (++dump_no % 6 == 0) Board_DumpFbFull();
            else                    Board_DumpFb();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
