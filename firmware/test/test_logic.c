#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wesr_logic.h"

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f && "fixture missing");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static void test_quote(void)
{
    char *raw = read_file("test/fixtures/quote_sh600519.txt");
    char *line = strtok(raw, "\n");
    wesr_quote_t q;
    assert(wesr_parse_quote_line(line, &q));
    assert(fabsf(q.last - 1237.00f) < 0.01f);
    assert(fabsf(q.prev_close - 1251.24f) < 0.01f);
    assert(fabsf(q.open - 1250.01f) < 0.01f);
    assert(fabsf(q.chg_pct + 1.14f) < 0.01f);
    assert(fabsf(q.high - 1256.13f) < 0.01f);
    assert(fabsf(q.low - 1231.05f) < 0.01f);
    assert(q.vol_hands == 31239);
    assert(q.stamp == 20260924161444ULL);
    assert(q.valid);
    free(raw);
}

static void test_quote_rejects_bad(void)
{
    wesr_quote_t q;
    assert(!wesr_parse_quote_line("v_sh600519=\"1~x~600519~\"", &q));      /* 字段不足 */
    assert(!wesr_parse_quote_line("v_sh600519=\"1~x~600519~0~0~0\"", &q)); /* 价格 0 */
    assert(!wesr_parse_quote_line("garbage", &q));                        /* 完全不对 */
    assert(!wesr_parse_quote_line("v_x=\"1~x~x~-1~10~10\";", &q));        /* 负价 */
}

static void test_minute(void)
{
    char *raw = read_file("test/fixtures/minute_sh600519.json");
    wesr_minute_t m;
    assert(wesr_parse_minute_json(raw, &m));
    assert(m.valid);
    assert(m.day == 20260924u);
    assert(m.n == 267);                       /* 实测点数，不是 240 */
    assert(m.pts[0].hhmm == 930);
    assert(fabsf(m.pts[0].price - 1250.01f) < 0.01f);
    assert(fabsf(m.pts[0].avg - 1250.01f) < 0.05f);   /* 首点均价 = 价格，自校验公式 */
    assert(fabsf(m.pts[0].vol - 183.0f) < 0.5f);      /* 首点累计量就是它自己的量 */
    assert(fabsf(m.pts[1].vol - (1393.0f - 183.0f)) < 0.5f);  /* 后面各点是差值 */
    assert(m.pts[m.n - 1].hhmm == 1530);
    assert(fabsf(m.pts[m.n - 1].price - 1237.00f) < 0.01f);
    free(raw);
}

static void test_minute_rejects_bad(void)
{
    wesr_minute_t m;
    assert(!wesr_parse_minute_json("", &m));
    assert(!wesr_parse_minute_json("{\"code\":-1,\"msg\":\"code param error\"}", &m));
    assert(!wesr_parse_minute_json("{\"data\":{\"sh600519\":{\"data\":{\"data\":[],\"date\":\"20260924\"}}}}",
                                   &m));   /* 空数组 = 无数据 */
    /* 累计量为 0 的点必须被跳过，不能算出 inf/nan 的均价 */
    assert(wesr_parse_minute_json(
        "{\"data\":{\"x\":{\"data\":{\"data\":[\"0930 10.00 0 0\",\"0931 10.10 5 5050.00\"],"
        "\"date\":\"20260924\"}}}}", &m));
    assert(m.n == 1);
    assert(m.pts[0].hhmm == 931);
    assert(fabsf(m.pts[0].avg - 10.10f) < 0.01f);
    assert(fabsf(m.pts[0].vol - 5.0f) < 0.01f);
}

static void test_minute_meta(void)
{
    char *raw = read_file("test/fixtures/minute_sh600519.json");
    wesr_minute_meta_t meta;
    assert(wesr_parse_minute_meta(raw, &meta));
    assert(meta.has_quote);
    assert(fabsf(meta.quote.last - 1237.00f) < 0.01f);       /* qt 数组下标 3 */
    assert(fabsf(meta.quote.prev_close - 1251.24f) < 0.01f); /* 下标 4 → 昨收 */
    assert(fabsf(meta.quote.open - 1250.01f) < 0.01f);       /* 下标 5 */
    assert(meta.closed);                                    /* 实测抓到的就是休市 */

    /* 没有 market / 没有 qt 时不崩，has_quote=false */
    assert(wesr_parse_minute_meta("{\"data\":{\"x\":{\"data\":{\"data\":[],\"date\":\"20260924\"}}}}",
                                  &meta));
    assert(!meta.has_quote);
    assert(!meta.closed);
    assert(!wesr_parse_minute_meta("", &meta));
    free(raw);
}

static void test_fmt(void)
{
    char b[32];
    wesr_fmt_price(b, sizeof b, 1682.5f, true);
    assert(strcmp(b, "1682.50") == 0);
    wesr_fmt_price(b, sizeof b, 0.0f, false);
    assert(strcmp(b, "\xE2\x80\x94") == 0);                 /* — */
    wesr_fmt_pct(b, sizeof b, 0.0f, true);
    assert(strcmp(b, "0.00%") == 0);                        /* 平盘不带箭头 */
    wesr_fmt_pct(b, sizeof b, 0.844f, true);
    assert(strcmp(b, "\xE2\x96\xB2""0.84%") == 0);          /* ▲0.84% */
    wesr_fmt_pct(b, sizeof b, -1.244f, true);
    assert(strcmp(b, "\xE2\x96\xBC""1.24%") == 0);          /* ▼1.24% */
    wesr_fmt_pct(b, sizeof b, -1.0f, false);
    assert(strcmp(b, "\xE2\x80\x94") == 0);
    assert(wesr_price_font_px(1682.5f) == 12);              /* 整数 4 位 → 降档 */
    assert(wesr_price_font_px(41.03f) == 14);
    assert(wesr_price_font_px(12345.6f) == 12);
    wesr_fmt_price(b, sizeof b, 4012.88f, true);
    assert(strcmp(b, "4012.88") == 0);
}

static void test_marks(void)
{
    char b[16];
    wesr_first_char("贵州茅台", b, sizeof b);
    assert(strcmp(b, "贵") == 0);
    wesr_first_char("hs300", b, sizeof b);
    assert(strcmp(b, "h") == 0);
    wesr_first_char("", b, sizeof b);
    assert(strcmp(b, "") == 0);

    wesr_stock_cfg_t list[4] = {
        {.code = "sh600519", .name = "贵州茅台"},
        {.code = "sz300750", .name = "宁德时代"},
        {.code = "sh601318", .name = "中国平安"},
        {.code = "sh601601", .name = "中国太保"},   /* 与上一只撞首字 */
    };
    wesr_make_marks(list, 4);
    assert(strcmp(list[0].mark, "贵") == 0);
    assert(strcmp(list[1].mark, "宁") == 0);
    assert(strcmp(list[2].mark, "中") == 0);
    assert(strcmp(list[3].mark, "中国") == 0);      /* 冲突 → 前两个字 */
}

static void test_map_and_time(void)
{
    assert(wesr_minute_index(900)  == 0);      /* 开盘前 clamp */
    assert(wesr_minute_index(925)  == 0);      /* 集合竞价点也贴左边界 */
    assert(wesr_minute_index(930)  == 0);
    assert(wesr_minute_index(1000) == 30);
    assert(wesr_minute_index(1130) == 120);
    assert(wesr_minute_index(1200) == 120);    /* 午休整段压掉 */
    assert(wesr_minute_index(1300) == 121);
    assert(wesr_minute_index(1500) == 241);
    assert(wesr_minute_index(1530) == 241);    /* 实测有 1530 点，clamp */
    assert(wesr_index_to_x(0, 0, 380) == 0);
    assert(wesr_index_to_x(241, 0, 380) == 380);
    assert(wesr_index_to_x(241 * 2, 0, 380) == 380);   /* 防溢出 */

    assert(wesr_in_trading(915));
    assert(wesr_in_trading(1000));
    assert(wesr_in_trading(1130));
    assert(!wesr_in_trading(1131));
    assert(!wesr_in_trading(1259));
    assert(wesr_in_trading(1300));
    assert(wesr_in_trading(1500));
    assert(!wesr_in_trading(1501));
    assert(!wesr_in_trading(900));
}

static void test_nav(void)
{
    wesr_nav_t n;
    wesr_nav_init(&n, 8);
    assert(n.page == 1 && n.group == 0 && n.idx == 0);
    for (int i = 0; i < 5; i++) wesr_nav_click(&n);
    assert(n.page == 2);                     /* 1→2→3→4→1→2 */

    wesr_nav_init(&n, 8);
    wesr_nav_double(&n);
    assert(n.page == 1 && n.group == 0);     /* 第 1 页双击无动作 */
    wesr_nav_click(&n); wesr_nav_click(&n);  /* 到第 3 页 */
    assert(n.page == 3);
    wesr_nav_double(&n);
    assert(n.idx == 1 && n.page == 3);       /* 第 3 页换股、不跳页 */
    for (int i = 0; i < 7; i++) wesr_nav_double(&n);
    assert(n.idx == 0);                      /* 8 只回绕 */

    wesr_nav_init(&n, 8);
    wesr_nav_click(&n);                      /* 第 2 页 */
    assert(wesr_nav_group_start(&n) == 0);
    wesr_nav_double(&n);
    assert(n.group == 1 && wesr_nav_group_start(&n) == 4);
    wesr_nav_double(&n);
    assert(n.group == 0);

    wesr_nav_init(&n, 3);                    /* 只有 3 只：组 2 不存在 */
    wesr_nav_click(&n);
    wesr_nav_double(&n);
    assert(n.group == 0);                    /* 规则 3：双击无效 */

    wesr_nav_init(&n, 8);
    wesr_nav_click(&n); wesr_nav_double(&n); /* 切到组 2 */
    wesr_nav_set_count(&n, 5);               /* 配置变小：组 2 仍有 1 只 */
    assert(n.group == 1 && wesr_nav_group_start(&n) == 4);
    wesr_nav_set_count(&n, 4);               /* 组 2 没了 */
    assert(n.group == 0);
    wesr_nav_set_count(&n, 2);               /* idx 越界要截断 */
    assert(n.idx <= 1);
    wesr_nav_set_count(&n, 0);
    assert(n.idx == 0 && n.group == 0);
    wesr_nav_double(&n);                     /* 规则 2：0 只时双击无动作 */
    assert(n.idx == 0);
}

int main(void)
{
    test_quote();
    test_quote_rejects_bad();
    test_minute();
    test_minute_rejects_bad();
    test_minute_meta();
    test_fmt();
    test_marks();
    test_map_and_time();
    test_nav();
    printf("all logic tests passed\n");
    return 0;
}
