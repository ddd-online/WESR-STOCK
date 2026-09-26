#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

typedef struct { uint8_t page, group, idx, count; } wesr_nav_t;
void    wesr_nav_init(wesr_nav_t *n, uint8_t count);
void    wesr_nav_click(wesr_nav_t *n);
void    wesr_nav_double(wesr_nav_t *n);
void    wesr_nav_set_count(wesr_nav_t *n, uint8_t count);
uint8_t wesr_nav_group_start(const wesr_nav_t *n);
uint8_t wesr_nav_pages(void);
