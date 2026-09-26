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

int main(void)
{
    test_quote();
    test_quote_rejects_bad();
    test_minute();
    test_minute_rejects_bad();
    printf("all logic tests passed\n");
    return 0;
}
