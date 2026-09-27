/* 顶栏 + 状态条 + 页面容器。Task 10 只做骨架（页面里放占位标签），
   真正的第 1–4 页在 Task 11–13 里替换。 */
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_page1.h"
#include "ui_page2.h"
#include "ui_page3.h"
#include "ui_page4.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_dots[4];
static lv_obj_t *s_bank[2];
static lv_obj_t *s_batt_label;
static lv_obj_t *s_batt_icon;
static lv_obj_t *s_wifi_x;      /* WiFi 不可用时跟在图标后的小叉 */
static lv_obj_t *s_bt_x;
static lv_obj_t *s_bar;          /* 状态条 */
static lv_obj_t *s_pages[4];
static uint8_t   s_page_now = 1;

void Ui_StatusBar(const char *text, bool alert)
{
    if (!s_bar) return;
    if (!text || !text[0]) { lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_bar, text);
    lv_obj_set_style_bg_opa(s_bar, alert ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_black(), 0);
    lv_obj_set_style_text_color(s_bar, alert ? lv_color_white() : lv_color_black(), 0);
}

static void build_header(void)
{
    lv_obj_t *hd = lv_obj_create(lv_scr_act());
    lv_obj_set_size(hd, UI_W, UI_HEADER_H);
    lv_obj_set_pos(hd, 0, 0);
    lv_obj_set_style_radius(hd, 0, 0);
    lv_obj_set_style_bg_color(hd, lv_color_white(), 0);
    lv_obj_set_style_pad_all(hd, 0, 0);
    lv_obj_set_style_border_width(hd, 1, 0);
    lv_obj_set_style_border_side(hd, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(hd, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *wifi = lv_label_create(hd);          /* 内置符号字体，省一份图标资源 */
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_align(wifi, LV_ALIGN_LEFT_MID, 5, 0);
    s_wifi_x = lv_label_create(hd);                /* 断开时显示 ✕（1-bit 屏没有灰度可表达弱化） */
    lv_label_set_text(s_wifi_x, LV_SYMBOL_CLOSE);
    lv_obj_align(s_wifi_x, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_add_flag(s_wifi_x, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bt = lv_label_create(hd);
    lv_label_set_text(bt, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(bt, LV_ALIGN_LEFT_MID, 33, 0);
    s_bt_x = lv_label_create(hd);
    lv_label_set_text(s_bt_x, LV_SYMBOL_CLOSE);
    lv_obj_align(s_bt_x, LV_ALIGN_LEFT_MID, 45, 0);
    lv_obj_add_flag(s_bt_x, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 4; i++) {                  /* 页码点 */
        s_dots[i] = lv_obj_create(hd);
        lv_obj_set_size(s_dots[i], 5, 5);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dots[i], 1, 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_black(), 0);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_white(), 0);
        lv_obj_set_pos(s_dots[i], 165 + i * 8, 9);
    }
    for (int i = 0; i < 2; i++) {                  /* 组方块（第 2 页用） */
        s_bank[i] = lv_obj_create(hd);
        lv_obj_set_size(s_bank[i], 7, 3);
        lv_obj_set_style_radius(s_bank[i], 0, 0);
        lv_obj_set_style_border_width(s_bank[i], 1, 0);
        lv_obj_set_style_border_color(s_bank[i], lv_color_black(), 0);
        lv_obj_set_style_bg_color(s_bank[i], lv_color_white(), 0);
        lv_obj_set_pos(s_bank[i], 205 + i * 9, 10);
    }

    s_batt_label = lv_label_create(hd);
    lv_label_set_text(s_batt_label, "82%");
    lv_obj_align(s_batt_label, LV_ALIGN_RIGHT_MID, -28, 0);

    s_batt_icon = lv_label_create(hd);
    lv_label_set_text(s_batt_icon, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_align(s_batt_icon, LV_ALIGN_RIGHT_MID, -6, 0);
}

/* 顶栏状态：电量百分比与电池图标档位、WiFi/蓝牙是否有连接（断开时图标后加 ✕） */
void Ui_UpdateHeader(const wesr_status_t *st)
{
    if (!st) return;
    char b[16];
    if (st->battery_v > 0.0f) snprintf(b, sizeof b, "%u%%", (unsigned)st->battery_pct);
    else                      snprintf(b, sizeof b, "--%%");
    lv_label_set_text(s_batt_label, b);

    const char *icon = LV_SYMBOL_BATTERY_EMPTY;
    if (st->charging)            icon = LV_SYMBOL_CHARGE;
    else if (st->battery_pct >= 90) icon = LV_SYMBOL_BATTERY_FULL;
    else if (st->battery_pct >= 65) icon = LV_SYMBOL_BATTERY_3;
    else if (st->battery_pct >= 40) icon = LV_SYMBOL_BATTERY_2;
    else if (st->battery_pct >= 15) icon = LV_SYMBOL_BATTERY_1;
    lv_label_set_text(s_batt_icon, icon);

    if (st->wifi_connected) lv_obj_add_flag(s_wifi_x, LV_OBJ_FLAG_HIDDEN);
    else                    lv_obj_clear_flag(s_wifi_x, LV_OBJ_FLAG_HIDDEN);
    if (st->bt_connected)   lv_obj_add_flag(s_bt_x, LV_OBJ_FLAG_HIDDEN);
    else                    lv_obj_clear_flag(s_bt_x, LV_OBJ_FLAG_HIDDEN);
}

static void build_statusbar(void)
{
    /* 状态条放屏幕最底边：设计稿是顶栏下方，但那会压住各页顶部内容（每页都是从 y=24 满高排的），
       放底部不用重排四页，反白时同样醒目。 */
    s_bar = lv_label_create(lv_scr_act());
    lv_obj_set_size(s_bar, UI_W, UI_BAR_H);
    lv_obj_set_pos(s_bar, 0, UI_H - UI_BAR_H);
    lv_obj_set_style_text_align(s_bar, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_bar, &font_cn12, 0);   /* 状态条是中文，必须用中文字体 */
    lv_obj_set_style_border_width(s_bar, 1, 0);
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
}

static void build_pages(void)
{
    for (int i = 0; i < 4; i++) {
        if (i == 0) {                 /* 第 1 页已经实现（Task 11） */
            s_pages[i] = Ui_Page1Create();
            continue;
        }
        if (i == 1) {                 /* 第 2 页：四宫格（Task 12） */
            s_pages[i] = Ui_Page2Create();
            continue;
        }
        if (i == 2) {                 /* 第 3 页：个股分时（Task 12） */
            s_pages[i] = Ui_Page3Create();
            continue;
        }
        if (i == 3) {                 /* 第 4 页：系统状态（Task 13） */
            s_pages[i] = Ui_Page4Create();
            continue;
        }
        s_pages[i] = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_pages[i], UI_W, UI_H - UI_HEADER_H);
        lv_obj_set_pos(s_pages[i], 0, UI_HEADER_H);
        lv_obj_set_style_radius(s_pages[i], 0, 0);
        lv_obj_set_style_border_width(s_pages[i], 0, 0);
        lv_obj_set_style_bg_color(s_pages[i], lv_color_white(), 0);
        lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_SCROLLABLE);
        /* 占位内容：Task 11–13 会把这里换成真正的页面 */
        char t[32];
        snprintf(t, sizeof t, "P%d placeholder", i + 1);
        lv_obj_t *l = lv_label_create(s_pages[i]);
        lv_label_set_text(l, t);
        lv_obj_center(l);
    }
}

void Ui_ShowPage(uint8_t page)
{
    if (page < 1 || page > 4) page = 1;
    s_page_now = page;
    for (int i = 0; i < 4; i++) {
        bool on = (i + 1) == page;
        lv_obj_set_style_bg_color(s_dots[i], on ? lv_color_black() : lv_color_white(), 0);
        if (on) lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
}

uint8_t Ui_CurrentPage(void) { return s_page_now; }

void Ui_Init(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), 0);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    build_header();
    build_pages();
    build_statusbar();
    lv_obj_move_foreground(s_bar);        /* 盖在页面之上，否则被页面容器遮住 */
    Ui_ShowPage(1);
}
