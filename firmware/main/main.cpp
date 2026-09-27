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
#include "ui_page2.h"
#include "ui_page3.h"
#include "ui_page4.h"
#include "wesr_logic.h"
#include "app_state.h"
#include "sensor_task.h"
#include "i2c_equipment.h"   /* rtcTimeStruct_t / Rtc_GetTime（port_bsp） */
#include <math.h>

/* 第 2/3 页用的假分时数据（240 点，确定性生成，便于与设计稿对照） */
static wesr_minute_t s_min[4];

static void make_fake_minutes(float prev_close, float pct, uint32_t seed, wesr_minute_t *out)
{
    memset(out, 0, sizeof *out);
    uint32_t s = seed;
    float sum = 0;
    for (int i = 0; i < 240; i++) {
        int m = (i < 120) ? (9 * 60 + 30 + i) : (13 * 60 + (i - 120));
        s = s * 1103515245u + 12345u;
        float rnd = (float)((s >> 16) & 0x7fff) / 32767.0f;
        float t = (float)i / 239.0f;
        float price = prev_close * (1.0f + (pct / 100.0f) * t
                                    + 0.006f * sinf((float)i * 0.13f)
                                    + (rnd - 0.5f) * 0.003f);
        sum += price;
        out->pts[i].hhmm = (uint16_t)((m / 60) * 100 + (m % 60));
        out->pts[i].price = price;
        out->pts[i].avg = sum / (float)(i + 1);
        out->pts[i].vol = 100.0f + rnd * 400.0f;
    }
    out->n = 240;
    out->day = 20260927u;
    out->valid = true;
}

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
        q[i].open = q[i].prev_close * (1.0f + pct[i] / 300.0f);
        q[i].high = last[i] * 1.006f;
        q[i].low = q[i].prev_close * 0.994f;
        q[i].vol_hands = (uint32_t)(10000 + i * 2500);
        q[i].valid = true;
    }
    for (int i = 0; i < 4; i++) make_fake_minutes(q[i].prev_close, pct[i], 7u + (uint32_t)i * 21u, &s_min[i]);

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
        /* 温湿度不在这里假填：sensor_task 起来后会覆盖（Task 14） */
        Ui_Page1Update(q, cfg.stocks, 8);
        Ui_Page2Update(s_min, q, cfg.stocks, 0, 8);
        Ui_Page3Update(&s_min[0], q, cfg.stocks, 0, 8);
        Ui_Page4Update(AppState_Status());
        Lvgl_unlock();
    }
}

/* 第 4 页的假状态（Task 14/15 会换成真实采集值） */
static void fill_fake_status(void)
{
    AppState_Init();
    AppState_Lock();
    wesr_status_t *st = AppState_Status();
    st->wifi_connected = true;
    snprintf(st->ssid, sizeof st->ssid, "HOME-5G");
    snprintf(st->ip, sizeof st->ip, "192.168.1.42");
    st->rssi = -58;
    st->bt_connected = false;
    st->battery_v = 0.0f;          /* 真值由 sensor_task 填（Task 14） */
    st->battery_pct = 0;
    st->charging = false;
    st->temp_c = 0.0f;             /* 同上：真值由 sensor_task 填 */
    st->humi_pct = 0.0f;
    st->sd_mounted = false;
    st->offline = false;
    st->closed = true;
    st->data_day = 20260924u;
    st->refresh_sec = 15;
    st->stock_count = 8;
    st->page = 1;
    st->group = 0;
    st->idx = 0;
    snprintf(st->fw, sizeof st->fw, "v0.1.0");
    st->uptime_s = 0;
    st->heap_kb = 0;
    st->psram_kb = 0;
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
    fill_fake_status();
    feed_fake_page1();        /* 里面会读 AppState 渲染第 4 页，所以必须先填状态 */
    Sensor_TaskStart();
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
        if (now_ms - last_dump_ms >= 15000) {
            last_dump_ms = now_ms;
            /* 调试期轮换页面：1→2→3→4→1…；每第 3 次来一次全分辨率。
               两个周期互质，所以 12 次（≈3 分钟）能把四页各抓一次全分辨率实拍。 */
            dump_no++;
            uint8_t page = (uint8_t)(((dump_no - 1) % 4) + 1);
            /* 状态条演示：轮换时交替显示"休市（细线）"与"未联网（反白）" */
            const char *bar = ((dump_no / 4) % 2) ? "未联网 · 显示最后数据"
                                                  : "休市 · 显示 09-24 收盘数据";
            bool alert = ((dump_no / 4) % 2) != 0;
            if (Lvgl_lock(-1)) {
                Ui_ShowPage(page);
                Ui_StatusBar(bar, alert);
                Lvgl_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(900));      /* 等 LVGL 刷完这一帧再回读显存 */
            /* 调试期一律全分辨率：页面在 1→2→3→4 轮转，所以每次 dump 就是下一页的完整实拍 */
            Board_DumpFbFull();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
