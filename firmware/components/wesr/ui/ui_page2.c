/* 第 2 页 · 四宫格分时：2×2，每格 200×138（名称/涨跌/现价 + 184×86 分时图）。
   数据不够的格子整块留空（边界规则 4）。 */
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_page2.h"
#include "ui_trend.h"
#include <string.h>

typedef struct {
    lv_obj_t *cell, *name, *pct, *price, *canvas;
} cell_t;

static cell_t s_cell[4];

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *Ui_Page2Create(void)
{
    lv_obj_t *page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(page, UI_W, UI_H - UI_HEADER_H);
    lv_obj_set_pos(page, 0, UI_HEADER_H);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        int col = i % 2, row = i / 2;
        lv_obj_t *c = lv_obj_create(page);
        lv_obj_set_size(c, UI_Q_W, UI_Q_H);
        lv_obj_set_pos(c, col * UI_Q_W, row * UI_Q_H);
        lv_obj_set_style_radius(c, 0, 0);
        lv_obj_set_style_bg_color(c, lv_color_white(), 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_black(), 0);
        lv_obj_set_style_border_side(c,
            (col == 0 && row == 0) ? (lv_border_side_t)(LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM) :
            (col == 1 && row == 0) ? LV_BORDER_SIDE_BOTTOM :
            (col == 0 && row == 1) ? LV_BORDER_SIDE_RIGHT : LV_BORDER_SIDE_NONE, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        s_cell[i].cell  = c;
        s_cell[i].name  = label(c, &font_cn16, 6, 2);
        s_cell[i].pct   = label(c, &font_num9, 0, 5);
        s_cell[i].price = label(c, &font_num16, 6, 20);
        s_cell[i].canvas = Ui_TrendCreate(c, UI_TREND_W, UI_TREND_H, false, false, false);
        lv_obj_set_pos(s_cell[i].canvas, 8, 40);
    }
    return page;
}

void Ui_Page2Update(const wesr_minute_t *m, const wesr_quote_t *q,
                    const wesr_stock_cfg_t *cfg, uint8_t start, uint8_t count)
{
    char b[32];
    for (int i = 0; i < 4; i++) {
        uint8_t idx = (uint8_t)(start + i);
        if (!q || !m || idx >= count) {          /* 整格留空：不画空坐标系 */
            lv_label_set_text(s_cell[i].name, "");
            lv_label_set_text(s_cell[i].pct, "");
            lv_label_set_text(s_cell[i].price, "");
            if (s_cell[i].canvas) Ui_TrendRender(s_cell[i].canvas, NULL, 0);
            continue;
        }
        lv_label_set_text(s_cell[i].name, cfg[idx].name);
        wesr_fmt_pct(b, sizeof b, q[idx].chg_pct, q[idx].valid);
        lv_label_set_text(s_cell[i].pct, b);
        lv_obj_align(s_cell[i].pct, LV_ALIGN_TOP_RIGHT, -5, 5);
        wesr_fmt_price(b, sizeof b, q[idx].last, q[idx].valid);
        lv_label_set_text(s_cell[i].price, b);
        if (s_cell[i].canvas) Ui_TrendRender(s_cell[i].canvas, &m[i], q[idx].prev_close);
    }
}
