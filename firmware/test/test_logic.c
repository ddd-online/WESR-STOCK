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
    assert(strcmp(b, "\xE2\x96\xB2"" 0.84%") == 0);         /* ▲ 0.84%（带空格） */
    wesr_fmt_pct(b, sizeof b, -1.244f, true);
    assert(strcmp(b, "\xE2\x96\xBC"" 1.24%") == 0);         /* ▼ 1.24% */
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

static void test_sched(void)
{
    wesr_sched_t s;
    wesr_sched_init(&s, 15);
    assert(wesr_sched_tick(&s, 0, true) == WESR_FETCH_QUOTES);   /* 第 1 页到点 */
    wesr_sched_result(&s, true, 0);
    assert(wesr_sched_tick(&s, 1000, true) == WESR_FETCH_NONE);  /* 未到 15s */
    assert(wesr_sched_tick(&s, 15000, true) == WESR_FETCH_QUOTES);

    wesr_sched_result(&s, false, 15000);                          /* 失败 → 5s 退避 */
    assert(s.fail_streak == 1 && !s.offline);
    assert(wesr_sched_tick(&s, 19999, true) == WESR_FETCH_NONE);
    assert(wesr_sched_tick(&s, 20000, true) == WESR_FETCH_QUOTES);
    wesr_sched_result(&s, false, 20000);                          /* 10s */
    assert(wesr_sched_tick(&s, 29999, true) == WESR_FETCH_NONE);
    assert(wesr_sched_tick(&s, 30000, true) == WESR_FETCH_QUOTES);
    wesr_sched_result(&s, false, 30000);                          /* 20s */
    assert(wesr_sched_tick(&s, 50000, true) == WESR_FETCH_QUOTES);
    wesr_sched_result(&s, false, 50000);                          /* 30s，封顶 */
    assert(wesr_sched_tick(&s, 80000, true) == WESR_FETCH_QUOTES);
    wesr_sched_result(&s, false, 80000);                          /* 第 5 次失败 */
    assert(s.offline);
    assert(wesr_sched_tick(&s, 110000, true) == WESR_FETCH_QUOTES);
    wesr_sched_result(&s, true, 110000);                          /* 恢复 */
    assert(!s.offline && s.fail_streak == 0);
    assert(wesr_sched_tick(&s, 125000, true) == WESR_FETCH_QUOTES); /* 回到 15s */

    wesr_sched_page(&s, 3);                                       /* 切页立刻可拉 */
    assert(wesr_sched_tick(&s, 125001, true) == WESR_FETCH_MINUTES);
    wesr_sched_result(&s, true, 125001);
    wesr_sched_page(&s, 4);
    assert(wesr_sched_tick(&s, 125002, true) == WESR_FETCH_NONE); /* 第 4 页不发请求 */

    wesr_sched_page(&s, 1);
    assert(wesr_sched_tick(&s, 200000, false) == WESR_FETCH_NONE); /* 休市不请求 */
    assert(wesr_sched_tick(&s, 210000, true) == WESR_FETCH_NONE);  /* 休市那次把下次检查推到 260000 */
    assert(wesr_sched_tick(&s, 260000, true) == WESR_FETCH_QUOTES);
}

static void test_bmp_primitives(void)
{
    uint8_t buf[8 * 2] = {0};
    wesr_bmp_t b;
    wesr_bmp_init(&b, buf, 16, 8);
    assert(b.stride == 2);
    wesr_bmp_px(&b, 0, 0);
    assert(wesr_bmp_get(&b, 0, 0));
    assert(buf[0] == 0x80);                       /* MSB 在左 */
    wesr_bmp_px(&b, 15, 7);
    assert(wesr_bmp_get(&b, 15, 7));
    assert(buf[15] == 0x01);                      /* y=7 → 第 7 行第 2 字节，x=15 是它的最低位 */
    wesr_bmp_clear(&b);
    assert(!wesr_bmp_get(&b, 0, 0));

    wesr_bmp_hline(&b, 2, 5, 3);
    for (int x = 2; x <= 5; x++) assert(wesr_bmp_get(&b, x, 3));
    assert(!wesr_bmp_get(&b, 6, 3));
    wesr_bmp_clear(&b);

    wesr_bmp_line(&b, 0, 0, 7, 7);                 /* 对角线 */
    for (int i = 0; i <= 7; i++) assert(wesr_bmp_get(&b, i, i));
    wesr_bmp_clear(&b);

    wesr_bmp_dash_hline(&b, 0, 9, 5, 3, 2);        /* on 3 off 2 */
    for (int x = 0; x < 10; x++) {
        bool want = (x % 5) < 3;
        assert(wesr_bmp_get(&b, x, 5) == want);
    }
    wesr_bmp_clear(&b);

    wesr_bmp_vbar(&b, 3, 2, 4, 7);
    assert(wesr_bmp_get(&b, 3, 4) && wesr_bmp_get(&b, 4, 7));
    assert(!wesr_bmp_get(&b, 5, 4));
}

static void test_chart_render(void)
{
    static uint8_t buf[(380 * 168 + 7) / 8];
    wesr_bmp_t b;
    wesr_bmp_init(&b, buf, 380, 168);

    wesr_minute_t m = {0};
    m.n = 3;
    m.day = 20260924u;
    m.valid = true;
    m.pts[0] = (wesr_point_t){ .hhmm = 930,  .price = 10.00f, .avg = 10.00f, .vol = 10 };
    m.pts[1] = (wesr_point_t){ .hhmm = 1000, .price = 10.50f, .avg = 10.20f, .vol = 30 };
    m.pts[2] = (wesr_point_t){ .hhmm = 1500, .price = 11.00f, .avg = 10.50f, .vol = 20 };

    wesr_chart_opts_t o = { .pad_top = 8, .pad_bottom = 4, .pad_x = 2,
                            .price_h = 116, .vol_top = 130, .vol_h = 20,
                            .hatch = false, .grid = false, .span_ratio = 1.1f };
    wesr_chart_render(&b, &m, 10.00f, &o);

    /* 昨收基准线（10.00）在价格区里像素最多的那一行，且必须是虚线（有断开） */
    int y_base = -1;
    for (int y = o.pad_top; y < o.price_h; y++) {
        int cnt = 0;
        for (int x = 0; x < 380; x++) if (wesr_bmp_get(&b, x, y)) cnt++;
        if (cnt > 100) { y_base = y; break; }
    }
    assert(y_base > 0);
    int gaps = 0;
    for (int x = 1; x < 380; x++) {
        if (wesr_bmp_get(&b, x - 1, y_base) && !wesr_bmp_get(&b, x, y_base)) gaps++;
    }
    assert(gaps > 20);

    /* 末点 1500 落在右边界那一列（x = w - pad_x） */
    int last_col = 0;
    for (int y = o.pad_top; y < o.price_h; y++) {
        if (wesr_bmp_get(&b, 380 - o.pad_x, y)) last_col++;
    }
    assert(last_col > 0);

    /* 不开斜纹时，价格线下方、基准线上方的区域应当是空的 */
    int empty = 0;
    for (int x = 10; x < 300; x++)
        for (int y = y_base + 2; y < y_base + 20; y++)
            if (!wesr_bmp_get(&b, x, y)) empty++;
    assert(empty > 1000);

    /* 成交量条区域有像素 */
    int vol_px = 0;
    for (int x = 0; x < 380; x++)
        for (int y = o.vol_top; y < o.vol_top + o.vol_h; y++)
            if (wesr_bmp_get(&b, x, y)) vol_px++;
    assert(vol_px > 0);

    /* 只有 3 个点也不崩：右侧大片留白（边界规则 9） */
    wesr_bmp_clear(&b);
    o.hatch = true;
    wesr_chart_render(&b, &m, 10.00f, &o);
    int right_empty = 0;
    for (int x = 360; x < 380; x++)
        for (int y = o.pad_top; y < o.price_h; y++)
            if (!wesr_bmp_get(&b, x, y)) right_empty++;
    assert(right_empty > 100);

    /* 斜纹开了以后，价格线上方的区域被斜纹填了一部分但不全填 */
    int filled = 0, total = 0;
    for (int x = 20; x < 200; x++) {
        for (int y = o.pad_top; y < y_base; y++) {
            if (y < o.pad_top + 4) continue;
            total++;
            if (wesr_bmp_get(&b, x, y)) filled++;
        }
    }
    assert(filled > 0 && filled < total);

    /* prev_close = 0 时不能崩：退化成不画 */
    wesr_bmp_clear(&b);
    wesr_chart_render(&b, &m, 0.0f, &o);
}

static void test_defaults_and_code(void)
{
    wesr_app_cfg_t cfg;
    wesr_cfg_defaults(&cfg);
    assert(cfg.count == 8);
    assert(cfg.refresh_sec == 15);
    assert(strcmp(cfg.stocks[0].code, "sh600519") == 0);
    assert(strcmp(cfg.stocks[0].mark, "贵") == 0);
    for (int i = 0; i < cfg.count; i++) assert(wesr_code_valid(cfg.stocks[i].code));

    assert(wesr_code_valid("sh600519"));
    assert(wesr_code_valid("sz300750"));
    assert(wesr_code_valid("bj430047"));
    assert(!wesr_code_valid("600519"));      /* 缺市场前缀 */
    assert(!wesr_code_valid("sh60051"));     /* 位数不够 */
    assert(!wesr_code_valid("us600519"));    /* 不支持的市场 */
    assert(!wesr_code_valid(""));
    assert(!wesr_code_valid(NULL));
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
    test_sched();
    test_bmp_primitives();
    test_chart_render();
    test_defaults_and_code();
    printf("all logic tests passed\n");
    return 0;
}
