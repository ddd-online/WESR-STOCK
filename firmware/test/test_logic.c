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

int main(void)
{
    test_quote();
    test_quote_rejects_bad();
    printf("all logic tests passed\n");
    return 0;
}
