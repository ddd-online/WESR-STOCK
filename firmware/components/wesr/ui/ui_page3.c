/* 第 3 页 · 个股分时：全名 + n/8 + 现价 + 昨收 + 分时大图（斜纹/均价/量柱/时间轴）+ 开高低收量 */
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_page3.h"
#include "ui_trend.h"
#include <stdio.h>

static lv_obj_t *s_name, *s_idx, *s_pct, *s_price, *s_prev;
static lv_obj_t *s_canvas;
static lv_obj_t *s_tick[5];
static lv_obj_t *s_ohlc;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* 图例里的线段样例：实线整段，虚线 4px 一段（间距 2px） */
static void line_sample(lv_obj_t *parent, int x, int y, int w, bool dashed)
{
    const int seg = dashed ? 4 : w;
    for (int i = 0; i < w; i += (dashed ? 6 : w)) {
        int wid = (w - i) < seg ? (w - i) : seg;
        lv_obj_t *o = lv_obj_create(parent);
        lv_obj_set_size(o, wid, 1);
        lv_obj_set_pos(o, x + i, y);
        lv_obj_set_style_bg_color(o, lv_color_black(), 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }
}

lv_obj_t *Ui_Page3Create(void)
{
    lv_obj_t *page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(page, UI_W, UI_H - UI_HEADER_H);
    lv_obj_set_pos(page, 0, UI_HEADER_H);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    s_name  = label(page, &font_cn16, 8, 0);
    s_idx   = label(page, &font_num9, 78, 4);
    s_pct   = label(page, &font_num14, 0, 2);              /* 用户反馈：11px 偏小，提到 14px */
    lv_obj_align(s_pct, LV_ALIGN_TOP_RIGHT, -8, 3);
    s_price = label(page, &font_num25, 8, 16);
    s_prev  = label(page, &font_cn12, 118, 28);   /* "昨收 xxx" 含汉字，必须用中文字体 */

    /* 图例：实线=价格、虚线=均价、横虚线=昨收。
       光写「价 均 昨收」三个字没人看得懂（用户反馈），得带线段样例 —— 照设计稿 p3sub。 */
    int lgx = 236, lgy = 28;
    line_sample(page, lgx, lgy + 6, 14, false);
    lv_label_set_text(label(page, &font_cn12, lgx + 17, lgy), "价");
    lgx += 34;                                 /* 样例 17 + 字 12 + 5px 间隙，别挤在一起 */
    line_sample(page, lgx, lgy + 6, 14, true);
    lv_label_set_text(label(page, &font_cn12, lgx + 17, lgy), "均");
    lgx += 34;
    line_sample(page, lgx, lgy + 6, 14, true);
    lv_label_set_text(label(page, &font_cn12, lgx + 17, lgy), "昨收");

    s_canvas = Ui_TrendCreate(page, UI_BIG_W, UI_BIG_H, true, true, true);
    lv_obj_set_pos(s_canvas, 10, 44);

    /* 时间刻度与开高低量都跟着画布高度走，别写死 y —— 画布一改高度就得跟着改两处 */
    const int tick_y = 44 + UI_BIG_H + 2;
    const int ohlc_y = tick_y + 16;
    static const char *ticks[5] = { "09:30", "10:30", "11:30", "14:00", "15:00" };
    static const int    idxs[5] = { 0, 60, 120, 181, 241 };
    for (int i = 0; i < 5; i++) {
        s_tick[i] = label(page, &font_num9, 10 + wesr_index_to_x(idxs[i], 0, UI_BIG_W), tick_y);
        lv_label_set_text(s_tick[i], ticks[i]);
    }
    /* 边界两个标签往内收，避免被切掉 */
    lv_obj_set_pos(s_tick[0], 10, tick_y);
    lv_obj_set_pos(s_tick[4], 10 + UI_BIG_W - 40, tick_y);

    s_ohlc = label(page, &font_cn12, 8, ohlc_y);     /* "开高低量" 同上 */
    /* 同上：LVGL label 默认是 "Text" */
    lv_label_set_text(s_name, "");
    lv_label_set_text(s_idx, "");
    lv_label_set_text(s_pct, "");
    lv_label_set_text(s_price, "");
    lv_label_set_text(s_prev, "");
    lv_label_set_text(s_ohlc, "");
    return page;
}

void Ui_Page3Update(const wesr_minute_t *m, const wesr_quote_t *q,
                    const wesr_stock_cfg_t *cfg, uint8_t idx, uint8_t count)
{
    char b[96], t[32], pb[16];
    if (!q || !cfg || count == 0 || idx >= count) {
        lv_label_set_text(s_name, "未配置股票");
        lv_label_set_text(s_idx, "");
        lv_label_set_text(s_pct, "");
        lv_label_set_text(s_price, "");
        lv_label_set_text(s_prev, "");
        lv_label_set_text(s_ohlc, "");
        if (s_canvas) Ui_TrendRender(s_canvas, NULL, 0);
        return;
    }
    lv_label_set_text(s_name, cfg[idx].name);
    snprintf(t, sizeof t, "%d/%d", idx + 1, count);
    lv_label_set_text(s_idx, t);
    wesr_fmt_pct(b, sizeof b, q[idx].chg_pct, q[idx].valid);
    lv_label_set_text(s_pct, b);
    wesr_fmt_price(b, sizeof b, q[idx].last, q[idx].valid);
    lv_label_set_text(s_price, b);
    wesr_fmt_price(pb, sizeof pb, q[idx].prev_close, q[idx].valid);
    snprintf(t, sizeof t, "昨收 %s", pb);
    lv_label_set_text(s_prev, t);

    if (m && m->valid && q[idx].valid) {
        snprintf(b, sizeof b, "开%8.2f  高%8.2f  低%8.2f  量%.0f手",
                 q[idx].open, q[idx].high, q[idx].low, (double)q[idx].vol_hands);
        lv_label_set_text(s_ohlc, b);
    } else {
        lv_label_set_text(s_ohlc, "开 —　高 —　低 —　量 —");
    }
    if (s_canvas) Ui_TrendRender(s_canvas, m, q[idx].prev_close);
}
