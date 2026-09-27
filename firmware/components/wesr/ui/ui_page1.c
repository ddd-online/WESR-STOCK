/* 第 1 页 · 时钟看盘：左 300px 大时钟 + 温湿度，右 100px 8 行「首字 / 涨跌 / 现价」。
   坐标对齐 design/pages-mockup.html 的设计稿。 */
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_page1.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_clock, *s_date, *s_temp, *s_humi;
static lv_obj_t *s_mark[UI_P1_ROWS], *s_pct[UI_P1_ROWS], *s_price[UI_P1_ROWS];

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *Ui_Page1Create(void)
{
    lv_obj_t *page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(page, UI_W, UI_H - UI_HEADER_H);
    lv_obj_set_pos(page, 0, UI_HEADER_H);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 左栏 300px ---- */
    lv_obj_t *left = lv_obj_create(page);
    lv_obj_set_size(left, UI_P1_LEFT_W, LV_PCT(100));
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_style_radius(left, 0, 0);
    lv_obj_set_style_bg_color(left, lv_color_white(), 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    /* 时间区（分隔线以上）：时钟 + 日期整组**水平垂直居中**。
       用 flex 居中最稳：标签宽度随字体变化，绝对定位要么偏心、要么溢出到右栏
       （之前 104px 不居中时溢出 2px 就把右栏顶部两行擦白了）。 */
    lv_obj_t *timearea = lv_obj_create(left);
    lv_obj_set_size(timearea, UI_P1_LEFT_W, 206);      /* 到温湿度分隔线为止 */
    lv_obj_set_pos(timearea, 0, 0);
    lv_obj_set_style_pad_all(timearea, 0, 0);
    /* 点阵字体的墨迹不在字形盒正中（num96 数字偏上、cn16 汉字占满），
       只靠 flex 居中会让整组**偏低 3.5px**。补一个底部 padding 把它顶回来：
       实测墨迹 y=72..188（中心 130），区域中心 126.5 → 差 3.5，故 pad=7。
       字体和日期格式都是定高定长，这个偏移是常数，不会随数据变。 */
    lv_obj_set_style_pad_bottom(timearea, 7, 0);
    lv_obj_set_style_pad_row(timearea, 6, 0);
    lv_obj_set_style_radius(timearea, 0, 0);
    lv_obj_set_style_border_width(timearea, 0, 0);
    lv_obj_set_style_bg_opa(timearea, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(timearea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(timearea, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(timearea, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_clock = mk_label(timearea, &font_num96, 0, 0);
    lv_label_set_text(s_clock, "00:00");
    /* 日期含汉字，必须用中文字体（数字字体没有 CJK 字形，会显示成方框） */
    s_date = mk_label(timearea, &font_cn16, 0, 0);
    lv_label_set_text(s_date, "--月--日");

    /* 温湿度：底部两栏，中间一条分隔线 */
    lv_obj_t *line = lv_obj_create(left);
    lv_obj_set_size(line, UI_P1_LEFT_W - 24, 1);
    lv_obj_set_pos(line, 12, 206);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);

    lv_obj_t *t_lb = mk_label(left, &font_cn16, 48, 214);
    lv_label_set_text(t_lb, "温度");
    s_temp = mk_label(left, &font_num25, 26, 232);
    lv_label_set_text(s_temp, "--.-");

    lv_obj_t *h_lb = mk_label(left, &font_cn16, 200, 214);
    lv_label_set_text(h_lb, "湿度");
    s_humi = mk_label(left, &font_num25, 186, 232);
    lv_label_set_text(s_humi, "--");

    /* ---- 右栏 100px：8 行 ---- */
    lv_obj_t *right = lv_obj_create(page);
    lv_obj_set_size(right, UI_P1_RIGHT_W, LV_PCT(100));
    lv_obj_set_pos(right, UI_P1_LEFT_W, 0);
    lv_obj_set_style_radius(right, 0, 0);
    lv_obj_set_style_bg_color(right, lv_color_white(), 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    const int row_h = (UI_H - UI_HEADER_H) / UI_P1_ROWS;   /* 34 */
    for (int i = 0; i < UI_P1_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(right);
        lv_obj_set_size(row, UI_P1_RIGHT_W, row_h);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        if (i < UI_P1_ROWS - 1) {
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        } else {
            lv_obj_set_style_border_width(row, 0, 0);
        }
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        s_mark[i] = mk_label(row, &font_cn16, 6, 0);
        s_pct[i]  = mk_label(row, &font_num12, 0, 2);      /* 用户反馈：9px 偏小，提到 12px */
        s_price[i] = mk_label(row, &font_num14, 6, 17);
        /* LVGL 的 label 默认文字是 "Text"，必须先置空，否则没数据时满屏 "Text" */
        lv_label_set_text(s_mark[i], "");
        lv_label_set_text(s_pct[i], "");
        lv_label_set_text(s_price[i], "");
    }
    return page;
}

void Ui_Page1SetClock(const char *hhmm, const char *date)
{
    lv_label_set_text(s_clock, hhmm);
    lv_label_set_text(s_date, date);
}

void Ui_Page1SetEnv(float temp_c, float humi_pct)
{
    char b[16];
    snprintf(b, sizeof b, "%.1f", temp_c);
    lv_label_set_text(s_temp, b);
    snprintf(b, sizeof b, "%.0f%%", humi_pct);
    lv_label_set_text(s_humi, b);
}

void Ui_Page1Update(const wesr_quote_t *q, const wesr_stock_cfg_t *cfg, uint8_t count)
{
    char b[32];
    for (int i = 0; i < UI_P1_ROWS; i++) {
        if (!q || i >= count) {
            lv_label_set_text(s_mark[i], "");
            lv_label_set_text(s_pct[i], "");
            lv_label_set_text(s_price[i], "");
            continue;
        }
        lv_label_set_text(s_mark[i], cfg[i].mark);
        wesr_fmt_pct(b, sizeof b, q[i].chg_pct, q[i].valid);
        lv_label_set_text(s_pct[i], b);
        /* 涨跌右对齐 */
        lv_obj_align(s_pct[i], LV_ALIGN_TOP_RIGHT, -4, 4);
        wesr_fmt_price(b, sizeof b, q[i].last, q[i].valid);
        lv_label_set_text(s_price[i], b);
        lv_obj_set_style_text_font(s_price[i],
            (q[i].valid && wesr_price_font_px(q[i].last) == 12) ? &font_num12 : &font_num14, 0);
    }
}
