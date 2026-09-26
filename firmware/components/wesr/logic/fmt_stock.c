#include "wesr_logic.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DASH  "\xE2\x80\x94"        /* — */
#define UP    "\xE2\x96\xB2"        /* ▲ */
#define DOWN  "\xE2\x96\xBC"        /* ▼ */

void wesr_fmt_price(char *out, size_t cap, float v, bool valid)
{
    if (!valid || !isfinite(v)) { snprintf(out, cap, "%s", DASH); return; }
    snprintf(out, cap, "%.2f", v);
}

void wesr_fmt_pct(char *out, size_t cap, float pct, bool valid)
{
    if (!valid || !isfinite(pct)) { snprintf(out, cap, "%s", DASH); return; }
    if (fabsf(pct) < 0.005f) { snprintf(out, cap, "0.00%%"); return; }
    snprintf(out, cap, "%s%.2f%%", pct > 0 ? UP : DOWN, fabsf(pct));
}

int wesr_price_font_px(float v)
{
    float a = fabsf(v);
    return (a >= 1000.0f) ? 12 : 14;   /* 整数部分 ≥4 位 → 降一档 */
}

/* 取 UTF-8 第一个字符（汉字 3 字节，ASCII 1 字节） */
void wesr_first_char(const char *utf8, char *out, size_t cap)
{
    out[0] = 0;
    if (!utf8 || !utf8[0]) return;
    unsigned char c = (unsigned char)utf8[0];
    size_t len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    if (len + 1 > cap) return;
    for (size_t i = 0; i < len; i++) {
        if (!utf8[i]) { out[0] = 0; return; }
        out[i] = utf8[i];
    }
    out[len] = 0;
}

/* 首字重复时，冲突的那只退成前两个字（边界规则 13） */
void wesr_make_marks(wesr_stock_cfg_t *list, int n)
{
    for (int i = 0; i < n; i++) {
        wesr_first_char(list[i].name, list[i].mark, sizeof list[i].mark);
    }
    for (int i = 0; i < n; i++) {
        if (!list[i].mark[0]) continue;
        for (int j = 0; j < i; j++) {
            if (strcmp(list[i].mark, list[j].mark) != 0) continue;
            /* 前两个字：取到第二个 UTF-8 字符边界 */
            char two[WESR_MARK_LEN] = {0};
            wesr_first_char(list[i].name, two, sizeof two);
            size_t used = strlen(two);
            wesr_first_char(list[i].name + used, two + used, sizeof two - used);
            snprintf(list[i].mark, sizeof list[i].mark, "%s", two);
            break;
        }
    }
}
