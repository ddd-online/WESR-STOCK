#include "wesr_logic.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 取 "date":"YYYYMMDD" 里的 8 位日期 */
static bool json_day(const char *s, uint32_t *day)
{
    const char *p = strstr(s, "\"date\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    char buf[9];
    for (int i = 0; i < 8; i++) {
        if (!isdigit((unsigned char)p[i])) return false;
        buf[i] = p[i];
    }
    buf[8] = 0;
    *day = (uint32_t)strtoul(buf, NULL, 10);
    return true;
}

/* 解析 "HHMM 价格 累计量(手) 累计额(元)" 数组；均价 = 累计额 /(累计量×100) */
bool wesr_parse_minute_json(const char *json, wesr_minute_t *out)
{
    if (!json || !out) return false;
    memset(out, 0, sizeof *out);

    const char *arr = strstr(json, "\"data\":[");
    if (!arr) return false;
    if (!json_day(json, &out->day)) return false;
    arr = strchr(arr, '[');
    if (!arr) return false;
    arr++;

    const char *p = arr;
    uint16_t n = 0;
    float prev_cum = 0.0f;
    while (*p && *p != ']' && n < WESR_MAX_POINTS) {
        if (*p != '"') { p++; continue; }
        p++;
        unsigned hh = 0, mm = 0;
        float price = 0, cum_vol = 0, cum_amt = 0;
        if (sscanf(p, "%2u%2u %f %f %f", &hh, &mm, &price, &cum_vol, &cum_amt) == 5 &&
            price > 0.0f && cum_vol > 0.0f && isfinite(cum_amt)) {
            out->pts[n].hhmm = (uint16_t)(hh * 100u + mm);
            out->pts[n].price = price;
            out->pts[n].avg = cum_amt / (cum_vol * 100.0f);   /* 量单位是手 */
            out->pts[n].vol = cum_vol - prev_cum;             /* 本分钟量 = 累计差值 */
            prev_cum = cum_vol;
            n++;
        }
        const char *e = strchr(p, '"');
        if (!e) break;
        p = e + 1;
    }
    out->n = n;
    out->valid = (n > 0);
    return out->valid;
}
