/* BLE 配网协议的编解码（spec §10）。纯 C：不 include 任何 ESP-IDF 头，
   宿主机 test/test_logic.c 直接编进去跑断言。
   协议：UTF-8 JSON + '\n' 结尾；双向固定 ≤20 字节分片；单条上限 4KB。 */
#include "wesr_logic.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 接收：字节流 → 整条报文 ---------------- */

void wesr_ble_rx_reset(wesr_ble_rx_t *rx)
{
    rx->len = 0;
    rx->drop = false;
    rx->buf[0] = 0;
}

uint16_t wesr_ble_chunk_len(uint16_t total, uint16_t sent)
{
    if (sent >= total) return 0;
    uint16_t left = (uint16_t)(total - sent);
    return left > WESR_BLE_CHUNK ? WESR_BLE_CHUNK : left;
}

/* 攒字节。缓冲里是一串以 '\n' 分隔的完整报文 —— '\n' 要留着，
   否则 take 无法知道第一条到哪里结束。
   超 4KB 还没换行 = 这条报文不要了：丢掉后面所有字节直到下一个 '\n'
   （不能直接原地清空就了事，否则残片的尾巴会粘到下一条报文头上）。 */
bool wesr_ble_rx_push(wesr_ble_rx_t *rx, const uint8_t *d, uint16_t n)
{
    bool has_line = false;
    for (uint16_t i = 0; i < n; i++) {
        char c = (char)d[i];
        if (rx->drop) {
            if (c == '\n') rx->drop = false;  /* 残片到此为止，下一条从头开始 */
            continue;
        }
        if (rx->len >= WESR_BLE_MSG_MAX) {    /* 超限：丢本次会话 */
            rx->len = 0;
            rx->drop = true;
            if (c == '\n') rx->drop = false;
            continue;
        }
        rx->buf[rx->len++] = c;
        if (c == '\n') has_line = true;
    }
    rx->buf[rx->len] = 0;
    return has_line;
}

uint16_t wesr_ble_rx_take(wesr_ble_rx_t *rx, char *out, uint16_t cap)
{
    for (;;) {
        if (rx->len == 0) return 0;
        const char *nl = (const char *)memchr(rx->buf, '\n', rx->len);
        if (!nl) return 0;
        uint16_t len = (uint16_t)(nl - rx->buf);
        uint16_t rest = (uint16_t)(rx->len - len - 1);
        if (len > 0 && len + 1 <= cap) {
            memcpy(out, rx->buf, len);
            out[len] = 0;
            memmove(rx->buf, nl + 1, rest);
            rx->len = rest;
            rx->buf[rx->len] = 0;
            return len;
        }
        /* 空行 / 调用者缓冲太小：丢掉这一条继续找下一条 */
        memmove(rx->buf, nl + 1, rest);
        rx->len = rest;
        rx->buf[rx->len] = 0;
    }
}

/* ---------------- 极简 JSON 取值 ---------------- */

/* 找 "key" 后面的值起点。带引号和冒号一起找，所以 name 不会命中 nickname；
   键名撞上但不是键（后面不是冒号）时继续往后找。 */
static const char *find_key(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    char pat[64];
    int pl = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (pl <= 0 || pl >= (int)sizeof pat) return NULL;
    const char *p = json;
    for (;;) {
        p = strstr(p, pat);
        if (!p) return NULL;
        const char *q = p + pl;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            return q;
        }
        p += pl;
    }
}

bool wesr_ble_get_str(const char *json, const char *key, char *out, uint16_t cap)
{
    const char *p = find_key(json, key);
    if (!p || *p != '"' || cap == 0) return false;
    p++;
    uint16_t n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p;
            if (!e) return false;
            p++;
            if (e == 'n')      c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') c = '\r';
            else               c = e;      /* \" \\ \/ 等原样 */
        }
        if ((uint16_t)(n + 2) > cap) return false;   /* 放不下：报失败，不截断 */
        out[n++] = c;
    }
    if (*p != '"') return false;
    out[n] = 0;
    return true;
}

bool wesr_ble_get_int(const char *json, const char *key, long *out)
{
    const char *p = find_key(json, key);
    if (!p) return false;
    if (*p != '-' && !isdigit((unsigned char)*p)) return false;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end && *end != ',' && *end != '}') return false;
    if (out) *out = v;
    return true;
}

bool wesr_ble_cmd_is(const char *json, const char *cmd)
{
    char v[24];
    return wesr_ble_get_str(json, "cmd", v, sizeof v) && strcmp(v, cmd) == 0;
}

/* ---------------- setStocks 的 items 校验 ---------------- */

int wesr_ble_parse_items(const char *json, wesr_stock_cfg_t *out, uint8_t max,
                         const char **err)
{
    if (err) *err = NULL;
    const char *p = find_key(json, "items");
    if (!p || *p != '[') {
        if (err) *err = WESR_BLE_E_ARG;
        return -1;
    }
    p++;
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '{') {
            if (err) *err = WESR_BLE_E_ARG;
            return -1;
        }
        const char *e = strchr(p, '}');       /* 我们自己的报文集：对象里不嵌套对象 */
        if (!e) {
            if (err) *err = WESR_BLE_E_ARG;
            return -1;
        }
        size_t on = (size_t)(e - p) + 1;
        char obj[192];
        if (on >= sizeof obj) {               /* 单个对象比缓冲还大：一定是脏数据 */
            if (err) *err = WESR_BLE_E_ARG;
            return -1;
        }
        memcpy(obj, p, on);
        obj[on] = 0;
        p = e + 1;

        if (n >= max) {
            if (err) *err = WESR_BLE_E_TOO_MANY;
            return -1;
        }
        wesr_stock_cfg_t *s = &out[n];
        memset(s, 0, sizeof *s);
        if (!wesr_ble_get_str(obj, "code", s->code, sizeof s->code) ||
            !wesr_code_valid(s->code)) {
            if (err) *err = WESR_BLE_E_CODE_FMT;
            return -1;
        }
        if (!wesr_ble_get_str(obj, "name", s->name, sizeof s->name) || !s->name[0]) {
            if (err) *err = WESR_BLE_E_ARG;
            return -1;
        }
        /* 首字：小程序没给就自动取名字第一个字（spec §11） */
        if (!wesr_ble_get_str(obj, "mark", s->mark, sizeof s->mark) || !s->mark[0])
            wesr_first_char(s->name, s->mark, sizeof s->mark);
        n++;
    }
    if (n == 0) {
        if (err) *err = WESR_BLE_E_ARG;
        return -1;
    }
    return n;
}

/* ---------------- 回包构造 ---------------- */

/* 宁可返回 -1 也不给半条 JSON：调用者的缓冲装不下就让它去改缓冲，别偷偷截断 */
static int finish(char *out, uint16_t cap, const char *tmp, int tmpsz, int len)
{
    if (len < 0 || len >= tmpsz) return -1;          /* snprintf 被截断了 */
    if ((uint16_t)len + 1 > cap) return -1;
    memcpy(out, tmp, (size_t)len + 1);
    return len;
}

int wesr_ble_fmt_status(char *out, uint16_t cap, const wesr_ble_status_t *s)
{
    char t[512];
    int n = snprintf(t, sizeof t,
        "{\"ev\":\"status\",\"fw\":\"%s\",\"mac\":\"%s\",\"batt_pct\":%d,\"batt_mv\":%d,"
        "\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"wifi\":\"%s\",\"cfg_count\":%u,"
        "\"page\":%u,\"group\":%u,\"idx\":%u}",
        s->fw ? s->fw : "", s->mac ? s->mac : "",
        s->batt_pct, s->batt_mv,
        s->ssid ? s->ssid : "", s->ip ? s->ip : "", s->rssi,
        s->wifi_state ? s->wifi_state : "idle",
        (unsigned)s->cfg_count, (unsigned)s->page, (unsigned)s->group, (unsigned)s->idx);
    return finish(out, cap, t, (int)sizeof t, n);
}

int wesr_ble_fmt_ack(char *out, uint16_t cap, const char *cmd)
{
    char t[320];
    int n = snprintf(t, sizeof t, "{\"ev\":\"ack\",\"cmd\":\"%s\"}", cmd ? cmd : "");
    return finish(out, cap, t, (int)sizeof t, n);
}

int wesr_ble_fmt_err(char *out, uint16_t cap, const char *code, const char *msg)
{
    char t[320];
    int n = snprintf(t, sizeof t, "{\"ev\":\"err\",\"code\":\"%s\",\"msg\":\"%s\"}",
                     code ? code : WESR_BLE_E_ARG, msg ? msg : "");
    return finish(out, cap, t, (int)sizeof t, n);
}

int wesr_ble_fmt_scan(char *out, uint16_t cap, const wesr_ap_t *aps, uint8_t n)
{
    char t[1024];
    int len = snprintf(t, sizeof t, "{\"ev\":\"scanResult\",\"aps\":[");
    if (len < 0 || len >= (int)sizeof t) return -1;
    for (uint8_t i = 0; i < n; i++) {
        int w = snprintf(t + len, sizeof t - (size_t)len, "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                         i ? "," : "", aps[i].ssid, (int)aps[i].rssi);
        if (w < 0 || w >= (int)(sizeof t - (size_t)len)) return -1;
        len += w;
    }
    int w = snprintf(t + len, sizeof t - (size_t)len, "]}");
    if (w < 0 || w >= (int)(sizeof t - (size_t)len)) return -1;
    len += w;
    return finish(out, cap, t, (int)sizeof t, len);
}

int wesr_ble_fmt_cfg(char *out, uint16_t cap, const wesr_app_cfg_t *cfg)
{
    char t[1024];
    int len = snprintf(t, sizeof t, "{\"ev\":\"cfg\",\"interval\":%u,\"items\":[",
                       (unsigned)cfg->refresh_sec);
    if (len < 0 || len >= (int)sizeof t) return -1;
    for (uint8_t i = 0; i < cfg->count && i < WESR_MAX_STOCKS; i++) {
        int w = snprintf(t + len, sizeof t - (size_t)len,
                         "%s{\"code\":\"%s\",\"name\":\"%s\",\"mark\":\"%s\"}",
                         i ? "," : "", cfg->stocks[i].code, cfg->stocks[i].name,
                         cfg->stocks[i].mark);
        if (w < 0 || w >= (int)(sizeof t - (size_t)len)) return -1;
        len += w;
    }
    int w = snprintf(t + len, sizeof t - (size_t)len, "]}");
    if (w < 0 || w >= (int)(sizeof t - (size_t)len)) return -1;
    len += w;
    return finish(out, cap, t, (int)sizeof t, len);
}
