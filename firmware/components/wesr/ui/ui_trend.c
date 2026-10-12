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
    /* 上一次画的内容指纹：完全一样就整段跳过（memset + 光栅化 + invalidate 全省） */
    uint32_t  sig;
    bool      has_sig;
} trend_t;

static trend_t s_trends[MAX_TRENDS];
static int     s_trend_n;

/* 分时数据的指纹：点数 + 日期 + 最后一个点 + 昨收。分时的点只会在末尾追加，
   所以末点变了就说明要重画；点数/日期/昨收变了更是要重画。 */
static uint32_t trend_sig(const wesr_minute_t *m, float prev_close)
{
    if (!m || !m->valid || m->n == 0) {
        return 0xA5A5A5A5u;          /* "无数据"也算一种可缓存的状态 */
    }
    const wesr_point_t *p = &m->pts[m->n - 1];
    uint32_t h = 2166136261u;
    /* 浮点按位参与哈希：用 memcpy 取位模式，别用指针强转（会违反 strict-aliasing） */
    uint32_t price_bits, prev_bits;
    memcpy(&price_bits, &p->price, sizeof price_bits);
    memcpy(&prev_bits, &prev_close, sizeof prev_bits);
    uint32_t words[5] = {
        (uint32_t)m->n,
        m->day,
        (uint32_t)(p->hhmm),
        price_bits,
        prev_bits,
    };
    for (int i = 0; i < 5; i++) {
        h = (h ^ words[i]) * 16777619u;
    }
    return h;
}

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

        /* 内容没变就什么都不做：以前每次更新都 memset 整块 bitmap + 全量重画 +
           无条件 lv_obj_invalidate，而 full_refresh=1 下这一次 invalidate 就是整屏重绘。 */
        uint32_t sig = trend_sig(m, prev_close);
        if (t->has_sig && t->sig == sig) return;
        t->sig = sig;
        t->has_sig = true;

        wesr_bmp_t b;
        wesr_bmp_init(&b, t->buf, t->w, t->h);
        wesr_chart_opts_t o;
        memset(&o, 0, sizeof o);
        o.pad_top = 6;
        o.pad_bottom = 4;
        o.pad_x = 2;
        /* 量柱区：设计稿是 20px（168 高的画布），这里画布更高，给到 24 才不塌 */
        int volh = t->volume ? 24 : 0;
        o.price_h = t->volume ? (t->h - volh - 4) : t->h;    /* 4 = 价格区与量柱之间的间隙 */
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
