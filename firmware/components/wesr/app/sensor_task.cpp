/* 传感器任务：SHTC3 温湿度 + 电池 ADC，每 10 秒更新一次状态并刷新第 1/4 页。
   （官方驱动是 C++ 类，所以这个文件是 .cpp） */
#include "app_state.h"
#include "board_init.h"
#include "ui_page1.h"
#include "ui_page4.h"
#include "ui_root.h"
#include "i2c_equipment.h"
#include "adc_bsp.h"
#include "lvgl_bsp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static Shtc3Port *s_shtc3;

static void sensor_task(void *arg)
{
    s_shtc3 = new Shtc3Port(*Board_I2c());
    float prev_v = 0.0f;
    for (;;) {
        float t = 0, h = 0;
        bool ok = (s_shtc3->Shtc3_ReadTempHumi(&t, &h) == 0);
        float v = Adc_GetBatteryVoltage();
        uint8_t pct = Adc_GetBatteryLevel();

        AppState_Lock();
        wesr_status_t *st = AppState_Status();
        if (ok) { st->temp_c = t; st->humi_pct = h; }
        /* 板子读不到 CHG 引脚，充电状态用"电压在涨"推断（粗略启发式够用） */
        st->charging = (prev_v > 0.0f && v > prev_v + 0.01f);
        if (v > 0.0f) { st->battery_v = v; st->battery_pct = pct; }
        st->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        /* "内存"只报内部 RAM：esp_get_free_heap_size() 把 PSRAM 也算进来了，
           会显示成 7.5MB 那种没意义的数字（设计稿指的是 DRAM 堆） */
        st->heap_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
        st->psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
        float show_t = st->temp_c, show_h = st->humi_pct, show_v = st->battery_v;
        uint8_t show_pct = st->battery_pct;
        AppState_Unlock();
        prev_v = v;

        if (Lvgl_lock(200)) {
            if (ok) Ui_Page1SetEnv(show_t, show_h);
            Ui_Page4Update(AppState_Status());
            Ui_UpdateHeader(AppState_Status());      /* 顶栏电量与电池图标从真实状态来 */
            Lvgl_unlock();
        }
        ESP_LOGI("sensor", "temp=%.1f humi=%.0f batt=%.2fV %u%%", show_t, show_h,
                 (double)show_v, (unsigned)show_pct);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

extern "C" void Sensor_TaskStart(void)
{
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 3, NULL);
}
