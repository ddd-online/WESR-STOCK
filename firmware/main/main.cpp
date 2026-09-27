/* WESR-STOCK 板端固件入口。
   Task 15 起：真实数据链路（WiFi → SNTP 校时 → 按页拉行情 → 刷 UI）；
   Task 16 会加上 KEY 切页换股，并关掉这里的调试 dump。 */
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
#include "ui_page4.h"
#include "wesr_logic.h"
#include "app_state.h"
#include "sensor_task.h"
#include "net_task.h"
#include "quote_service.h"
#include "input_task.h"
#include "i2c_equipment.h"   /* rtcTimeStruct_t / Rtc_GetTime（port_bsp） */

/* 时钟：每 30 秒读一次 RTC（秒级刷新对 1-bit 屏没意义，还费电） */
static void clock_task(void *arg)
{
    static const char *wd[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    for (;;) {
        rtcTimeStruct_t t;
        Rtc_GetTime(&t);
        char hhmm[8], date[32];
        snprintf(hhmm, sizeof hhmm, "%02d:%02d", t.hour, t.minute);
        if (t.year >= 2020) {
            uint8_t w = wesr_weekday(t.year, t.month, t.day);
            snprintf(date, sizeof date, "%d月%d日 %s", t.month, t.day, wd[w]);
        } else {
            snprintf(date, sizeof date, "--月--日");
        }
        if (Lvgl_lock(200)) {
            Ui_Page1SetClock(hhmm, date);
            Lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* 初始状态：只填与网络无关的字段；电压/温湿度由 sensor_task、行情由 quote_service 填 */
static void init_status(void)
{
    wesr_app_cfg_t cfg;
    wesr_cfg_defaults(&cfg);
    AppState_Init();
    AppState_Lock();
    wesr_status_t *st = AppState_Status();
    snprintf(st->fw, sizeof st->fw, "v0.1.0");
    st->refresh_sec = cfg.refresh_sec;
    st->stock_count = cfg.count;
    st->page = 1;
    st->idx = 0;
    st->group = 0;
    st->offline = true;          /* 还没拉到行情前先按"未联网"显示，拿到数据会翻过来 */
    st->closed = false;
    AppState_Unlock();
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
    init_status();
    if (Lvgl_lock(200)) {
        Ui_Page4Update(AppState_Status());
        Lvgl_unlock();
    }
    Sensor_TaskStart();
    xTaskCreate(clock_task, "clock", 3072, NULL, 2, NULL);
    Input_TaskStart();       /* KEY：单击切页 / 双击换股 / 长按配网 */
    Net_TaskStart();
    ESP_LOGI("wesr", "ui up, version %s", "0.1.0");

    /* 主循环：正常情况下什么都不做（各任务自管）。
       开发期可开 CONFIG_WESR_DEBUG_FB_DUMP 定期回读显存，用于无相机自检画面。 */
    for (;;) {
#if CONFIG_WESR_DEBUG_FB_DUMP
        static uint32_t last_dump_ms;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_dump_ms >= 15000) {
            last_dump_ms = now_ms;
            Board_DumpFbFull();
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
