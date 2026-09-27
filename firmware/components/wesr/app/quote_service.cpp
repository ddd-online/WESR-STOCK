/* 行情服务：腾讯公开接口 → 解析 → 写 AppState → 刷 UI。
   实测要点（见 spec §4）：快照可以一次批量 8 只；分时接口不支持批量、必须走 HTTPS；
   分时响应里自带快照数组（qt.<code>）和 market 状态，所以第 2/3 页不用再拉一次快照。 */
#include "quote_service.h"
#include "app_state.h"
#include "ui_page1.h"
#include "ui_page2.h"
#include "ui_page3.h"
#include "ui_page4.h"
#include "ui_root.h"
#include "i2c_equipment.h"
#include "lvgl_bsp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define TAG "quote"

static const wesr_app_cfg_t *s_cfg;
static wesr_nav_t            s_nav;            /* 当前页/组/索引（Task 16 起由按键驱动） */

/* ---------- HTTP ---------- */
static char *http_get(const char *url, int *out_len)
{
    esp_http_client_config_t ccfg;
    memset(&ccfg, 0, sizeof ccfg);
    ccfg.url = url;
    ccfg.timeout_ms = 5000;
    ccfg.crt_bundle_attach = esp_crt_bundle_attach;
    ccfg.user_agent = "Mozilla/5.0";

    esp_http_client_handle_t c = esp_http_client_init(&ccfg);
    if (!c) return NULL;
    char *buf = NULL;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int len = esp_http_client_fetch_headers(c);
        if (len > 0 && len < 65536) {
            buf = (char *)heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM);
            if (buf) {
                int got = esp_http_client_read(c, buf, len);
                if (got <= 0) { free(buf); buf = NULL; }
                else { buf[got] = 0; if (out_len) *out_len = got; }
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}

/* ---------- 第 1 页：批量快照 ---------- */
static bool fetch_quotes(void)
{
    char url[320] = "https://qt.gtimg.cn/q=";
    for (int i = 0; i < s_cfg->count; i++) {
        if (i) strncat(url, ",", sizeof url - strlen(url) - 1);
        strncat(url, s_cfg->stocks[i].code, sizeof url - strlen(url) - 1);
    }
    int len = 0;
    char *body = http_get(url, &len);
    if (!body) return false;

    int hits = 0;
    AppState_Lock();
    wesr_quote_t *q = AppState_Status()->quotes;
    for (int i = 0; i < s_cfg->count; i++) q[i].valid = false;
    char *line = body;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        for (int i = 0; i < s_cfg->count; i++) {
            char tag[24];
            snprintf(tag, sizeof tag, "v_%s=", s_cfg->stocks[i].code);
            if (strncmp(line, tag, strlen(tag)) == 0) {
                if (wesr_parse_quote_line(line, &q[i])) hits++;
                break;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    AppState_Unlock();
    free(body);
    ESP_LOGI(TAG, "quotes: %d/%d ok", hits, s_cfg->count);
    return hits > 0;
}

/* ---------- 第 2/3 页：分时（单只一次请求，响应自带快照与市场状态） ---------- */
static bool fetch_minute(const char *code, wesr_minute_t *out, wesr_quote_t *qout,
                         bool *closed, uint32_t *day)
{
    char url[192];
    snprintf(url, sizeof url, "https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=%s", code);
    int len = 0;
    char *body = http_get(url, &len);
    if (!body) return false;

    bool ok = wesr_parse_minute_json(body, out);
    wesr_minute_meta_t meta;
    if (wesr_parse_minute_meta(body, &meta)) {
        if (meta.has_quote && qout) { *qout = meta.quote; qout->valid = true; }
        if (closed) *closed = meta.closed;
        if (day && out->valid) *day = out->day;
    }
    free(body);
    return ok;
}

static bool fetch_page_minutes(void)
{
    uint8_t start = wesr_nav_group_start(&s_nav);
    uint8_t want = (s_nav.page == 3) ? 1 : 4;
    uint8_t base = (s_nav.page == 3) ? s_nav.idx : start;
    bool all_ok = true;
    for (int i = 0; i < want; i++) {
        uint8_t idx = (uint8_t)(base + i);
        if (idx >= s_cfg->count) break;
        AppState_Lock();
        wesr_minute_t   *m = &AppState_Status()->minutes[i];
        wesr_quote_t    *q = &AppState_Status()->quotes[idx];
        bool closed = AppState_Status()->closed;
        uint32_t day = AppState_Status()->data_day;
        AppState_Unlock();

        bool ok = fetch_minute(s_cfg->stocks[idx].code, m, q, &closed, &day);
        if (ok) {
            AppState_Lock();
            AppState_Status()->closed = closed;
            AppState_Status()->data_day = day;
            AppState_Status()->minutes_ok = true;
            AppState_Unlock();
        } else {
            all_ok = false;
        }
    }
    ESP_LOGI(TAG, "minutes: page=%u base=%u ok=%d", s_nav.page, base, (int)all_ok);
    return all_ok;
}

/* ---------- UI 刷新 ---------- */
void Quote_RefreshUi(void)
{
    if (!Lvgl_lock(300)) return;
    AppState_Lock();
    wesr_status_t *st = AppState_Status();
    uint8_t start = wesr_nav_group_start(&s_nav);
    if (s_nav.page == 1) Ui_Page1Update(st->quotes, s_cfg->stocks, s_cfg->count);
    else if (s_nav.page == 2) Ui_Page2Update(st->minutes, st->quotes, s_cfg->stocks, start, s_cfg->count);
    else if (s_nav.page == 3) {
        uint8_t i = (s_nav.idx < 4) ? s_nav.idx : 0;   /* 第 3 页只拉了 1 只，缓存在 [0] */
        Ui_Page3Update(&st->minutes[0], st->quotes, s_cfg->stocks, s_nav.idx, s_cfg->count);
        (void)i;
    }
    Ui_Page4Update(st);
    uint32_t day = st->data_day;
    bool closed = st->closed, offline = st->offline;
    AppState_Unlock();
    Lvgl_unlock();

    char b[64];
    if (closed) {
        snprintf(b, sizeof b, "休市 · 显示 %02u-%02u 收盘数据",
                 (unsigned)((day / 100) % 100), (unsigned)(day % 100));
        Ui_StatusBar(b, false);
    } else if (offline) {
        Ui_StatusBar("未联网 · 显示最后数据", true);
    } else if (!s_cfg->ssid[0]) {
        Ui_StatusBar("未配网 · 长按 KEY 用小程序配网", true);
    } else {
        Ui_StatusBar("", false);
    }
}

/* ---------- 主循环 ---------- */
void Quote_Run(const wesr_app_cfg_t *cfg)
{
    s_cfg = cfg;
    wesr_nav_init(&s_nav, cfg->count);
    wesr_nav_click(&s_nav);                    /* 调试期从第 1 页开始（Task 16 交给按键） */
    wesr_nav_click(&s_nav);
    wesr_nav_click(&s_nav);
    wesr_nav_click(&s_nav);

    wesr_sched_t sched;
    wesr_sched_init(&sched, cfg->refresh_sec);
    uint8_t last_page = 0;

    Quote_RefreshUi();     /* 先按当前状态画一次（未配网/未联网状态条、清掉占位文字） */
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        /* 本地时间（RTC）→ 交易时段判断 */
        rtcTimeStruct_t t;
        Rtc_GetTime(&t);
        uint16_t hhmm = (uint16_t)(t.hour * 100 + t.minute);
        bool trading = (t.year >= 2026) && wesr_in_trading(hhmm);

        AppState_Lock();
        uint8_t page = AppState_Status()->page ? AppState_Status()->page : 1;
        bool srv_closed = AppState_Status()->closed;
        AppState_Unlock();
        if (page != last_page) { wesr_sched_page(&sched, page); last_page = page; }

        bool may_fetch = trading && !srv_closed;
        wesr_fetch_t what = wesr_sched_tick(&sched, now_ms, may_fetch);
        if (what != WESR_FETCH_NONE) {
            bool ok = (what == WESR_FETCH_QUOTES) ? fetch_quotes() : fetch_page_minutes();
            wesr_sched_result(&sched, ok, now_ms);
            AppState_Lock();
            if (ok) {
                AppState_Status()->last_ok_ms = now_ms;
                AppState_Status()->offline = false;
            } else {
                AppState_Status()->today_fail++;
                AppState_Status()->offline = sched.offline;
            }
            AppState_Unlock();
            Quote_RefreshUi();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
