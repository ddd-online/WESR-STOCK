#include "wesr_logic.h"
#include <math.h>
#include <stdlib.h>          /* abs() */
#include <string.h>

void wesr_bmp_init(wesr_bmp_t *b, uint8_t *buf, int w, int h)
{
    b->buf = buf;
    b->w = w;
    b->h = h;
    b->stride = (w + 7) / 8;
    wesr_bmp_clear(b);
}

void wesr_bmp_clear(wesr_bmp_t *b)
{
    memset(b->buf, 0, (size_t)b->stride * (size_t)b->h);
}

void wesr_bmp_px(wesr_bmp_t *b, int x, int y)
{
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return;
    b->buf[y * b->stride + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
}

bool wesr_bmp_get(const wesr_bmp_t *b, int x, int y)
{
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return false;
    return (b->buf[y * b->stride + (x >> 3)] >> (7 - (x & 7))) & 1u;
}

void wesr_bmp_hline(wesr_bmp_t *b, int x0, int x1, int y)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) wesr_bmp_px(b, x, y);
}

void wesr_bmp_line(wesr_bmp_t *b, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        wesr_bmp_px(b, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void wesr_bmp_dash_hline(wesr_bmp_t *b, int x0, int x1, int y, int on, int off)
{
    if (on < 1) on = 1;
    if (off < 1) off = 1;
    int period = on + off;
    for (int x = x0; x <= x1; x++) {
        if (((x - x0) % period) < on) wesr_bmp_px(b, x, y);
    }
}

void wesr_bmp_vbar(wesr_bmp_t *b, int x, int w, int y_top, int y_bot)
{
    for (int i = 0; i < w; i++)
        for (int y = y_top; y <= y_bot; y++) wesr_bmp_px(b, x + i, y);
}

void wesr_bmp_hatch45(wesr_bmp_t *b, const int *y_line, int x0, int x1, int y_base,
                      int period, int on)
{
    if (period < 2) period = 2;
    for (int x = x0; x <= x1; x++) {
        int yl = y_line[x - x0];
        int lo = yl < y_base ? yl : y_base;
        int hi = yl < y_base ? y_base : yl;
        if (lo < 0) lo = 0;
        if (hi >= b->h) hi = b->h - 1;
        for (int y = lo; y <= hi; y++) {
            int d = (x + y) % period;
            if (d < 0) d += period;
            if (d < on) wesr_bmp_px(b, x, y);
        }
    }
}

/* 分时图：纵轴以昨收为中心上下对称；实线=价格、虚线=均价、横虚线=昨收、斜纹=涨跌面积 */
void wesr_chart_render(wesr_bmp_t *b, const wesr_minute_t *m, float prev_close,
                       const wesr_chart_opts_t *o)
{
    if (!b || !m || !o || m->n == 0) return;
    if (m->n > WESR_MAX_POINTS) return;      /* 守住数组边界 */
    if (b->w > 400) return;                  /* 逐列插值数组的上限 */

    int x0 = o->pad_x, x1 = b->w - o->pad_x;
    int price_h = o->price_h > 0 ? o->price_h : b->h;
    float ratio = o->span_ratio > 0 ? o->span_ratio : 1.1f;
    if (!(prev_close > 0)) prev_close = m->pts[0].price;
    if (!(prev_close > 0)) return;           /* 全无数据：留白 */

    float hi = 1.0f, lo = 1.0f;
    for (uint16_t i = 0; i < m->n; i++) {
        float r = m->pts[i].price / prev_close;
        if (r > hi) hi = r;
        if (r < lo) lo = r;
    }
    float span = fmaxf(hi - 1.0f, 1.0f - lo) * ratio;
    if (span < 0.002f) span = 0.002f;

    int top = o->pad_top, bot = price_h - o->pad_bottom;
    float units = 2.0f * span;
    int y_base = top + (int)((float)(bot - top) * span / units);

    /* 时间刻度竖虚线：10:30 / 11:30·13:00 / 14:00 */
    if (o->grid) {
        const int marks[3] = { 60, 120, 181 };
        for (int k = 0; k < 3; k++) {
            int xg = wesr_index_to_x(marks[k], x0, x1);
            for (int y = top; y < bot; y++) {
                if (((y - top) % 4) < 1) wesr_bmp_px(b, xg, y);
            }
        }
    }

    /* 这三个数组加起来 4KB，放栈上会把小栈任务（main 默认 3.5KB）压爆 —— 实测栈溢出重启。
       渲染只在持有 LVGL 锁时调用，不存在并发，用 static 复用即可。 */
    static int y_line[WESR_MAX_POINTS];
    static int xs[WESR_MAX_POINTS];
    for (uint16_t i = 0; i < m->n; i++) {
        float r = m->pts[i].price / prev_close;
        int idx = wesr_minute_index(m->pts[i].hhmm);
        xs[i] = wesr_index_to_x(idx, x0, x1);
        float up = (1.0f + span) - r;
        int y = top + (int)((float)(bot - top) * up / units);
        if (y < top) y = top;
        if (y > bot) y = bot;
        y_line[i] = y;
    }

    /* 涨/跌面积：斜纹。
       y_line 按"点"存，而 hatch45 按"列"取 y；点数少时列数远大于点数，
       直接传 y_line 会越界读。这里先按列线性插值出一份稠密的逐列 y。 */
    if (o->hatch) {
        static int col_y[400];
        int x_l = xs[0], x_r = xs[m->n - 1];
        uint16_t seg = 0;
        for (int x = x_l; x <= x_r; x++) {
            while (seg + 1 < m->n && x > xs[seg + 1]) seg++;
            if (seg + 1 >= m->n) { col_y[x - x_l] = y_line[m->n - 1]; continue; }
            int sx0 = xs[seg], sx1 = xs[seg + 1];
            int sy0 = y_line[seg], sy1 = y_line[seg + 1];
            int dx = sx1 - sx0;
            col_y[x - x_l] = (dx <= 0) ? sy1 : (sy0 + (sy1 - sy0) * (x - sx0) / dx);
        }
        wesr_bmp_hatch45(b, col_y, x_l, x_r, y_base, 4, 1);
    }

    /* 昨收基准线（3 on / 2 off） */
    wesr_bmp_dash_hline(b, x0, x1, y_base, 3, 2);

    /* 均价线（虚线，用 2 on 2 off 近似 2.5/2） */
    for (uint16_t i = 1; i < m->n; i++) {
        if ((i % 4) >= 2) continue;
        float r = m->pts[i].avg / prev_close;
        int y1 = top + (int)((float)(bot - top) * ((1.0f + span) - r) / units);
        float r0 = m->pts[i - 1].avg / prev_close;
        int y0 = top + (int)((float)(bot - top) * ((1.0f + span) - r0) / units);
        wesr_bmp_line(b, xs[i - 1], y0, xs[i], y1);
    }

    /* 价格线（实线） */
    for (uint16_t i = 1; i < m->n; i++) {
        wesr_bmp_line(b, xs[i - 1], y_line[i - 1], xs[i], y_line[i]);
    }

    /* 成交量柱 */
    if (o->vol_h > 0) {
        float vmax = 0.0f;
        for (uint16_t i = 0; i < m->n; i++) {
            if (m->pts[i].vol > vmax) vmax = m->pts[i].vol;
        }
        if (vmax > 0) {
            for (uint16_t i = 0; i < m->n; i++) {
                int h = (int)((float)o->vol_h * (m->pts[i].vol / vmax));
                if (h < 1 && m->pts[i].vol > 0) h = 1;
                if (h > 0) {
                    wesr_bmp_vbar(b, xs[i], 1, o->vol_top + o->vol_h - h,
                                  o->vol_top + o->vol_h - 1);
                }
            }
        }
    }
}
