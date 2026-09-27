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
#include "ui_page1.h"
#include "wesr_logic.h"
#include "i2c_equipment.h"   /* rtcTimeStruct_t / Rtc_GetTime（port_bsp） */

/* Task 11：第 1 页先用固定假数据把版式验出来（真实行情在 Task 15 接）。
   假的报价刻意与设计稿一致，便于逐像素对照。 */
static void feed_fake_page1(void)
{
    wesr_app_cfg_t cfg;
    wesr_cfg_defaults(&cfg);

    static const float last[8] = { 1682.50f, 372.16f, 284.66f, 41.03f,
                                   52.88f, 4012.88f, 128.40f, 28.50f };
    static const float pct[8]  = { 0.84f, -1.24f, 2.10f, 0.12f,
                                   -0.45f, -0.36f, 0.62f, -0.22f };
    wesr_quote_t q[8];
    for (int i = 0; i < 8; i++) {
        memset(&q[i], 0, sizeof q[i]);
        q[i].last = last[i];
        q[i].prev_close = last[i] / (1.0f + pct[i] / 100.0f);
        q[i].chg_pct = pct[i];
        q[i].valid = true;
    }

    /* 时钟：RTC 没设过（年份 < 2020）就先显示固定值 */
    rtcTimeStruct_t t;
    Rtc_GetTime(&t);
    char hhmm[8], date[24];
    if (t.year >= 2020) {
        snprintf(hhmm, sizeof hhmm, "%02d:%02d", t.hour, t.minute);
        snprintf(date, sizeof date, "%d月%d日", t.month, t.day);
    } else {
        snprintf(hhmm, sizeof hhmm, "09:41");
        snprintf(date, sizeof date, "9月27日");
    }

    if (Lvgl_lock(-1)) {
        Ui_Page1SetClock(hhmm, date);
        Ui_Page1SetEnv(24.6f, 58.0f);      /* 温湿度假数据，Task 14 换真传感器 */
        Ui_Page1Update(q, cfg.stocks, 8);
        Lvgl_unlock();
    }
}

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
    feed_fake_page1();
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
