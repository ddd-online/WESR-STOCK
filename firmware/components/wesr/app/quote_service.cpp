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

static wesr_app_cfg_t   *s_cfg;
static wesr_nav_t            s_nav;            /* 当前页/组/索引（Task 16 起由按键驱动） */

void Quote_SetCfg(wesr_app_cfg_t *cfg) { s_cfg = cfg; }

wesr_app_cfg_t *Quote_Cfg(void)
{
    static wesr_app_cfg_t fallback;            /* net_task 还没登记时兜底，别让调用者拿 NULL */
    if (!s_cfg) wesr_cfg_defaults(&fallback);
    return s_cfg ? s_cfg : &fallback;
}

void Quote_CfgChanged(void)
{
    AppState_Lock();
    wesr_nav_set_count(&s_nav, s_cfg->count);          /* 只数变少时把组/索引夹回来 */
    AppState_Status()->stock_count = s_cfg->count;
    AppState_Status()->refresh_sec = s_cfg->refresh_sec;
    /* 作废行情缓存：休市时"按页补一次"的判据就是这两条 valid，清了才会立刻重拉 */
    AppState_Status()->quotes[0].valid = false;
    AppState_Status()->minutes[0].valid = false;
    AppState_Status()->cfg_gen++;
    AppState_Unlock();
    Quote_RefreshUi();
}

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
/* 连接复用：按 host 缓存 esp_http_client 句柄。
   之前每个请求都 init→open→close→cleanup，cleanup 会连 transport 一起销毁，
   于是每轮 5 次全新 TCP+TLS 握手（实测每轮多花数秒、且在 BLE 常驻时更容易撞上内部堆不够）。
   HTTP/1.1 默认持久连接，所以只要不销毁句柄、且上一次响应读完，就能复用同一条连接。
   安全阀：响应被截断（total == cap-1）或没读完整时把该连接标记丢弃，下次重建，
   避免"残留字节被当成下一次的响应头"。 */
#define HTTP_CACHE_N 4

/* 响应缓冲：实测快照 4.2KB、分时 10.9KB，且服务端都是 chunked 无 Content-Length。
   之前每个请求按 cap=32768 malloc、用完 free（实际只用 4~11KB），
   每轮 4~5 次 32KB 的 PSRAM 分配回收既浪费也容易碎。改成一块常驻 16KB。 */
#define HTTP_BUF_SIZE        (16 * 1024)
/* 一次请求最多给多久收完响应（socket 级 timeout_ms 之外的整体上限） */
#define HTTP_RECV_BUDGET_MS  8000

static char *s_http_buf;

static char *http_buf_get(void)
{
    if (!s_http_buf) s_http_buf = (char *)heap_caps_malloc(HTTP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    return s_http_buf;
}

typedef struct {
    char *key;                     /* "host:port"；NULL 表示空槽 */
    esp_http_client_handle_t h;
} http_slot_t;

static http_slot_t s_http_slots[HTTP_CACHE_N];

static void http_cache_drop(http_slot_t *s)
{
    if (s->h) {
        esp_http_client_close(s->h);
        esp_http_client_cleanup(s->h);
        s->h = NULL;
    }
    free(s->key);          /* 句柄被借出时 h 为 NULL，但 key 还在：两件事要分开做 */
    s->key = NULL;
}

static void http_cache_put(const char *key, esp_http_client_handle_t h)
{
    int free_i = -1;
    for (int i = 0; i < HTTP_CACHE_N; i++) {
        if (s_http_slots[i].key && strcmp(s_http_slots[i].key, key) == 0) {
            http_cache_drop(&s_http_slots[i]);      /* 同 host 旧连接换掉 */
            free_i = i;
            break;
        }
        if (!s_http_slots[i].key && free_i < 0) free_i = i;
    }
    if (free_i < 0) {                               /* 满了：淘汰 0 号 */
        http_cache_drop(&s_http_slots[0]);
        free_i = 0;
    }
    s_http_slots[free_i].key = strdup(key);
    s_http_slots[free_i].h = h;
}

static esp_http_client_handle_t http_cache_get(const char *key)
{
    for (int i = 0; i < HTTP_CACHE_N; i++) {
        if (s_http_slots[i].key && strcmp(s_http_slots[i].key, key) == 0) {
            esp_http_client_handle_t h = s_http_slots[i].h;
            s_http_slots[i].h = NULL;               /* 借出：槽位留着 key 方便还回 */
            return h;
        }
    }
    return NULL;
}

/* verify=false 时不校验证书（spec §4.4 的降级策略：公开行情数据、无凭证） */
static char *http_get_ex(const char *url, int *out_len, bool verify)
{
    /* 从 URL 里取 "host:port" 当复用键 */
    char key[80];
    {
        const char *p = strstr(url, "://");
        p = p ? p + 3 : url;
        size_t n = 0;
        while (p[n] && p[n] != '/' && n < sizeof key - 1) { key[n] = p[n]; n++; }
        key[n] = 0;
    }

    /* 只有"已校验成功"的连接才复用；降级连接（不校验）不复用，免得把不安全会话留着 */
    esp_http_client_handle_t c = verify ? http_cache_get(key) : NULL;
    bool reuse = (c != NULL);

    if (!c) {
        esp_http_client_config_t ccfg;
        memset(&ccfg, 0, sizeof ccfg);
        ccfg.url = url;
        ccfg.timeout_ms = 4000;
        ccfg.buffer_size = 2048;       /* 默认 512：11KB 响应要读 22 轮 */
        ccfg.buffer_size_tx = 512;
        ccfg.disable_auto_redirect = true;   /* 3xx 不静默跟随（每次跟随都是白付一次握手） */
        ccfg.keep_alive_enable = true;
        ccfg.keep_alive_idle = 5;
        ccfg.keep_alive_interval = 5;
        ccfg.keep_alive_count = 3;
        if (verify) {
            ccfg.crt_bundle_attach = esp_crt_bundle_attach;
        } else {
            ccfg.skip_cert_common_name_check = true;   /* 降级：不校验 CN */
            /* mbedTLS 4 下只设 skip_cert_common_name_check 不够：
               "No server verification option set ... SSL_SETUP_FAILED"，降级必然失败。
               这里补上 crt_bundle，让降级路径至少能建起会话。 */
            ccfg.crt_bundle_attach = esp_crt_bundle_attach;
        }
        ccfg.user_agent = "Mozilla/5.0";
        c = esp_http_client_init(&ccfg);
        if (!c) return NULL;
    }

    char *buf = NULL;
    esp_err_t oe = ESP_FAIL;

    /* 复用的连接可能已被服务端关掉（keep-alive 到期），失败就丢掉重建再试一次 */
    for (int attempt = 0; attempt < 2; attempt++) {
        oe = esp_http_client_open(c, 0);
        if (oe == ESP_OK) break;
        if (reuse) {
            ESP_LOGI(TAG, "复用连接不可用(%d)，重建: %s", (int)oe, key);
            esp_http_client_cleanup(c);
            c = NULL;
            reuse = false;
            {
                esp_http_client_config_t ccfg;
                memset(&ccfg, 0, sizeof ccfg);
                ccfg.url = url;
                ccfg.timeout_ms = 4000;
                ccfg.buffer_size = 2048;
                ccfg.buffer_size_tx = 512;
                ccfg.disable_auto_redirect = true;
                ccfg.keep_alive_enable = true;
                ccfg.keep_alive_idle = 5;
                ccfg.keep_alive_interval = 5;
                ccfg.keep_alive_count = 3;
                if (verify) ccfg.crt_bundle_attach = esp_crt_bundle_attach;
                else { ccfg.skip_cert_common_name_check = true; ccfg.crt_bundle_attach = esp_crt_bundle_attach; }
                ccfg.user_agent = "Mozilla/5.0";
                c = esp_http_client_init(&ccfg);
                if (!c) return NULL;
            }
            continue;
        }
        break;
    }

    bool keep = false;
    if (oe == ESP_OK) {
        int len = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        buf = http_buf_get();
        if (!buf) {
            ESP_LOGE(TAG, "响应缓冲分配失败");
        } else {
            /* 实测：快照 4.2KB / 分时 10.9KB，且服务端都是 chunked 无 Content-Length
               （fetch_headers 恒返回 -1）。所以不再按 len 每次 malloc，改用一块 16KB 常驻
               缓冲：消掉每轮 5 次 32KB 的 PSRAM 分配回收，也避免 PSRAM 碎片。 */
            const size_t cap = HTTP_BUF_SIZE;
            size_t total = 0, used = 0;
            uint32_t t_start = (uint32_t)(esp_timer_get_time() / 1000);
            bool truncated = false;
            for (;;) {
                if (used >= cap - 1) { truncated = true; break; }
                int r = esp_http_client_read(c, buf + used, (int)(cap - 1 - used));
                if (r > 0) {
                    used += (size_t)r;
                    total = used;
                    continue;
                }
                /* r <= 0 并不等于"读完了"：socket 超时且已读到数据时 IDF 会返回已读字节数，
                   EAGAIN 则返回 0。所以这里要拿 is_complete_data_received 判完整性，
                   给它一段有上限的时间继续收，避免把"半截响应 + HTTP 200"当成功。 */
                bool complete = esp_http_client_is_complete_data_received(c);
                uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000) - t_start;
                if (complete || elapsed > HTTP_RECV_BUDGET_MS) {
                    if (!complete) truncated = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            (void)total;
            buf[used] = 0;
            bool complete = esp_http_client_is_complete_data_received(c);
            ESP_LOGI(TAG, "http %d len=%d read=%u complete=%d trunc=%d reuse=%d",
                     status, len, (unsigned)used, (int)complete, (int)truncated, (int)reuse);
            if (used == 0) {
                buf = NULL;
            } else if (out_len) {
                *out_len = (int)used;
            } else if (len > 0 && (size_t)len != used) {
                ESP_LOGW(TAG, "  Content-Length=%d 但只读到 %u 字节", len, (unsigned)used);
            }
            keep = (status >= 200 && status < 300) && !truncated && complete;
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

    if (keep) {
        http_cache_put(key, c);
    } else {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }

    return buf;
}

/* 先用 crt_bundle 校验；失败再降级重试一次（并打警告） */
static char *http_get(const char *url, int *out_len)
{
    char *b = http_get_ex(url, out_len, true);
    if (!b) {
        /* 明文 http 也会走到这里（第一次连接失败时），日志别说成 TLS 问题 */
        ESP_LOGW(TAG, "%s 首次失败，重建连接重试: %s",
                 (strncmp(url, "https:", 6) == 0) ? "TLS 校验失败" : "open 失败", url);
        b = http_get_ex(url, out_len, false);
    }
    return b;
}

/* ---------- 第 1 页：批量快照 ---------- */
static bool fetch_quotes(void)
{
    /* curl 出来的明文 http 口，实测可用；省掉整个 TLS 握手（第 1 页每轮都拉）。 */
    char url[320] = "http://qt.gtimg.cn/q=";
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
    /* 快照自带成交时间（字段 30 = yyyymmddHHMMSS）。把它记成"手上这份数据是哪天的"，
       状态条那行「休市 · 显示 MM-DD 收盘数据」就准了 —— 以前只有分时接口才写 data_day，
       第 1 页显示的日期可能是上一次拉分时留下的。 */
    AppState_Lock();
    if (q[0].valid && q[0].stamp) {
        AppState_Status()->data_day = (uint32_t)(q[0].stamp / 1000000ULL);
    }
    AppState_Unlock();
    /* 缓冲是常驻的，不释放（见 http_buf_get） */
    ESP_LOGI(TAG, "quotes: %d/%d ok", hits, s_cfg->count);
    return hits > 0;
}

/* ---------- 第 2/3 页：分时（单只一次请求，响应自带快照与市场状态） ---------- */
/* 优先走**明文 HTTP**，省掉 4 次 TLS 握手（本机网络上握手本身就不稳，实测
   mbedtls_ssl_handshake -0x008D；而旧的"降级不校验"路径在 mbedTLS 4 下必然失败，
   每只股票白付一次 5 秒超时）。实测网关行为（PC 上逐个确认过）：
     - http://web.ifzq.gtimg.cn/...      → 302（腾讯 stgw 拦明文，不跟跳转就拿不到数据）
     - http://proxy.finance.qq.com/...   → 200，和 HTTPS 同一份 10895 字节 JSON
   所以明文只对 proxy.finance 有效，最后再用 HTTPS 的 web.ifzq 兜底（某些网络反过来）。 */
static const char *kMinuteUrls[] = {
    "http://proxy.finance.qq.com/ifzqgtimg/appstock/app/minute/query?code=%s",
    "https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=%s",
};

static bool fetch_minute(const char *code, wesr_minute_t *out, wesr_quote_t *qout,
                         bool *closed, uint32_t *day)
{
    char url[192];
    /* 回退判据必须是"解析出数据"。以前只要 body 非空就跳出循环，
       于是 302/错误页这种"有响应但没数据"的情况不会走备用主机。 */
    for (int h = 0; h < 2; h++) {
        snprintf(url, sizeof url, kMinuteUrls[h], code);
        char *body = http_get(url, NULL);
        if (!body) continue;

        bool ok = wesr_parse_minute_json(body, out);
        if (ok) {
            wesr_minute_meta_t meta;
            if (wesr_parse_minute_meta(body, &meta)) {
                if (meta.has_quote && qout) { *qout = meta.quote; qout->valid = true; }
                if (closed) *closed = meta.closed;
                if (day && out->valid) *day = out->day;
            }
            return true;     /* 缓冲常驻，不释放 */
        }
        ESP_LOGW(TAG, "分时解析失败(host %d)，试备用主机: %s", h, code);
    }
    return false;
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
            /* 每只到手就刷：第 2 页 4 只串行发请求，等全完再刷的话画面要空白 2~4 秒；
               分页刷的代价已经被帧差短路兜住了。 */
            Quote_RefreshUi();
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
void Quote_Run(wesr_app_cfg_t *cfg)
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
    bool last_trading = true;      /* 开机那次 bootstrap 已经拉过了，别刚开机又多拉一次 */
    uint32_t last_cfg_gen = 0;

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
        bool bt_now = AppState_Status()->bt_connected;
        /* 换组/换股也要立刻到期：否则休市时下一次网络请求要等一分钟的 idle 重检 */
        uint8_t group = AppState_Status()->group, idx = AppState_Status()->idx;
        uint32_t cfg_gen = AppState_Status()->cfg_gen;
        uint32_t dday = AppState_Status()->data_day;
        /* "休市"由本地时间 + "手上这份数据是哪天的"推出来，**不再看服务端那个 market 标记**：
           那个标记只有分时接口能给，第 1 页只发快照，一旦置上就再也下不来
           （用户实测：9:15 开盘后一上午显示休市、13:00 也没恢复）。 */
        uint32_t today = (uint32_t)(t.year * 10000 + t.month * 100 + t.day);
        bool closed_now = !trading || (dday != 0 && dday != today);
        AppState_Status()->closed = closed_now;
        AppState_Unlock();
        /* 蓝牙连接状态由 NimBLE 回调写，但回调里不能碰 LVGL（主机任务栈只有 4KB），
           所以顶栏图标在这里跟着刷。 */
        if (bt_now != last_bt) { last_bt = bt_now; Quote_RefreshUi(); }
        s_nav.page = page;          /* 内部页状态必须跟着走，否则刷新永远刷错页 */
        s_nav.group = group;
        s_nav.idx = idx;
        if (page != last_page || group != last_group || idx != last_idx ||
            cfg_gen != last_cfg_gen) {
            wesr_sched_page(&sched, page);
            last_page = page; last_group = group; last_idx = idx; last_cfg_gen = cfg_gen;
        }
        /* 跨进一个新的交易时段（早上开盘、下午开盘）：缓存里还挂着上一交易日的收盘数据，
           服务端那句"休市"也还是上一时段留下的 true —— 两者合起来让 need 恒为 false，
           于是一整天都不拉新数据（实测：把 RTC 拨到 09:29:40，09:30 之后一条请求都没有）。
           所以进时段时主动作废缓存，并让调度立刻到期。
           判据用"进入交易时段"而不是"日期变了"：半夜零点作废只会白拉一次昨天的数据。 */
        if (trading && !last_trading) {
            AppState_Lock();
            AppState_Status()->quotes[0].valid = false;
            AppState_Status()->minutes[0].valid = false;
            AppState_Unlock();
            wesr_sched_page(&sched, page);
            ESP_LOGI(TAG, "交易时段开始，作废缓存立刻重拉");
        }
        last_trading = trading;
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
        /* 轮询间隔：交易时段内如果手上还是上一交易日的数据（集合竞价那几分钟就是这样），
           60 秒复核一次；正常交易时段按配置的 refresh_sec。 */
        sched.interval_s = (trading && closed_now)
                         ? 60
                         : (s_cfg->refresh_sec ? s_cfg->refresh_sec : 15);
        /* 第 2/3 页的分时数据服务端是**分钟级**的（实测同一分钟内响应内容不变），
           按 refresh_sec=15s 去拉等于每 4 次里 3 次白拉：那 4 次请求 + LVGL 重绘全白付。
           快照仍是 15s（现价要跟手），分时给它一个至少 30s 的下限。 */
        if (page == 2 || page == 3) {
            if (sched.interval_s < 30) sched.interval_s = 30;
        }

        bool need = false;
        if (closed_now) {
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
        /* 交易时段一律允许拉（服务端的休市标记不能当闸门，它是单向的）；
           时段外只按"当前页缓存是空的/归属不对"补一次。 */
        bool may_fetch = trading || (closed_now && need);
        wesr_fetch_t what = wesr_sched_tick(&sched, now_ms, may_fetch);
        if (what != WESR_FETCH_NONE) {
            bool ok = (what == WESR_FETCH_QUOTES) ? fetch_quotes() : fetch_page_minutes();
            if (ok) {
                wesr_sched_result(&sched, true, now_ms);
            } else if (closed_now && need) {
                /* "按页补一次"不应该推后周期刷新：否则休市切页补到数据后，
                   下一次真正的周期拉取要再等一个 interval（最长 75 秒）。
                   记失败（进退避、更新 offline）但把 next_ms 留在当刻，
                   主循环 200ms 后再判一次 —— 那时 need 已为 false，会正常走周期。 */
                wesr_sched_result(&sched, false, now_ms);
                sched.next_ms = now_ms;
            } else {
                wesr_sched_result(&sched, false, now_ms);
            }
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
