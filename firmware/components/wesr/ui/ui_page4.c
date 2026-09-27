/* 第 4 页 · 系统状态：双栏「标签 - 值」，排障面板（不连串口也能看出哪里不对） */
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_page4.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

enum {
    R_NET_T, R_WIFI, R_SIG, R_IP, R_BT,
    R_PWR_T, R_BAT, R_CHG, R_TH, R_SD,
    R_MKT_T, R_SRC, R_INT, R_UPD, R_FAIL, R_POOL, R_GRP, R_P3,
    R_SYS_T, R_FW, R_UP, R_HEAP, R_PSRAM,
    R_COUNT
};

typedef struct { const char *text; uint8_t col; int y; bool is_title; } row_t;

static const row_t k_rows[R_COUNT] = {
    [R_NET_T] = { "网络",        0,   0, true },
    [R_WIFI]  = { "WiFi",        0,  16, false },
    [R_SIG]   = { "信号",        0,  32, false },
    [R_IP]    = { "IP",          0,  48, false },
    [R_BT]    = { "蓝牙",        0,  64, false },
    [R_PWR_T] = { "电源 / 环境", 0,  86, true },
    [R_BAT]   = { "电池",        0, 102, false },
    [R_CHG]   = { "充电",        0, 118, false },
    [R_TH]    = { "温湿度",      0, 134, false },
    [R_SD]    = { "SD 卡",       0, 150, false },
    [R_MKT_T] = { "行情",        1,   0, true },
    [R_SRC]   = { "数据源",      1,  16, false },
    [R_INT]   = { "刷新",        1,  32, false },
    [R_UPD]   = { "更新",        1,  48, false },
    [R_FAIL]  = { "今日失败",    1,  64, false },
    [R_POOL]  = { "股票池",      1,  80, false },
    [R_GRP]   = { "第 2 页组",   1,  96, false },
    [R_P3]    = { "第 3 页",     1, 112, false },
    [R_SYS_T] = { "系统",        1, 134, true },
    [R_FW]    = { "固件",        1, 150, false },
    [R_UP]    = { "运行时长",    1, 166, false },
    [R_HEAP]  = { "内存",        1, 182, false },
    [R_PSRAM] = { "PSRAM",       1, 198, false },
};

static lv_obj_t *s_val[R_COUNT];

lv_obj_t *Ui_Page4Create(void)
{
    lv_obj_t *page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(page, UI_W, UI_H - UI_HEADER_H);
    lv_obj_set_pos(page, 0, UI_HEADER_H);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    const int col_x[2] = { 6, 206 };
    const int col_w = 186;
    for (int i = 0; i < R_COUNT; i++) {
        lv_obj_t *l = lv_label_create(page);
        lv_obj_set_style_text_font(l, &font_cn12, 0);
        lv_obj_set_style_text_color(l, lv_color_black(), 0);
        lv_label_set_text(l, k_rows[i].text);
        lv_obj_set_pos(l, col_x[k_rows[i].col], k_rows[i].y);
        if (k_rows[i].is_title) {
            lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
            continue;
        }
        lv_obj_t *v = lv_label_create(page);
        lv_obj_set_style_text_font(v, &font_cn12, 0);
        lv_obj_set_style_text_color(v, lv_color_black(), 0);
        lv_label_set_text(v, "—");
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT,
                     -(UI_W - (col_x[k_rows[i].col] + col_w)), k_rows[i].y);
        s_val[i] = v;
    }
    return page;
}

static void setv(int idx, const char *fmt, ...)
{
    if (!s_val[idx]) return;
    char b[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    lv_label_set_text(s_val[idx], b);
}

void Ui_Page4Update(const wesr_status_t *st)
{
    if (!st) return;
    setv(R_WIFI, "%s", st->wifi_connected ? st->ssid : "未连接");
    setv(R_SIG, "%d dBm", st->rssi);
    setv(R_IP, "%s", st->ip[0] ? st->ip : "—");
    setv(R_BT, "%s", st->bt_connected ? "已连接·小程序" : "未连接");
    setv(R_BAT, "%.2fV %u%%", st->battery_v, (unsigned)st->battery_pct);
    setv(R_CHG, "%s", st->charging ? "充电中" : "未充电");
    setv(R_TH, "%.1f°C %.0f%%", st->temp_c, st->humi_pct);
    setv(R_SD, "%s", st->sd_mounted ? "已挂载" : "未插入");
    setv(R_SRC, "直连·腾讯");
    setv(R_INT, "%us", (unsigned)st->refresh_sec);
    setv(R_UPD, "%s", st->offline ? "已断网" : "刚刚");
    setv(R_FAIL, "%u 次", (unsigned)st->today_fail);
    setv(R_POOL, "%u 只", (unsigned)st->stock_count);
    setv(R_GRP, "%u-%u", (unsigned)(st->group ? 5 : 1),
                        (unsigned)(st->group ? 8 : 4));
    setv(R_P3, "%u/%u", (unsigned)(st->idx + 1), (unsigned)st->stock_count);
    setv(R_FW, "%s", st->fw);
    setv(R_UP, "%u天 %02u:%02u", st->uptime_s / 86400,
         (st->uptime_s % 86400) / 3600, (st->uptime_s % 3600) / 60);
    setv(R_HEAP, "%uK", (unsigned)st->heap_kb);
    setv(R_PSRAM, "%uK 空闲", (unsigned)st->psram_kb);
}
