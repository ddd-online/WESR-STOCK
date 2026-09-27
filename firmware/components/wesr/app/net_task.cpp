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
#include <string.h>
#include <time.h>

#define WIFI_OK_BIT BIT0
#define TAG         "net"

static EventGroupHandle_t   s_wifi_ev;
static bool                 s_wifi_inited;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        AppState_Lock();
        AppState_Status()->wifi_connected = false;
        AppState_Unlock();
        esp_wifi_connect();                      /* 断了就一直重连 */
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
}

bool Net_WifiConnect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ssid || !ssid[0]) return false;
    wifi_init_once();

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    strncpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, pass ? pass : "", sizeof wc.sta.password - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* 开放网络也能连 */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupClearBits(s_wifi_ev, WIFI_OK_BIT);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_ev, WIFI_OK_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & WIFI_OK_BIT)) {
        ESP_LOGW(TAG, "wifi connect timeout (%s)", ssid);
        return false;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        AppState_Lock();
        AppState_Status()->rssi = ap.rssi;
        AppState_Unlock();
    }
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
    AppState_Lock();
    snprintf(AppState_Status()->ssid, sizeof AppState_Status()->ssid, "%s",
             cfg.ssid[0] ? cfg.ssid : "");
    AppState_Status()->stock_count = cfg.count;
    AppState_Status()->refresh_sec = cfg.refresh_sec;
    AppState_Status()->fw[0] = 0;
    snprintf(AppState_Status()->fw, sizeof AppState_Status()->fw, "v0.1.0");
    AppState_Unlock();

    if (!Net_WifiConnect(cfg.ssid, cfg.pass, 20000)) {
        ESP_LOGW(TAG, "no wifi: ssid=%s", cfg.ssid[0] ? cfg.ssid : "(empty)");
    } else {
        Net_TimeSync(15000);
    }
    Quote_Run(&cfg);                      /* 行情主循环（内部处理断网/退避） */
}

void Net_TaskStart(void)
{
    xTaskCreate(net_task, "net", 6144, NULL, 4, NULL);
}
