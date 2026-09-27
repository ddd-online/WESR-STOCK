#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* C++（main.cpp / board_init.cpp）也要调用这些纯 C 函数，必须给它们 C 链接 */
#ifdef __cplusplus
extern "C" {
#endif

#define WESR_MAX_STOCKS 8
#define WESR_MAX_POINTS 300
#define WESR_CODE_LEN   12
#define WESR_NAME_LEN   32
#define WESR_MARK_LEN   10

typedef struct {
    char code[WESR_CODE_LEN];
    char name[WESR_NAME_LEN];
    char mark[WESR_MARK_LEN];
} wesr_stock_cfg_t;

typedef struct {
    float last, prev_close, open, high, low, chg, chg_pct;
    uint32_t vol_hands;
    uint64_t stamp;          /* yyyymmddHHMMSS，放不进 uint32 */
    bool valid;
} wesr_quote_t;

typedef struct {
    uint16_t hhmm;
    float price, avg;
    float vol;               /* 本分钟成交量（手）= 累计量差值，画量柱用 */
} wesr_point_t;

typedef struct {
    wesr_point_t pts[WESR_MAX_POINTS];
    uint16_t n;
    uint32_t day;            /* 接口返回的 yyyymmdd */
    bool valid;
} wesr_minute_t;

bool wesr_parse_quote_line(const char *line, wesr_quote_t *q);
bool wesr_parse_minute_json(const char *json, wesr_minute_t *out);

typedef struct {
    bool has_quote;          /* qt 数组解析成功 */
    wesr_quote_t quote;      /* 现价/昨收等，从 qt 数组按快照下标取 */
    bool closed;             /* 服务端 market 字段说休市 */
} wesr_minute_meta_t;

bool wesr_parse_minute_meta(const char *json, wesr_minute_meta_t *meta);

void wesr_fmt_price(char *out, size_t cap, float v, bool valid);
void wesr_fmt_pct(char *out, size_t cap, float pct, bool valid);
int  wesr_price_font_px(float v);
void wesr_first_char(const char *utf8, char *out, size_t cap);
void wesr_make_marks(wesr_stock_cfg_t *list, int n);

int  wesr_minute_index(uint16_t hhmm);
int  wesr_index_to_x(int idx, int x0, int x1);
bool wesr_in_trading(uint16_t hhmm);

/* 星期几：0=周日 … 6=周六（Zeller 公式，不依赖 RTC 的 weekday 寄存器是否被写过） */
uint8_t wesr_weekday(int year, int month, int day);

typedef struct { uint8_t page, group, idx, count; } wesr_nav_t;
void    wesr_nav_init(wesr_nav_t *n, uint8_t count);
void    wesr_nav_click(wesr_nav_t *n);
void    wesr_nav_double(wesr_nav_t *n);
void    wesr_nav_set_count(wesr_nav_t *n, uint8_t count);
uint8_t wesr_nav_group_start(const wesr_nav_t *n);
uint8_t wesr_nav_pages(void);

typedef enum { WESR_FETCH_NONE = 0, WESR_FETCH_QUOTES, WESR_FETCH_MINUTES } wesr_fetch_t;
typedef struct {
    uint8_t page;
    uint16_t interval_s;
    uint32_t next_ms;
    uint8_t fail_streak;
    bool offline;
} wesr_sched_t;
void         wesr_sched_init(wesr_sched_t *s, uint16_t interval_s);
void         wesr_sched_page(wesr_sched_t *s, uint8_t page);
wesr_fetch_t wesr_sched_tick(wesr_sched_t *s, uint32_t now_ms, bool trading);
void         wesr_sched_result(wesr_sched_t *s, bool ok, uint32_t now_ms);

/* 1bpp 位图：行按 (w+7)/8 字节对齐，MSB 在左 */
typedef struct { uint8_t *buf; int w, h, stride; } wesr_bmp_t;
void wesr_bmp_init(wesr_bmp_t *b, uint8_t *buf, int w, int h);
void wesr_bmp_clear(wesr_bmp_t *b);
void wesr_bmp_px(wesr_bmp_t *b, int x, int y);
bool wesr_bmp_get(const wesr_bmp_t *b, int x, int y);
void wesr_bmp_hline(wesr_bmp_t *b, int x0, int x1, int y);
void wesr_bmp_line(wesr_bmp_t *b, int x0, int y0, int x1, int y1);
void wesr_bmp_dash_hline(wesr_bmp_t *b, int x0, int x1, int y, int on, int off);
void wesr_bmp_vbar(wesr_bmp_t *b, int x, int w, int y_top, int y_bot);
void wesr_bmp_hatch45(wesr_bmp_t *b, const int *y_line, int x0, int x1, int y_base,
                      int period, int on);

typedef struct {
    int  pad_top, pad_bottom, pad_x;
    int  price_h;            /* 价格区高度 */
    int  vol_top, vol_h;     /* 成交量条：vol_h = 0 表示不画 */
    bool hatch;              /* 价格与昨收之间填斜纹 */
    bool grid;               /* 10:30 / 11:30·13:00 / 14:00 竖虚线 */
    float span_ratio;        /* 纵轴留白系数，用 1.1；<=0 时按 1.1 */
} wesr_chart_opts_t;

void wesr_chart_render(wesr_bmp_t *b, const wesr_minute_t *m, float prev_close,
                       const wesr_chart_opts_t *o);

typedef struct {
    char ssid[33];
    char pass[65];
    wesr_stock_cfg_t stocks[WESR_MAX_STOCKS];
    uint8_t count;
    uint16_t refresh_sec;
} wesr_app_cfg_t;

void wesr_cfg_defaults(wesr_app_cfg_t *cfg);
bool wesr_code_valid(const char *code);

#ifdef __cplusplus
}
#endif
