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
    assert(meta.quote.vol_hands == 31239);                    /* 下标 36：成交量(手) */
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
    assert(wesr_nav_minute_key(&n) == 0);    /* 第 2 页组 1 → 分时缓存的归属是第 0 只 */
    wesr_nav_double(&n);
    assert(n.group == 1 && wesr_nav_group_start(&n) == 4);
    /* 组 2 的归属必须是 4（不是 0）——休市补数据的 need 全靠这个值判断，
       之前只看 valid 导致第 2 组永远不补拉，分时被画成直线 */
    assert(wesr_nav_minute_key(&n) == 4);
    wesr_nav_double(&n);
    assert(n.group == 0);

    wesr_nav_init(&n, 8);
    wesr_nav_click(&n); wesr_nav_click(&n);  /* 第 3 页 */
    assert(wesr_nav_minute_key(&n) == 0);    /* 第 3 页看 idx 那一只 */
    wesr_nav_double(&n);
    assert(wesr_nav_minute_key(&n) == 1);
    for (int i = 0; i < 6; i++) wesr_nav_double(&n);
    assert(n.idx == 7 && wesr_nav_minute_key(&n) == 7);   /* 8 只：最后一只是 7 */

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

    /* 量柱：sqrt 映射。线性的话 10/30 只有 6px（整排看着像一条线），sqrt 给 11px；
       实测数据中位数只有满量的 7%，线性必然压成一条线。 */
    {
        int col[3] = { o.pad_x, wesr_index_to_x(wesr_minute_index(1000), o.pad_x, 380 - o.pad_x),
                       380 - o.pad_x };
        int bars[3] = { 0, 0, 0 };
        for (int k = 0; k < 3; k++) {
            for (int y = o.vol_top; y < o.vol_top + o.vol_h; y++) {
                if (wesr_bmp_get(&b, col[k], y)) bars[k]++;
            }
        }
        assert(bars[0] >= 10 && bars[0] <= 12);   /* sqrt(10/30)*20 = 11.5 */
        assert(bars[1] == o.vol_h);               /* 满量 = 满高 */
        assert(bars[2] >= 15 && bars[2] <= 17);   /* sqrt(20/30)*20 = 16.3 */
    }

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
    assert(strcmp(cfg.stocks[0].code, "sh603936") == 0);
    assert(strcmp(cfg.stocks[0].mark, "博") == 0);       /* 首字自动取 */
    assert(strcmp(cfg.stocks[7].code, "sz002815") == 0);
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

static void test_weekday(void)
{
    assert(wesr_weekday(2026, 9, 27) == 0);   /* 2026-09-27 周日 */
    assert(wesr_weekday(2026, 9, 26) == 6);   /* 周六 */
    assert(wesr_weekday(2026, 1, 6) == 2);    /* 周二 */
    assert(wesr_weekday(2000, 2, 29) == 2);   /* 闰日 */
    assert(wesr_weekday(2024, 3, 1) == 5);    /* 周五 */
}

/* BLE 配网协议：分片重组 + 报文构造 + 股票列表校验（spec §10） */
static void test_ble_proto(void)
{
    /* 1) 分片重组：20 字节一片，攒到 '\n' 才成一条 */
    wesr_ble_rx_t rx; wesr_ble_rx_reset(&rx);
    char msg[256];
    const char *s = "{\"cmd\":\"setWifi\",\"ssid\":\"waveware_private\",\"pass\":\"12345678\"}\n";
    uint16_t total = (uint16_t)strlen(s), sent = 0;
    bool got = false;
    while (total - sent > 0) {
        uint16_t n = wesr_ble_chunk_len(total, sent);
        got = wesr_ble_rx_push(&rx, (const uint8_t *)s + sent, n);
        sent += n;
    }
    assert(got);                                     /* 最后一包才凑齐 */
    assert(wesr_ble_rx_take(&rx, msg, sizeof msg) == strlen(s) - 1);
    assert(strcmp(msg, "{\"cmd\":\"setWifi\",\"ssid\":\"waveware_private\",\"pass\":\"12345678\"}") == 0);
    assert(wesr_ble_rx_take(&rx, msg, sizeof msg) == 0);   /* 取空了 */

    /* 2) 一次写进两条报文：要能逐条取出 */
    wesr_ble_rx_reset(&rx);
    const char *two = "{\"cmd\":\"hello\"}\n{\"cmd\":\"scan\"}\n";
    assert(wesr_ble_rx_push(&rx, (const uint8_t *)two, (uint16_t)strlen(two)));
    assert(wesr_ble_rx_take(&rx, msg, sizeof msg) == 15 && strcmp(msg, "{\"cmd\":\"hello\"}") == 0);
    assert(wesr_ble_rx_take(&rx, msg, sizeof msg) == 14 && strcmp(msg, "{\"cmd\":\"scan\"}") == 0);

    /* 3) 超长（>4KB）不含 '\n'：丢掉这条，在下一个 '\n' 处重新同步 */
    wesr_ble_rx_reset(&rx);
    uint8_t junk[64]; memset(junk, 'x', sizeof junk);
    for (int i = 0; i < 100; i++)
        assert(!wesr_ble_rx_push(&rx, junk, sizeof junk));    /* 6400 > 4096，全程没报文 */
    assert(rx.len == 0 && rx.drop);
    assert(!wesr_ble_rx_push(&rx, (const uint8_t *)"tail\n", 5));  /* 残片尾在 '\n' 处丢掉 */
    assert(!rx.drop);
    assert(wesr_ble_rx_push(&rx, (const uint8_t *)"{\"cmd\":\"hello\"}\n", 16));
    assert(wesr_ble_rx_take(&rx, msg, sizeof msg) == 15);

    /* 4) 分片长度：整条 41 字节 → 20/20/1 */
    assert(wesr_ble_chunk_len(41, 0) == 20);
    assert(wesr_ble_chunk_len(41, 20) == 20);
    assert(wesr_ble_chunk_len(41, 40) == 1);
    assert(wesr_ble_chunk_len(41, 41) == 0);
    assert(wesr_ble_chunk_len(0, 0) == 0);

    /* 5) 取值：字符串里的冒号/逗号不能截断，键不能撞（name vs nickname） */
    const char *j = "{\"cmd\":\"setWifi\",\"ssid\":\"a:b,c\",\"pass\":\"p\\\"q\"}";
    char v[64]; long iv;
    assert(wesr_ble_get_str(j, "ssid", v, sizeof v) && strcmp(v, "a:b,c") == 0);
    assert(wesr_ble_get_str(j, "pass", v, sizeof v) && strcmp(v, "p\"q") == 0);
    assert(!wesr_ble_get_str(j, "ss", v, sizeof v));      /* 不做前缀匹配 */
    assert(wesr_ble_cmd_is(j, "setWifi") && !wesr_ble_cmd_is(j, "set"));
    const char *k = "{\"cmd\":\"setInterval\",\"sec\":30}";
    assert(wesr_ble_get_int(k, "sec", &iv) && iv == 30);
    assert(!wesr_ble_get_int(k, "no_such", &iv));         /* 缺字段要报错，别默认 0 */

    /* 6) 股票代码校验 */
    assert(wesr_code_valid("sh600519") && wesr_code_valid("sz000858") && wesr_code_valid("bj430047"));
    assert(!wesr_code_valid("600519") && !wesr_code_valid("sh60051") && !wesr_code_valid("sh6005199"));
    assert(!wesr_code_valid("SH600519") && !wesr_code_valid(""));

    /* 7) setStocks：3 只，mark 缺省时自动取名字第一个字 */
    wesr_stock_cfg_t list[WESR_MAX_STOCKS]; const char *err = NULL;
    const char *items = "{\"cmd\":\"setStocks\",\"items\":["
        "{\"code\":\"sh600519\",\"name\":\"贵州茅台\"},"
        "{\"code\":\"sz000858\",\"name\":\"五粮液\",\"mark\":\"酒\"},"
        "{\"code\":\"sh000300\",\"name\":\"沪深300\"}]}";
    int n = wesr_ble_parse_items(items, list, WESR_MAX_STOCKS, &err);
    assert(n == 3 && err == NULL);
    assert(strcmp(list[0].code, "sh600519") == 0 && strcmp(list[0].mark, "贵") == 0);
    assert(strcmp(list[1].mark, "酒") == 0);             /* 给了 mark 就用给的 */
    assert(strcmp(list[2].name, "沪深300") == 0);

    /* 8) setStocks 出错：代码格式 / 超过 8 只 / 缺 items */
    assert(wesr_ble_parse_items("{\"items\":[{\"code\":\"600519\",\"name\":\"x\"}]}",
                                list, WESR_MAX_STOCKS, &err) == -1 && strcmp(err, "E_CODE_FMT") == 0);
    assert(wesr_ble_parse_items(
        "{\"items\":[{\"code\":\"sh600519\",\"name\":\"a\"},{\"code\":\"sh600520\",\"name\":\"b\"},"
        "{\"code\":\"sh600521\",\"name\":\"c\"},{\"code\":\"sh600522\",\"name\":\"d\"},"
        "{\"code\":\"sh600523\",\"name\":\"e\"},{\"code\":\"sh600524\",\"name\":\"f\"},"
        "{\"code\":\"sh600525\",\"name\":\"g\"},{\"code\":\"sh600526\",\"name\":\"h\"},"
        "{\"code\":\"sh600527\",\"name\":\"i\"}]}",
        list, WESR_MAX_STOCKS, &err) == -1 && strcmp(err, "E_TOO_MANY") == 0);
    assert(wesr_ble_parse_items("{\"cmd\":\"setStocks\"}", list, WESR_MAX_STOCKS, &err) == -1
           && strcmp(err, "E_ARG") == 0);

    /* 9) 回包：关键字段在不在、长度对不对 */
    char out[512];
    wesr_ble_status_t st = { "0.1.0", "A0B1C2D3E4F5", "waveware_private", "10.12.254.79",
                             "connected", 96, 4090, -52, 8, 2, 1, 0 };
    int len = wesr_ble_fmt_status(out, sizeof out, &st);
    assert(len > 0 && len <= WESR_BLE_MSG_MAX);
    assert(strstr(out, "\"ev\":\"status\"") && strstr(out, "\"batt_pct\":96"));
    assert(strstr(out, "\"rssi\":-52") && strstr(out, "\"cfg_count\":8"));
    assert(!strstr(out, "pass"));                        /* 密码绝不出现在回包 */

    assert(wesr_ble_fmt_ack(out, sizeof out, "setWifi") > 0
           && strstr(out, "\"ev\":\"ack\"") && strstr(out, "\"cmd\":\"setWifi\""));
    assert(wesr_ble_fmt_err(out, sizeof out, "E_WIFI", "connect failed") > 0
           && strstr(out, "E_WIFI"));

    wesr_ap_t aps[2] = { { "waveware_private", -52 }, { "TP-LINK_5G", -71 } };
    assert(wesr_ble_fmt_scan(out, sizeof out, aps, 2) > 0
           && strstr(out, "\"ev\":\"scanResult\"") && strstr(out, "\"ssid\":\"TP-LINK_5G\""));

    wesr_app_cfg_t cfg; wesr_cfg_defaults(&cfg);
    cfg.count = 2;
    snprintf(cfg.stocks[0].code, sizeof cfg.stocks[0].code, "sh600519");
    snprintf(cfg.stocks[0].name, sizeof cfg.stocks[0].name, "贵州茅台");
    snprintf(cfg.stocks[0].mark, sizeof cfg.stocks[0].mark, "贵");
    snprintf(cfg.stocks[1].code, sizeof cfg.stocks[1].code, "sz000858");
    snprintf(cfg.stocks[1].name, sizeof cfg.stocks[1].name, "五粮液");
    snprintf(cfg.stocks[1].mark, sizeof cfg.stocks[1].mark, "酒");
    assert(wesr_ble_fmt_cfg(out, sizeof out, &cfg) > 0 && strstr(out, "\"ev\":\"cfg\""));
    assert(strstr(out, "\"code\":\"sz000858\"") && !strstr(out, "\"pass\""));

    /* 10) 目标缓冲太小：宁可返回 -1，也不许截断出半条 JSON */
    assert(wesr_ble_fmt_status(out, 20, &st) == -1);
    assert(wesr_ble_fmt_cfg(out, 20, &cfg) == -1);

    printf("  ble_proto ok\n");
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
    test_weekday();
    test_nav();
    test_sched();
    test_bmp_primitives();
    test_chart_render();
    test_defaults_and_code();
    test_ble_proto();
    printf("all logic tests passed\n");
    return 0;
}
