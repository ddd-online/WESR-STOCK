/* BLE 配网（spec §10）：NUS 外设 = 小程序写 RX、板子 notify TX。

   三个必须知道的点：
   1. 命令在**独立任务**里执行 —— scan/setWifi 要几秒，在 NimBLE 回调里做会把主机任务
      堵住，手机那边直接掉线。
   2. 蓝牙只在配网模式广播（长按 KEY）；退出就停广播 + 断开。
   3. 分片/重组/JSON 的活全在 logic/ble_proto.c（纯 C，宿主机测过），这里只做搬运。
   ponytail: 控制器只 init 一次，退出配网只停广播（controller 常开约 1~2mA）。要极致省电
   就改成 nimble_port_deinit()，代价是重连状态机得整个重做。 */
#include "ble_task.h"
#include "app_state.h"
#include "cfg_store.h"
#include "net_task.h"
#include "quote_service.h"
#include "wesr_logic.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "i2c_equipment.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define TAG "ble"

#define BLE_NAME      "WESR-STOCK"
#define BLE_NAME_SHORT "WESR"          /* 广播包里只放得下 8 字节名 + 128bit UUID */
#define BLE_OUT_MAX   1024             /* 回包缓冲：最长的 scanResult（12 个 AP）约 500 字节 */

/* Nordic UART Service：6E4000xx-B5A3-F393-E0A9-E50E24DCCA9E（NimBLE 要小端字节序） */
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UUID_RX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UUID_TX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t s_rx_val, s_tx_val;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_addr_type;
static bool     s_on, s_inited, s_synced, s_subscribed;
static QueueHandle_t     s_q;
static wesr_ble_rx_t     s_rx;            /* 4KB：必须 static，任务栈放不下 */
static SemaphoreHandle_t s_tx;            /* 两个任务都会发通知，分片不能被交错 */
static char              s_txbuf[BLE_OUT_MAX + 2];

static void Ble_SendLine(const char *msg);
static void Ble_OnLine(const char *line, char *out, uint16_t cap);
static int  send_status(char *buf, uint16_t cap);

/* ---------------- 发送：整条 → 20 字节一片 notify ---------------- */

static void Ble_SendLine(const char *msg)
{
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "没有订阅者，丢弃回包：%.40s", msg);
        return;
    }
    size_t n = strlen(msg);
    if (n + 1 > sizeof s_txbuf) return;
    /* 拿锁再写 s_txbuf：两个任务（订阅推送 / 命令回包）同时发的话分片会串页 */
    xSemaphoreTake(s_tx, portMAX_DELAY);
    memcpy(s_txbuf, msg, n);
    s_txbuf[n++] = '\n';
    for (uint16_t sent = 0; sent < n;) {
        uint16_t len = wesr_ble_chunk_len((uint16_t)n, sent);
        if (!len) break;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_txbuf + sent, len);
        if (om) {
            int rc = ble_gatts_notify_custom(s_conn, s_tx_val, om);
            if (rc) ESP_LOGW(TAG, "notify rc=%d", rc);
        }
        sent += len;
        vTaskDelay(pdMS_TO_TICKS(2));   /* 20 字节连发手机侧会丢包 */
    }
    ESP_LOGI(TAG, "tx %u bytes", (unsigned)n);
    xSemaphoreGive(s_tx);
}

/* ---------------- 接收：写进来的字节喂给重组器 ---------------- */

static void rx_feed(const uint8_t *d, uint16_t n)
{
    /* GATT 回调只在 NimBLE 主机任务里跑，天然单线程，不用加锁 */
    static char line[WESR_BLE_MSG_MAX + 1];   /* 同上：4KB 不能放栈 */
    wesr_ble_rx_push(&s_rx, d, n);
    uint16_t len;
    while ((len = wesr_ble_rx_take(&s_rx, line, sizeof line)) > 0) {
        (void)len;
        char *cp = strdup(line);
        if (!cp) break;
        BaseType_t qr = xQueueSend(s_q, &cp, 0);
        if (qr != pdTRUE) {                        /* 队列满：丢，不阻塞蓝牙 */
            free(cp);
            break;
        }
    }
}

static int gatt_cb(uint16_t conn_handle, uint16_t attr_handle,
                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_rx_val)
        return BLE_ATT_ERR_UNLIKELY;           /* TX 只 notify，不给读 */
    uint8_t buf[64];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0 || len == 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    rx_feed(buf, len);
    return 0;
}

/* 写成具名数组而不是 C 的复合字面量：这个文件是 C++，而且 IDF 默认
   -Werror=missing-field-initializers，每个成员都得列全。 */
static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &UUID_RX.u,
        .access_cb = gatt_cb,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle = &s_rx_val,
        .cpfd = nullptr,
    },
    {
        .uuid = &UUID_TX.u,
        .access_cb = gatt_cb,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_tx_val,
        .cpfd = nullptr,
    },
    {},
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .includes = nullptr,
        .characteristics = s_chrs,
    },
    {},
};

/* ---------------- 广播 / 连接 ---------------- */

static void set_bt(bool connected)
{
    /* 只写状态，别在这里碰 LVGL：gap_cb 跑在 NimBLE 主机任务里，
       那个任务栈只有 4KB、而且被堵住就直接表现为"GATT 服务发现超时"。
       顶栏图标由 quote_service 的主循环发现变化后刷新。 */
    AppState_Lock();
    AppState_Status()->bt_connected = connected;
    AppState_Unlock();
}

static void advertise(void);

static int gap_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
            set_bt(true);
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected: reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        set_bt(false);
        if (s_on) advertise();                 /* 配网模式里掉线要能重连 */
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_on) advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        s_subscribed = event->subscribe.cur_notify != 0;
        ESP_LOGI(TAG, "subscribe notify=%d", (int)s_subscribed);
        /* 订阅一开就推一条 status：客户端不用先发 hello 就知道链路通不通，
           也顺手躲掉"写完 CCCD 立刻写命令"那种抢跑（实测偶发，命令被丢掉不回包）。 */
        if (s_subscribed) {
            static char buf[BLE_OUT_MAX];      /* 主机任务 4KB 栈，放不下 4KB 局部数组 */
            if (send_status(buf, sizeof buf) > 0) Ble_SendLine(buf);
        }
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    if (!ble_hs_synced()) {                    /* 主机没跟控制器同步：现在广播一定失败 */
        ESP_LOGW(TAG, "advertise skipped: host not synced");
        return;
    }
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof f);
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)BLE_NAME_SHORT;
    f.name_len = strlen(BLE_NAME_SHORT);
    f.name_is_complete = 0;                    /* 完整名字放 scan rsp */
    f.uuids128 = &UUID_SVC;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { ESP_LOGE(TAG, "adv fields rc=%d", rc); return; }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof rsp);
    rsp.name = (uint8_t *)BLE_NAME;
    rsp.name_len = strlen(BLE_NAME);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);          /* 失败也只是名字不全，不致命 */

    struct ble_gap_adv_params ap;
    memset(&ap, 0, sizeof ap);
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_cb, NULL);
    ESP_LOGI(TAG, "advertising as %s (rc=%d)", BLE_NAME, rc);
}

/* ---------------- 命令执行任务 ---------------- */

static void ble_worker(void *arg)
{
    (void)arg;
    char *line = NULL;
    static char out[BLE_OUT_MAX];              /* 一个 1KB 缓冲给所有命令回包用（单任务，不用锁） */
    for (;;) {
        if (xQueueReceive(s_q, &line, portMAX_DELAY) != pdTRUE || !line) continue;
        Ble_OnLine(line, out, sizeof out);
        free(line);
    }
}

/* 命令表见 spec §10。解析/校验在 ble_proto.c（宿主机测过），这里只做"执行 + 回包"。 */
static void reply(char *out, uint16_t cap, int n)
{
    if (n > 0) Ble_SendLine(out);
    else       ESP_LOGW(TAG, "回包装不下（%d 字节）", n);
}

static void Ble_OnLine(const char *line, char *out, uint16_t cap)
{
    ESP_LOGI(TAG, "rx %s", line);

    if (wesr_ble_cmd_is(line, "hello")) {
        reply(out, cap, send_status(out, cap));
        return;
    }

    if (wesr_ble_cmd_is(line, "scan")) {
        static wesr_ap_t aps[12];                 /* spec §10：最多 12 个；static 省栈 */
        int n = Net_WifiScan(aps, 12);
        reply(out, cap, wesr_ble_fmt_scan(out, cap, aps, (uint8_t)n));
        return;
    }

    if (wesr_ble_cmd_is(line, "getCfg")) {
        reply(out, cap, wesr_ble_fmt_cfg(out, cap, Quote_Cfg()));
        return;
    }

    if (wesr_ble_cmd_is(line, "setWifi")) {
        /* 用 memcpy 而不是 snprintf("%s") 落盘：长度自己校验过了，省得编译器
           为"可能截断"报 -Werror=format-truncation */
        char ssid[96], pass[96];
        if (!wesr_ble_get_str(line, "ssid", ssid, sizeof ssid) || !ssid[0]) {
            reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_SSID, "ssid empty or >32"));
            return;
        }
        size_t sl = strlen(ssid);
        if (sl > 32) {
            reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_SSID, "ssid empty or >32"));
            return;
        }
        if (!wesr_ble_get_str(line, "pass", pass, sizeof pass)) pass[0] = 0;  /* 开放网络 */
        size_t pl = strlen(pass);
        if (pl > 64) {
            reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_PASS, "pass >64"));
            return;
        }
        wesr_app_cfg_t *c = Quote_Cfg();
        memcpy(c->ssid, ssid, sl + 1);
        memcpy(c->pass, pass, pl + 1);
        if (wesr_cfg_save(c) != ESP_OK) {
            reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_NVS, "save failed"));
            return;
        }
        AppState_Lock();
        memcpy(AppState_Status()->ssid, ssid, sl + 1);
        AppState_Unlock();
        ESP_LOGI(TAG, "setWifi %s, connecting…", ssid);
        if (!Net_WifiConnect(ssid, pass, 20000)) {
            reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_WIFI, "connect failed"));
            return;
        }
        Quote_CfgChanged();                       /* 新网通了：立刻重拉行情 */
        reply(out, cap, wesr_ble_fmt_ack(out, cap, "setWifi"));
        return;
    }

    if (wesr_ble_cmd_is(line, "setStocks")) {
        static wesr_stock_cfg_t list[WESR_MAX_STOCKS];
        const char *err = NULL;
        int n = wesr_ble_parse_items(line, list, WESR_MAX_STOCKS, &err);
        if (n < 0) {
            reply(out, cap, wesr_ble_fmt_err(out, cap, err ? err : WESR_BLE_E_ARG, "bad items"));
            return;
        }
        wesr_app_cfg_t *c = Quote_Cfg();
        memset(c->stocks, 0, sizeof c->stocks);   /* 只数变少时别留旧名字 */
        memcpy(c->stocks, list, (size_t)n * sizeof list[0]);
        c->count = (uint8_t)n;
        if (wesr_cfg_save(c) != ESP_OK) {
            reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_NVS, "save failed"));
            return;
        }
        Quote_CfgChanged();
        ESP_LOGI(TAG, "setStocks %d 只", n);
        reply(out, cap, wesr_ble_fmt_ack(out, cap, "setStocks"));
        return;
    }

    if (wesr_ble_cmd_is(line, "setInterval")) {
        long sec = 0;
        if (!wesr_ble_get_int(line, "sec", &sec) ||
            (sec != 5 && sec != 10 && sec != 15 && sec != 30 && sec != 60)) {
            reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_ARG, "sec must be 5/10/15/30/60"));
            return;
        }
        wesr_app_cfg_t *c = Quote_Cfg();
        c->refresh_sec = (uint16_t)sec;
        if (wesr_cfg_save(c) != ESP_OK) {
            reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_NVS, "save failed"));
            return;
        }
        Quote_CfgChanged();
        reply(out, cap, wesr_ble_fmt_ack(out, cap, "setInterval"));
        return;
    }

    if (wesr_ble_cmd_is(line, "timeSync")) {
        long unix_s = 0;
        if (!wesr_ble_get_int(line, "unix", &unix_s) || unix_s < 1577836800L) {  /* < 2020-01-01 */
            reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_ARG, "unix out of range"));
            return;
        }
        setenv("TZ", "CST-8", 1);                 /* 和 net_task 一致：板子存本地时间 */
        tzset();
        time_t t = (time_t)unix_s;
        struct tm tm;
        localtime_r(&t, &tm);
        Rtc_SetTime((uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1),
                    (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min,
                    (uint8_t)tm.tm_sec);
        struct timeval tv;
        memset(&tv, 0, sizeof tv);
        tv.tv_sec = t;
        settimeofday(&tv, NULL);                  /* 系统时间也跟上，别只写 RTC */
        ESP_LOGI(TAG, "timeSync -> %04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        reply(out, cap, wesr_ble_fmt_ack(out, cap, "timeSync"));
        return;
    }

    if (wesr_ble_cmd_is(line, "exit")) {
        reply(out, cap, wesr_ble_fmt_ack(out, cap, "exit"));
        vTaskDelay(pdMS_TO_TICKS(300));           /* 先让 ack 发出去，再拆广播和连接 */
        Quote_NavSetPairing(false);
        return;
    }

    reply(out, cap, wesr_ble_fmt_err(out, cap, WESR_BLE_E_ARG, "unknown cmd"));
}

static int send_status(char *buf, uint16_t cap)
{
    AppState_Lock();
    wesr_status_t *st = AppState_Status();
    wesr_ble_status_t v = {
        .fw = st->fw,
        .mac = st->mac,
        .ssid = st->ssid,
        .ip = st->ip,
        .wifi_state = st->wifi_connected ? "connected" : "idle",
        .batt_pct = st->battery_pct,
        .batt_mv = (int)(st->battery_v * 1000.0f),
        .rssi = st->rssi,
        .cfg_count = st->stock_count,
        .page = st->page,
        .group = st->group,
        .idx = st->idx,
    };
    int n = wesr_ble_fmt_status(buf, cap, &v);
    AppState_Unlock();
    return n;
}

/* ---------------- 起停 ---------------- */

static void on_sync(void)
{
    s_synced = true;
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_addr_type) != 0) ESP_LOGE(TAG, "no addr");
    if (s_on) advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset: %d", reason); }

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void Ble_Enable(bool on)
{
    if (on == s_on) return;
    s_on = on;

    if (!on) {
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, 0x13 /* 用户主动断开 */);
        vTaskDelay(pdMS_TO_TICKS(120));              /* 先让断开事件走完再拆栈 */
        if (s_inited) {
            /* 必须把栈整个拆掉，不能只停广播：这块板子的内存是硬约束 ——
               BLE 常驻要 50KB 内部 RAM，而 HTTPS 握手 + 整屏刷新的 SPI DMA
               凑巧都要 15~40KB 连续内部内存。实测常驻时：
                 quote: http open failed (28674 ESP_ERR_HTTP_CONNECT)
                 spicommon_dma_setup_priv_buffer: Failed to allocate priv TX buffer
               → ESP_ERROR_CHECK → abort → 重启。退出配网就该把 50KB 还回去。 */
            nimble_port_stop();
            vTaskDelay(pdMS_TO_TICKS(50));
            nimble_port_deinit();
            s_inited = false;
            s_synced = false;
            s_subscribed = false;
            s_conn = BLE_HS_CONN_HANDLE_NONE;
        }
        ESP_LOGI(TAG, "stack down, internal free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return;
    }

    if (!s_inited) {
        if (nimble_port_init() != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init failed");
            s_on = false;
            return;
        }
        ble_svc_gap_init();
        ble_svc_gatt_init();
        ble_svc_gap_device_name_set(BLE_NAME);
        ble_gatts_count_cfg(s_svcs);
        ble_gatts_add_svcs(s_svcs);
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        /* 蓝牙地址：小程序用它区分是哪台板子（固件这边没有别的唯一标识） */
        uint8_t m[6];
        if (esp_read_mac(m, ESP_MAC_BT) == ESP_OK) {
            AppState_Lock();
            snprintf(AppState_Status()->mac, sizeof AppState_Status()->mac,
                     "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
            AppState_Unlock();
        }
        nimble_port_freertos_init(host_task);
        s_inited = true;
        ESP_LOGI(TAG, "stack up, waiting for sync");
    } else if (s_synced) {
        advertise();                            /* 之前只是停过广播 */
    }
}

void Ble_Start(void)
{
    if (s_q) return;                            /* 幂等 */
    s_q = xQueueCreate(4, sizeof(char *));
    s_tx = xSemaphoreCreateMutex();
    wesr_ble_rx_reset(&s_rx);
    BaseType_t tc = xTaskCreate(ble_worker, "ble", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "worker task create=%d, internal free=%u largest=%u", (int)tc,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
