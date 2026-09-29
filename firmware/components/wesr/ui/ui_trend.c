#include "ui_trend.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

#define MAX_TRENDS 8

typedef struct {
    lv_obj_t *canvas;
    uint8_t  *buf;
    int       w, h;
    bool      hatch, volume, grid;
} trend_t;

static trend_t s_trends[MAX_TRENDS];
static int     s_trend_n;

lv_obj_t *Ui_TrendCreate(lv_obj_t *parent, int w, int h, bool hatch, bool volume, bool grid)
{
    if (s_trend_n >= MAX_TRENDS) return NULL;
    trend_t *t = &s_trends[s_trend_n++];
    t->w = w; t->h = h;
    t->hatch = hatch; t->volume = volume; t->grid = grid;
    size_t bytes = (size_t)((w + 7) / 8) * (size_t)h;
    t->buf = (uint8_t *)heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    if (!t->buf) { s_trend_n--; return NULL; }
    t->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(t->canvas, t->buf, w, h, LV_IMG_CF_ALPHA_1BIT);
    lv_obj_set_size(t->canvas, w, h);
    return t->canvas;
}

void Ui_TrendRender(lv_obj_t *canvas, const wesr_minute_t *m, float prev_close)
{
    for (int i = 0; i < s_trend_n; i++) {
        trend_t *t = &s_trends[i];
        if (t->canvas != canvas) continue;
        wesr_bmp_t b;
        wesr_bmp_init(&b, t->buf, t->w, t->h);
        wesr_chart_opts_t o;
        memset(&o, 0, sizeof o);
        o.pad_top = 6;
        o.pad_bottom = 4;
        o.pad_x = 2;
        /* 量柱区：设计稿是 20px（168 高的画布），这里画布更高，给到 24 才不塌 */
        int volh = t->volume ? 24 : 0;
        o.price_h = t->volume ? (t->h - volh - 10) : t->h;   /* 10 = 价格区与量柱之间的间隙 */
        o.vol_top = t->h - volh - 4;
        o.vol_h   = volh;
        o.hatch = t->hatch;
        o.grid = t->grid;
        o.span_ratio = 1.1f;
        if (m && m->valid) wesr_chart_render(&b, m, prev_close, &o);
        lv_obj_invalidate(canvas);
        return;
    }
}
