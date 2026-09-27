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
#include "ble_task.h"
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
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#define TAG "quote"

static const wesr_app_cfg_t *s_cfg;
static wesr_nav_t            s_nav;            /* 当前页/组/索引（Task 16 起由按键驱动） */

void Quote_SetPage(uint8_t page)
{
    s_nav.page = page ? page : 1;
    Quote_RefreshUi();
}

/* 把 nav 状态同步到 AppState（第 4 页要显示当前组/索引） */
static void publish_nav(void)
{
    AppState_Lock();
    AppState_Status()->page = s_nav.page;
    AppState_Status()->group = s_nav.group;
    AppState_Status()->idx = s_nav.idx;
    AppState_Unlock();
}

void Quote_NavClick(void)
{
    wesr_nav_click(&s_nav);
    publish_nav();
    if (Lvgl_lock(200)) {
        Ui_ShowPage(s_nav.page);
        Lvgl_unlock();
    }
    Quote_RefreshUi();
    ESP_LOGI(TAG, "KEY click -> page %u", s_nav.page);
}

void Quote_NavDouble(void)
{
    wesr_nav_double(&s_nav);
    publish_nav();
    Quote_RefreshUi();
    ESP_LOGI(TAG, "KEY double -> page %u group %u idx %u", s_nav.page, s_nav.group, s_nav.idx);
}

void Quote_NavSetPairing(bool on)
{
    AppState_Lock();
    AppState_Status()->pairing = on;
    AppState_Status()->pairing_ms = (uint32_t)(esp_timer_get_time() / 1000);
    AppState_Unlock();
    Ble_Enable(on);          /* 配网模式 = 蓝牙广播开（M4） */
    Quote_RefreshUi();
    ESP_LOGI(TAG, "KEY long press -> pairing %s", on ? "on" : "off");
}

/* ---------- HTTP ---------- */
/* verify=false 时不校验证书（spec §4.4 的降级策略：公开行情数据、无凭证） */
static char *http_get_ex(const char *url, int *out_len, bool verify)
{
    esp_http_client_config_t ccfg;
    memset(&ccfg, 0, sizeof ccfg);
    ccfg.url = url;
    ccfg.timeout_ms = 5000;
    if (verify) {
        ccfg.crt_bundle_attach = esp_crt_bundle_attach;
    } else {
        ccfg.skip_cert_common_name_check = true;   /* 降级：不校验证书 */
    }
    ccfg.user_agent = "Mozilla/5.0";

    esp_http_client_handle_t c = esp_http_client_init(&ccfg);
    if (!c) return NULL;
    char *buf = NULL;
    esp_err_t oe = esp_http_client_open(c, 0);
    if (oe == ESP_OK) {
        int len = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        /* chunked/无 Content-Length 时 len 会是 -1，所以不能只按 len 分配 */
        size_t cap = (len > 0 && len < 65536) ? (size_t)len + 1 : 32768;
        buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (buf) {
            size_t total = 0;
            for (;;) {
                int r = esp_http_client_read(c, buf + total, (int)(cap - 1 - total));
                if (r <= 0) break;
                total += (size_t)r;
                if (total >= cap - 1) break;
            }
            buf[total] = 0;
            ESP_LOGI(TAG, "http %d len=%d read=%u", status, len, (unsigned)total);
            if (total == 0) { free(buf); buf = NULL; }
            else if (out_len) *out_len = (int)total;
        }
    } else {
        ESP_LOGW(TAG, "http open failed (%d, verify=%d): %s", (int)oe, (int)verify, url);
        if (verify) {
            /* 诊断：是 DNS 解析不出来，还是解析出来了但连不上 */
            const char *host = strstr(url, "://");
            if (host) {
                char h[64];
                host += 3;
                size_t n = 0;
                while (host[n] && host[n] != '/' && host[n] != ':' && n < sizeof h - 1) { h[n] = host[n]; n++; }
                h[n] = 0;
                struct addrinfo hints;
                memset(&hints, 0, sizeof hints);
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                struct addrinfo *res = NULL;
                int g = getaddrinfo(h, "443", &hints, &res);
                if (g == 0 && res) {
                    struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;
                    ESP_LOGW(TAG, "  dns %s -> %s", h, inet_ntoa(a->sin_addr));
                    freeaddrinfo(res);
                } else {
                    ESP_LOGW(TAG, "  dns %s failed rc=%d", h, g);
                }
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}

/* 先用 crt_bundle 校验；失败再降级重试一次（并打警告） */
static char *http_get(const char *url, int *out_len)
{
    char *b = http_get_ex(url, out_len, true);
    if (!b) {
        ESP_LOGW(TAG, "TLS 校验失败，降级为不校验重试: %s", url);
        b = http_get_ex(url, out_len, false);
    }
    return b;
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
/* 两个主机返回同一份数据；实测某些网络里 web.ifzq 连不上而 proxy.finance 可以，
   所以两个都试一遍（都是腾讯自己的接口）。 */
static const char *kMinuteUrls[] = {
    "https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=%s",
    "https://proxy.finance.qq.com/ifzqgtimg/appstock/app/minute/query?code=%s",
};

static bool fetch_minute(const char *code, wesr_minute_t *out, wesr_quote_t *qout,
                         bool *closed, uint32_t *day)
{
    char url[192];
    char *body = NULL;
    for (int h = 0; h < 2 && !body; h++) {
        snprintf(url, sizeof url, kMinuteUrls[h], code);
        body = http_get(url, NULL);
    }
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
    uint8_t want = (s_nav.page == 3) ? 1 : 4;
    uint8_t base = wesr_nav_minute_key(&s_nav);
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
            AppState_Status()->minutes_key = base;   /* 缓存归属：休市判 need 要用 */
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
    Ui_UpdateHeader(st);          /* WiFi/蓝牙连接状态在顶栏用图标表达 */
    uint32_t day = st->data_day;
    bool closed = st->closed, offline = st->offline;
    AppState_Unlock();
    Lvgl_unlock();

    char b[64];
    AppState_Lock();
    bool pairing = AppState_Status()->pairing;
    AppState_Unlock();
    if (pairing) {
        Ui_StatusBar("配网模式 · 等待小程序", true);
    } else if (closed) {
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
    uint8_t last_page = 0, last_group = 0xFF, last_idx = 0xFF;
    bool last_bt = false;

    /* 开机先补一次：休市/非交易时段本来不发请求，但那样画面上永远是空的；
       设计要的是"休市时显示上一交易日的收盘数据"，所以开机先拉一次快照 + 拉一只分时
       （分时响应里带 market 状态，才知道是不是休市）。 */
    if (cfg->ssid[0]) {
        bool ok1 = fetch_quotes();
        bool closed = false;
        uint32_t day = 0;
        bool ok2 = false;
        AppState_Lock();
        AppState_Status()->minutes_ok = false;
        AppState_Unlock();
        /* 开机把第 1 组四只都拉一遍：这样即便休市，第 2 页也有上一交易日的分时可看 */
        for (int i = 0; i < 4 && i < cfg->count; i++) {
            AppState_Lock();
            wesr_minute_t *m = &AppState_Status()->minutes[i];
            wesr_quote_t  *q = &AppState_Status()->quotes[i];
            AppState_Unlock();
            if (fetch_minute(cfg->stocks[i].code, m, q, &closed, &day)) ok2 = true;
            /* 注意：休市也要把 4 只都拉完 —— 第 2 页四宫格要显示四只的上一交易日分时 */
        }
        AppState_Lock();
        if (ok2) AppState_Status()->minutes_ok = true;
        AppState_Status()->closed = closed;
        AppState_Status()->data_day = day;
        AppState_Status()->minutes_key = 0;      /* 开机只拉了第 1 组（base=0） */
        AppState_Status()->offline = !(ok1 || ok2);
        AppState_Unlock();
        ESP_LOGI(TAG, "bootstrap: quotes=%d minute=%d closed=%d day=%lu", (int)ok1, (int)ok2,
                 (int)closed, (unsigned long)day);
    }

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
        bool bt_now = AppState_Status()->bt_connected;
        /* 换组/换股也要立刻到期：否则休市时下一次网络请求要等一分钟的 idle 重检 */
        uint8_t group = AppState_Status()->group, idx = AppState_Status()->idx;
        AppState_Unlock();
        /* 蓝牙连接状态由 NimBLE 回调写，但回调里不能碰 LVGL（主机任务栈只有 4KB），
           所以顶栏图标在这里跟着刷。 */
        if (bt_now != last_bt) { last_bt = bt_now; Quote_RefreshUi(); }
        s_nav.page = page;          /* 内部页状态必须跟着走，否则刷新永远刷错页 */
        s_nav.group = group;
        s_nav.idx = idx;
        if (page != last_page || group != last_group || idx != last_idx) {
            wesr_sched_page(&sched, page);
            last_page = page; last_group = group; last_idx = idx;
        }
        /* 配网模式 3 分钟无操作自动退出（M4 起这里还会关掉 BLE 广播） */
        AppState_Lock();
        bool pair_timeout = AppState_Status()->pairing &&
            (uint32_t)(now_ms - AppState_Status()->pairing_ms) > 180000u;
        if (pair_timeout) AppState_Status()->pairing = false;
        AppState_Unlock();
        if (pair_timeout) {
            ESP_LOGI(TAG, "pairing timeout");
            Ble_Enable(false);
            Quote_RefreshUi();
        }

        /* 交易时段正常轮询；休市时"按页补一次"——这样切到没数据的那页也能看到上一交易日数据 */
        bool need = false;
        if (srv_closed) {
            AppState_Lock();
            wesr_status_t *st = AppState_Status();
            if (page == 1) need = !st->quotes[0].valid;
            /* 第 2/3 页要连"缓存归属"一起比：只看 valid 的话，切到第 2 组（或第 3 页
               换股）时缓存里还是上一组/上一只的分时，会被当成已有数据 → 根本不补拉，
               于是拿第 1 组的价格去配第 2 组的昨收画图，整条线被 clamp 成直线。 */
            else if (page == 2 || page == 3)
                need = !st->minutes[0].valid || st->minutes_key != wesr_nav_minute_key(&s_nav);
            AppState_Unlock();
        }
        bool may_fetch = (trading && !srv_closed) || (srv_closed && need);
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
