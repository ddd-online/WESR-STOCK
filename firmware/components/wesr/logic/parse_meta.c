#include "wesr_logic.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 在第 [start,end) 里找第 idx 个 JSON 字符串，写进 out */
static bool array_str(const char *start, const char *end, int idx, char *out, size_t cap)
{
    const char *p = start;
    for (int i = 0; i <= idx; i++) {
        p = strchr(p, '"');
        if (!p || p >= end) return false;
        p++;
        const char *e = strchr(p, '"');
        if (!e || e > end) return false;
        if (i == idx) {
            size_t len = (size_t)(e - p);
            if (len >= cap) return false;
            memcpy(out, p, len);
            out[len] = 0;
            return true;
        }
        p = e + 1;
    }
    return false;
}

static bool arr_f(const char *s, const char *e, int idx, float *out)
{
    char buf[40];
    if (!array_str(s, e, idx, buf, sizeof buf)) return false;
    char *endp = NULL;
    float v = strtof(buf, &endp);
    if (endp == buf || !isfinite(v)) return false;
    *out = v;
    return true;
}

/* 分时响应里自带快照数组（qt.<code>）与市场状态（qt.market），省掉一次请求 */
bool wesr_parse_minute_meta(const char *json, wesr_minute_meta_t *meta)
{
    /* 空/NULL 视为非法输入；能扫但没带 qt/market 的响应不算错（返回 true，字段留空） */
    if (!json || !json[0] || !meta) return false;
    memset(meta, 0, sizeof *meta);

    /* 快照：qt 段里第一对"能解析出快照"的 [ ]。
       注意实测响应里 "v_ff_sh600519":[] 这个空数组排在最前面，不能直接取第一个 [。 */
    const char *qt = strstr(json, "\"qt\"");
    if (qt) {
        const char *p = qt;
        while ((p = strchr(p, '[')) != NULL) {
            const char *e = strchr(p, ']');
            if (!e) break;
            float last = 0, prev = 0;
            if (arr_f(p, e, 3, &last) && arr_f(p, e, 4, &prev) &&
                last > 0 && prev > 0) {
                meta->quote.last = last;
                meta->quote.prev_close = prev;
                arr_f(p, e, 5, &meta->quote.open);
                arr_f(p, e, 31, &meta->quote.chg);
                arr_f(p, e, 32, &meta->quote.chg_pct);
            arr_f(p, e, 33, &meta->quote.high);
            arr_f(p, e, 34, &meta->quote.low);
            float volf = 0;
            if (arr_f(p, e, 36, &volf)) meta->quote.vol_hands = (uint32_t)volf;   /* 成交量(手) */
            meta->quote.valid = true;
                meta->has_quote = true;
                break;
            }
            p = e + 1;
        }
    }

    /* 市场状态："market":[ "...|SH_close_...|..." ]。
       这串有 25 个市场、约 600 字节，不要复制到定长缓冲（会截断），直接在区间内扫。 */
    const char *mk = strstr(json, "\"market\"");
    if (mk) {
        const char *b = strchr(mk, '[');
        const char *e = b ? strchr(b, ']') : NULL;
        if (b && e) {
            const char *hit = NULL;
            for (const char *q = b; q + 4 <= e; q++) {
                if (memcmp(q, "|SH_", 4) == 0 || memcmp(q, "|SZ_", 4) == 0) {
                    hit = q;
                    break;
                }
            }
            if (hit && hit + 4 + 5 <= e) meta->closed = (strncmp(hit + 4, "close", 5) == 0);
        }
    }
    return true;
}
