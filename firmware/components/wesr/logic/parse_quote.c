#include "wesr_logic.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 从 v_xxx="a~b~c..."; 取第 idx 个字段（0 起）。中文名在第 1 个字段，我们不取它，所以不涉编码。 */
static bool field(const char *line, int idx, char *out, size_t cap)
{
    const char *p = strchr(line, '"');
    if (!p) return false;
    p++;
    for (int i = 0; i < idx; i++) {
        p = strchr(p, '~');
        if (!p) return false;
        p++;
    }
    const char *e = p;
    while (*e && *e != '~' && *e != '"') e++;
    size_t len = (size_t)(e - p);
    if (len == 0 || len >= cap) return false;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

static bool fnum(const char *line, int idx, float *out)
{
    char buf[40];
    if (!field(line, idx, buf, sizeof buf)) return false;
    char *end = NULL;
    float v = strtof(buf, &end);
    if (end == buf || !isfinite(v)) return false;
    *out = v;
    return true;
}

static uint64_t uint_field(const char *line, int idx)
{
    char buf[40];
    if (!field(line, idx, buf, sizeof buf)) return 0;
    return strtoull(buf, NULL, 10);
}

bool wesr_parse_quote_line(const char *line, wesr_quote_t *q)
{
    if (!line || !q) return false;
    memset(q, 0, sizeof *q);
    float last = 0, prev = 0;
    if (!fnum(line, 3, &last) || !fnum(line, 4, &prev)) return false;
    if (!(last > 0) || !(prev > 0)) return false;      /* 停牌/异常 */
    q->last = last;
    q->prev_close = prev;
    if (!fnum(line, 5, &q->open))  q->open = last;
    if (!fnum(line, 31, &q->chg))  q->chg = last - prev;
    if (!fnum(line, 32, &q->chg_pct)) q->chg_pct = (last - prev) / prev * 100.0f;
    if (!fnum(line, 33, &q->high)) q->high = last;
    if (!fnum(line, 34, &q->low))  q->low = last;
    q->vol_hands = (uint32_t)uint_field(line, 36);
    q->stamp = uint_field(line, 30);
    q->valid = true;
    return true;
}
