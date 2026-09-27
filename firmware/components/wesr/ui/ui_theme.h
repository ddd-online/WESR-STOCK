#pragma once
/* 设计稿（design/pages-mockup.html）里的尺寸，硬编码成常量，四页只引用这里 */
#define UI_W           400
#define UI_H           300
#define UI_HEADER_H    24
#define UI_BAR_H       14
#define UI_P1_LEFT_W   300
#define UI_P1_RIGHT_W  100
#define UI_P1_ROWS     8
#define UI_Q_W         200
#define UI_Q_H         138
#define UI_TREND_W     184
#define UI_TREND_H     86
#define UI_BIG_W       380
#define UI_BIG_H       168

/* 位图字体（由 tools/gen_fonts.ps1 生成到本目录，构建产物不入 git） */
#include "lvgl.h"
LV_FONT_DECLARE(font_num78);
LV_FONT_DECLARE(font_num25);
LV_FONT_DECLARE(font_num19);
LV_FONT_DECLARE(font_num16);
LV_FONT_DECLARE(font_num14);
LV_FONT_DECLARE(font_num12);
LV_FONT_DECLARE(font_num11);
LV_FONT_DECLARE(font_num9);
LV_FONT_DECLARE(font_cn16);
