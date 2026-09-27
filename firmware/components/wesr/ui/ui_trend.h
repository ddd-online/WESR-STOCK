#pragma once
#include "lvgl.h"
#include "wesr_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1-bit 分时图控件：内部用 chart_draw 画到 1bpp 缓冲，再交给 LVGL canvas 显示 */
lv_obj_t *Ui_TrendCreate(lv_obj_t *parent, int w, int h, bool hatch, bool volume, bool grid);
void      Ui_TrendRender(lv_obj_t *canvas, const wesr_minute_t *m, float prev_close);

#ifdef __cplusplus
}
#endif
