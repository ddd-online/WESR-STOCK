#pragma once
/* 只在文字真的变了才 lv_label_set_text。
   LVGL 8 的 lv_label_set_text() 第一句就是 lv_obj_invalidate()，而且比较的是**指针**不是内容
   （lv_label.c:90/95），所以"把同一个字符串再设一遍"也会让整屏重绘一次；
   本项目的 full_refresh=1 下，任何一次 invalidate 都等于整屏重绘 + 整帧落屏。
   实测传感器 10s 周期里大量帧的值其实没变，这一层就能把那些帧整帧省掉。 */
#include "lvgl.h"

static inline void Ui_SetTextIfChanged(lv_obj_t *l, const char *s)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (cur && s && strcmp(cur, s) == 0) return;
    if ((!cur || !cur[0]) && (!s || !s[0])) return;
    lv_label_set_text(l, s ? s : "");
}

/* 同理：lv_obj_set_style_* 是无条件 lv_obj_refresh_style（lv_obj_style.c:270-276），
   同一个字体反复设也会重绘。 */
static inline void Ui_SetFontIfChanged(lv_obj_t *l, const lv_font_t *f)
{
    if (!l) return;
    if (lv_obj_get_style_text_font(l, LV_PART_MAIN) == f) return;
    lv_obj_set_style_text_font(l, f, 0);
}
