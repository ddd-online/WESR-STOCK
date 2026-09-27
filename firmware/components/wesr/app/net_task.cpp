/* 网络：WiFi STA 连接 + SNTP 校时（写 PCF85063）。
   官方 app_bsp 里的 espwifi_init() 把 SSID 硬编码成示例值且从不 connect，所以这里自己实现。 */
#include "net_task.h"
#include "app_state.h"
#include "cfg_store.h"
#include "quote_service.h"
#include "i2c_equipment.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define WIFI_OK_BIT BIT0
#define TAG         "net"

static EventGroupHandle_t   s_wifi_ev;
static bool                 s_wifi_inited;
static bool                 s_have_creds;      /* 没有凭据时不要反复 esp_wifi_connect */
static SemaphoreHandle_t    s_radio_mtx;       /* 连接与扫描串行化：BLE 配网任务也会调进来 */

static void radio_take(void)
{
    if (s_radio_mtx) xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
}

static void radio_give(void)
{
    if (s_radio_mtx) xSemaphoreGive(s_radio_mtx);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected reason=%d", d ? d->reason : -1);
        AppState_Lock();
        AppState_Status()->wifi_connected = false;
        AppState_Unlock();
        if (s_have_creds) esp_wifi_connect();    /* 断了就一直重连 */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        AppState_Lock();
        wesr_status_t *st = AppState_Status();
        snprintf(st->ip, sizeof st->ip, IPSTR, IP2STR(&e->ip_info.ip));
        st->wifi_connected = true;
        AppState_Unlock();
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        if (s_wifi_ev) xEventGroupSetBits(s_wifi_ev, WIFI_OK_BIT);
    }
}

static void wifi_init_once(void)
{
    if (s_wifi_inited) return;
    s_wifi_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event, NULL, NULL));
    s_wifi_inited = true;
    s_radio_mtx = xSemaphoreCreateMutex();
}

bool Net_WifiConnect(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_init_once();
    radio_take();
    /* 先把射频起来：没凭据时也要能扫描（配网就看这一步） */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "wifi start failed: %d", (int)e);
        radio_give();
        return false;
    }
    if (!ssid || !ssid[0]) { radio_give(); return false; }

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    strncpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, pass ? pass : "", sizeof wc.sta.password - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* 开放网络也能连 */

    esp_wifi_disconnect();                        /* 清掉上一次残留状态 */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_have_creds = true;
    esp_wifi_connect();                           /* 必须显式发起连接：漏了就一直 NO_AP_FOUND */

    xEventGroupClearBits(s_wifi_ev, WIFI_OK_BIT);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_ev, WIFI_OK_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & WIFI_OK_BIT)) {
        ESP_LOGW(TAG, "wifi connect timeout (%s)", ssid);
        radio_give();
        return false;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        AppState_Lock();
        AppState_Status()->rssi = ap.rssi;
        AppState_Unlock();
    }
    radio_give();
    return true;
}

static void on_time_synced(struct timeval *tv)
{
    struct tm t;
    time_t sec = tv->tv_sec;
    localtime_r(&sec, &t);
    Rtc_SetTime((uint16_t)(t.tm_year + 1900), (uint8_t)(t.tm_mon + 1), (uint8_t)t.tm_mday,
                (uint8_t)t.tm_hour, (uint8_t)t.tm_min, (uint8_t)t.tm_sec);
    ESP_LOGI(TAG, "sntp synced %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900,
             t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

void Net_TimeSync(int timeout_ms)
{
    setenv("TZ", "CST-8", 1);        /* 东八区 */
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(on_time_synced);
    esp_sntp_init();

    for (int i = 0; i < timeout_ms / 500; i++) {
        time_t now = 0;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);
        if (t.tm_year + 1900 >= 2026) return;      /* 时间已经合理了 */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "sntp timeout");
}

static void net_task(void *arg)
{
    static wesr_app_cfg_t cfg;
    AppState_Init();
    wesr_cfg_load(&cfg);                 /* 含 menuconfig 里的 WiFi 凭据 */
    Quote_SetCfg(&cfg);                  /* 早点登记：BLE 配网可能在 Quote_Run 之前就下发配置 */
    AppState_Lock();
    snprintf(AppState_Status()->ssid, sizeof AppState_Status()->ssid, "%s",
             cfg.ssid[0] ? cfg.ssid : "");
    AppState_Status()->stock_count = cfg.count;
    AppState_Status()->refresh_sec = cfg.refresh_sec;
    AppState_Status()->fw[0] = 0;
    snprintf(AppState_Status()->fw, sizeof AppState_Status()->fw, "v0.1.0");
    AppState_Unlock();

    /* 连不上就"扫描记录 + 隔 20 秒重试"：串口里能同时看到"扫得到"与"连不上"的对比 */
    for (int attempt = 1; ; attempt++) {
        if (Net_WifiConnect(cfg.ssid, cfg.pass, 20000)) {
            ESP_LOGI(TAG, "wifi connected (attempt %d)", attempt);
            Net_TimeSync(15000);
            break;
        }
        ESP_LOGW(TAG, "no wifi (attempt %d): ssid=%s", attempt,
                 cfg.ssid[0] ? cfg.ssid : "(empty)");
        Net_WifiScanLog();          /* 扫一遍附近网络，和上面的失败原因对照着看 */
        if (!cfg.ssid[0]) break;    /* 没配网就别转圈了，等 M4 配网 */
        /* 配网可能刚把新凭据写进来、自己连上了：别再用旧凭据跟它打架 */
        AppState_Lock();
        bool up = AppState_Status()->wifi_connected;
        AppState_Unlock();
        if (up) {
            ESP_LOGI(TAG, "wifi up (configured over BLE)");
            Net_TimeSync(15000);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20000));
    }
    Quote_Run(&cfg);                      /* 行情主循环（内部处理断网/退避） */
}

void Net_TaskStart(void)
{
    /* 12KB：HTTPS/TLS 握手很吃栈（6KB 实测直接栈溢出重启） */
    xTaskCreate(net_task, "net", 12288, NULL, 4, NULL);
}

/* 扫描附近的 2.4G 网络（ESP32-S3 只支持 2.4G，所以扫到的都是 2.4G）。
   按信号从强到弱返回，跳过隐藏 SSID（小程序里也选不了）。 */
int Net_WifiScan(wesr_ap_t *out, uint8_t max)
{
    if (!out || !max) return 0;
    wifi_init_once();
    radio_take();
    /* 关键：先关掉"断线自动重连"开关再断——否则事件回调会立刻发起连接，
       扫描永远撞上"正在连接"状态而失败（实测 scan failed）。 */
    bool had_creds = s_have_creds;
    s_have_creds = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_err_t se = esp_wifi_start();          /* 已经起来会报 WIFI_STATE，忽略 */
    (void)se;

    int n_out = 0;
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof sc);
    sc.show_hidden = true;
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 24) n = 24;
        wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(n ? n : 1, sizeof(wifi_ap_record_t));
        if (recs && n && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n; i++) {              /* n ≤ 24，冒泡够了 */
                for (int j = i + 1; j < n; j++) {
                    if (recs[j].rssi > recs[i].rssi) {
                        wifi_ap_record_t t = recs[i];
                        recs[i] = recs[j];
                        recs[j] = t;
                    }
                }
            }
            for (int i = 0; i < n && n_out < max; i++) {
                const char *ssid = (const char *)recs[i].ssid;
                if (!ssid[0]) continue;
                snprintf(out[n_out].ssid, sizeof out[n_out].ssid, "%s", ssid);
                out[n_out].rssi = (int8_t)recs[i].rssi;
                n_out++;
            }
        }
        free(recs);
    } else {
        ESP_LOGW(TAG, "scan failed: %d", (int)e);
    }
    s_have_creds = had_creds;
    if (had_creds) esp_wifi_connect();        /* 扫完把连接接回去 */
    radio_give();
    return n_out;
}

/* 扫描并打到串口：v1 时用来肉眼确认该填哪个 SSID */
void Net_WifiScanLog(void)
{
    static wesr_ap_t aps[20];
    int n = Net_WifiScan(aps, 20);
    if (n == 0) { ESP_LOGW(TAG, "scan: none"); return; }
    ESP_LOGI(TAG, "scan: %d ap(s), ssid / rssi", n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  %2d) %-24s %4d dBm", i + 1, aps[i].ssid, (int)aps[i].rssi);
    }
}
