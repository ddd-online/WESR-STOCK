# WESR-STOCK 板端固件 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让微雪 ESP32-S3-RLCD-4.2 板子独立跑起来：4 个页面、按键切页换股、直连腾讯接口拉行情、按设计稿绘制分时图 —— 不依赖小程序就能用（配置用编译期默认值）。

**Architecture:** 在官方 `10_FactoryProgram` 上裁剪（复用它的 I2C/SPI 屏/LVGL/按键 BSP），新代码集中在一个组件 `components/wesr/`。可测的纯逻辑（解析、格式化、坐标映射、导航状态机、刷新调度、1-bit 光栅化）写成不依赖 ESP-IDF 的纯 C，放在 `logic/`，用宿主机 gcc + fixture 跑 TDD；UI 与联网是薄封装。

**Tech Stack:** ESP-IDF 5.5.x（C++17 编译单元，逻辑用 C）、LVGL v8、FreeRTOS、esp_http_client + esp_crt_bundle、NVS、官方 `port_bsp`/`app_bsp`/`ExternLib`。

**Spec:** `docs/superpowers/specs/2026-09-27-wesr-stock-design.md`

## Global Constraints

- 屏幕 **400×300 横屏**，**1-bit 黑白**（只允许 `#000000` / `#FFFFFF`，无灰度、无红绿、无半透明）。
- 顶栏 24px：WiFi 图标 12×10、蓝牙图标 9×12、页码点 4×5px、组方块 2×（7×3）、电量文本 11px、电池图标 19×10。
- 第 1 页左右 **300px / 100px**；右栏 8 行各 34.25px，行间 1px 分隔线，行内「首字 14px + 涨跌 9px」与「现价 14px」。
- 第 2 页四宫格各 200×138；第 3 页分时图 380×168；状态条 9px 居中，需用户动手的状态用反白。
- 行情源：`https://qt.gtimg.cn/q=<code,...>`（≤8 个，GBK 响应，只取数字字段）；`https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=<code>`（**必须 HTTPS**，**不支持批量**）。
- 快照字段下标：3=现价、4=昨收、5=今开、30=时间戳、31=涨跌额、32=涨跌幅%、33=最高、34=最低、36=成交量(手)；字段 1 是 GBK 中文名，**不解析**。
- 分时：`data.<code>.data.data = ["HHMM 价格 累计量(手) 累计额(元)"]`，另有 `.date`；点数不定（实测 267），**不假设 240**；均价 = `累计额 ÷ (累计量 × 100)`。
- 刷新：**只拉当前页**（页 1 = 1 次批量快照，页 2 = 4 次分时，页 3 = 1 次分时，页 4 = 0 次）；交易时段 09:15–11:30、13:00–15:00 每 15 秒；失败退避 5/10/20/30s，连续 5 次 → 未联网状态条。
- 按键：单击 = 切页（1→2→3→4→1）、双击 = 第 2 页切组 / 第 3 页 `(idx+1) % count`（不跳页）、长按 3 秒 = 配网（本计划只留接口，M4 实现）。**无手动刷新**。
- 边界规则 14 条（spec §9）是硬性验收项，逻辑层必须单测覆盖。
- 时区 UTC+8；NTP `ntp.aliyun.com` / `pool.ntp.org`；切页必须 < 100ms（用缓存，不等网络）。

---

## File Structure

```
firmware/                                   # 裁剪自 references/official/02_ESP-IDF/10_FactoryProgram
  CMakeLists.txt  sdkconfig.defaults  partitions.csv
  main/
    CMakeLists.txt  main.cpp  user_config.h # 引脚配置照抄官方
  components/
    port_bsp/  app_bsp/  ExternLib/         # 拷官方，不改（ExternLib 只留 multi_button）
    wesr/                                   # 本项目唯一新组件
      CMakeLists.txt
      include/wesr_logic.h                  # 纯 C 结构体与常量（宿主机可用）
      include/app_state.h
      logic/parse_quote.c                   # 快照行解析
      logic/parse_minute.c                  # 分时 JSON 解析 + 均价
      logic/fmt_stock.c                     # 价格/涨跌/首字/字号档位
      logic/chart_map.c                     # 分钟序号 ↔ x 坐标
      logic/trade_time.c                    # 交易时段 + 休市判定
      logic/nav.c                           # 页面/组/股票索引状态机
      logic/sched.c                          # 按页刷新调度 + 退避
      logic/chart_draw.c                    # 1-bit 光栅化（线/虚线/斜纹/柱子）
      app/app_state.c  cfg_store.c  net_task.c  quote_service.c
      app/sensor_task.c  input_task.c
      ui/ui_theme.h  ui_root.c  ui_page1.c  ui_page2.c  ui_page3.c  ui_page4.c  ui_trend.c
      port/board_init.c
  test/
    Makefile  test_logic.c                  # 宿主机 assert 测试（唯一测试入口）
    fixtures/quote_sh600519.txt             # 真实快照响应（GBK 原样）
    fixtures/minute_sh600519.json           # 真实分时响应
  README.md
```

每个 logic 文件只做一件事，且**不 include 任何 ESP-IDF 头文件**（否则宿主机测不了）。

---

### Task 1: 工程骨架（裁剪官方 FactoryProgram，能编译能上板）

**Files:**
- Create: `firmware/`（从 `references/official/02_ESP-IDF/10_FactoryProgram` 拷贝，排除 `build/`、`managed_components/`、`sdkconfig.old`）
- Delete: `firmware/components/ui_bsp/`、`firmware/components/ExternLib/codec_board/`、`firmware/components/ExternLib/esp_codec_dev/`、`firmware/components/app_bsp/ble_scan_bsp.*`
- Modify: `firmware/main/CMakeLists.txt`（去掉对 ui_bsp 的依赖）
- Create: `firmware/components/wesr/CMakeLists.txt`

**Interfaces:**
- Consumes: 无
- Produces: 可编译的 IDF 工程，后续任务都往里加文件

为什么删这些：v1 不用音频（codec_board + esp_codec_dev 是唯一的大块依赖），也不用出厂 UI（`ui_bsp` 里有 8MB 图片字体），BLE 扫描在 M4 之前用不到。

- [ ] **Step 1: 拷贝工程并裁剪**

```powershell
$src = 'D:\github\WESR-STOCK\references\official\02_ESP-IDF\10_FactoryProgram'
$dst = 'D:\github\WESR-STOCK\firmware'
robocopy $src $dst /E /XD build managed_components /XF sdkconfig.old
Remove-Item -Recurse -Force "$dst\components\ui_bsp",
  "$dst\components\ExternLib\codec_board",
  "$dst\components\ExternLib\esp_codec_dev"
Remove-Item -Force "$dst\components\app_bsp\ble_scan_bsp.c","$dst\components\app_bsp\ble_scan_bsp.h"
```

- [ ] **Step 2: 让 main 不再引用被删的组件**

把 `firmware/main/main.cpp` 里对 `ble_scan_bsp.h`、出厂 UI（`ui_bsp`）的 include 与调用全删掉，只保留：初始化 NVS → `Board_Init()`（board_init 稍后建）→ 起任务。此步的临时版本只要求能编译通过：

```cpp
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI("wesr", "boot ok, version %s", "0.1.0");
    while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
```

同时把 `firmware/main/CMakeLists.txt` 的 `REQUIRES` 里出现的 `ui_bsp`、`codec_board` 之类删掉（保留 `port_bsp app_bsp`）。

- [ ] **Step 3: 加空组件 wesr**

`firmware/components/wesr/CMakeLists.txt`：

```cmake
idf_component_register(
    SRCS
    INCLUDE_DIRS "include"
    REQUIRES port_bsp app_bsp esp_wifi nvs_flash esp_http_client json mbedtls
)
```

- [ ] **Step 4: 编译**

Run: `cd firmware; idf.py set-target esp32s3; idf.py build`
Expected: 编译成功，末尾打印 `Project build complete`。（首次会自动拉依赖，慢是正常的）

- [ ] **Step 5: 上板冒烟**

Run: `idf.py -p COM3 flash monitor`
Expected: 串口打印 `boot ok, version 0.1.0`，不重启、无 Guru Meditation。

- [ ] **Step 6: Commit**

```bash
git add firmware
git commit -m "feat(firmware): 以官方 FactoryProgram 为骨架裁剪出 wesr 工程"
```

---

### Task 2: 宿主机测试骨架 + 快照行解析

**Files:**
- Create: `firmware/include/wesr_logic.h` → 实际路径 `firmware/components/wesr/include/wesr_logic.h`
- Create: `firmware/components/wesr/logic/parse_quote.c`
- Create: `firmware/test/Makefile`、`firmware/test/test_logic.c`
- Create: `firmware/test/fixtures/quote_sh600519.txt`

**Interfaces:**
- Consumes: 无
- Produces:
  - `bool wesr_parse_quote_line(const char *line, wesr_quote_t *q)`
  - `typedef struct { float last, prev_close, open, high, low, chg, chg_pct; uint32_t vol_hands; uint64_t stamp; bool valid; } wesr_quote_t;`

- [ ] **Step 1: 写共享头文件**

`firmware/components/wesr/include/wesr_logic.h`：

```c
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
```

- [ ] **Step 2: 存真实 fixture**

把本次实测抓到的快照响应原样存进 `firmware/test/fixtures/quote_sh600519.txt`（GBK 字节，不要转码）：

```powershell
curl.exe -s --max-time 20 "https://qt.gtimg.cn/q=sh600519" -o firmware/test/fixtures/quote_sh600519.txt
```

Expected: 文件以 `v_sh600519="1~` 开头，约 550 字节。

- [ ] **Step 3: 写失败的测试**

`firmware/test/test_logic.c`（先只放快照用例）：

```c
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

int main(void)
{
    test_quote();
    printf("all logic tests passed\n");
    return 0;
}
```

- [ ] **Step 4: 写 Makefile 并确认测试编译失败**

`firmware/test/Makefile`：

```makefile
CC      ?= gcc
CFLAGS  := -std=c11 -Wall -Wextra -O1 -g -I../components/wesr/include
LOGIC   := $(wildcard ../components/wesr/logic/*.c)

build/test_logic: test_logic.c $(LOGIC)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_logic.c $(LOGIC) -lm

.PHONY: run clean
run: build/test_logic
	./build/test_logic
clean:
	rm -rf build
```

Run: `cd firmware; mingw32-make -C test run`
Expected: FAIL —— `undefined reference to wesr_parse_quote_line`（函数还没写）

- [ ] **Step 5: 实现解析**

`firmware/components/wesr/logic/parse_quote.c`：

```c
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
```

- [ ] **Step 6: 跑测试**

Run: `cd firmware; mingw32-make -C test run`
Expected: PASS，打印 `all logic tests passed`

- [ ] **Step 7: 加异常用例（边界规则 8）**

在 `test_logic.c` 的 `main()` 前补：

```c
static void test_quote_rejects_bad(void)
{
    wesr_quote_t q;
    assert(!wesr_parse_quote_line("v_sh600519=\"1~x~600519~\"", &q));      /* 字段不足 */
    assert(!wesr_parse_quote_line("v_sh600519=\"1~x~600519~0~0~0\"", &q)); /* 价格 0 */
    assert(!wesr_parse_quote_line("garbage", &q));                        /* 完全不对 */
    assert(!wesr_parse_quote_line("v_x=\"1~x~x~-1~10~10\";", &q));        /* 负价 */
}
```

并在 `main()` 里加 `test_quote_rejects_bad();`。

Run: `mingw32-make -C test run`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add firmware/components/wesr/include firmware/components/wesr/logic/parse_quote.c firmware/test
git commit -m "feat(wesr): 宿主机测试骨架 + 腾讯快照行解析"
```

---

### Task 3: 分时 JSON 解析 + 均价

**Files:**
- Create: `firmware/components/wesr/logic/parse_minute.c`
- Create: `firmware/test/fixtures/minute_sh600519.json`
- Modify: `firmware/test/test_logic.c`

**Interfaces:**
- Consumes: `wesr_minute_t`、`wesr_point_t`（Task 2 的头文件）
- Produces: `bool wesr_parse_minute_json(const char *json, wesr_minute_t *out)`

- [ ] **Step 1: 存真实 fixture**

```powershell
curl.exe -sL --max-time 25 -A "Mozilla/5.0" `
  "https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=sh600519" `
  -o firmware/test/fixtures/minute_sh600519.json
```

Expected: 约 11KB 的 JSON，形如 `{"code":0,"msg":"","data":{"sh600519":{"data":{"data":["0930 1250.01 ..."]...`

- [ ] **Step 2: 写失败的测试**

在 `test_logic.c` 里加：

```c
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
}
```

并在 `main()` 里加两行调用。

Run: `cd firmware; mingw32-make -C test run`
Expected: FAIL —— `undefined reference to wesr_parse_minute_json`

- [ ] **Step 3: 实现解析**

`firmware/components/wesr/logic/parse_minute.c`：

```c
#include "wesr_logic.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
```

- [ ] **Step 4: 跑测试**

Run: `cd firmware; mingw32-make -C test run`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/logic/parse_minute.c firmware/test
git commit -m "feat(wesr): 分时 JSON 解析与均价计算"
```

---

### Task 3b: 从分时响应里取快照与市场状态

**Files:**
- Create: `firmware/components/wesr/logic/parse_meta.c`
- Modify: `firmware/components/wesr/include/wesr_logic.h`
- Modify: `firmware/test/test_logic.c`

**Interfaces:**
- Consumes: `wesr_parse_minute_json`（Task 3）用同一个 buffer，但**不改它的签名**
- Produces:
  - `typedef struct { bool has_quote; wesr_quote_t quote; bool closed; } wesr_minute_meta_t;`
  - `bool wesr_parse_minute_meta(const char *json, wesr_minute_meta_t *meta)`

为什么单独一个函数而不是改 `wesr_parse_minute_json` 的签名：多扫一遍 11KB JSON 的代价可以忽略，但能避免改已有测试和调用点。

- [ ] **Step 1: 加声明**

```c
typedef struct {
    bool has_quote;          /* qt 数组解析成功 */
    wesr_quote_t quote;      /* 现价/昨收等，从 qt 数组按 §4.1 下标取 */
    bool closed;             /* 服务端 market 字段说休市 */
} wesr_minute_meta_t;

bool wesr_parse_minute_meta(const char *json, wesr_minute_meta_t *meta);
```

- [ ] **Step 2: 写失败的测试**

```c
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
```

Run: `cd firmware; mingw32-make -C test run`
Expected: FAIL —— `undefined reference to wesr_parse_minute_meta`

- [ ] **Step 3: 实现**

`firmware/components/wesr/logic/parse_meta.c`：

```c
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

bool wesr_parse_minute_meta(const char *json, wesr_minute_meta_t *meta)
{
    /* 空/NULL 视为非法输入；能扫但没带 qt/market 的响应不算错（返回 true，字段留空） */
    if (!json || !json[0] || !meta) return false;
    memset(meta, 0, sizeof *meta);

    /* 快照：qt 段里第一对"能解析出快照"的 [ ]。
       实测响应里 "v_ff_<code>":[] 这个空数组排在最前面，直接取第一个 [ 会拿到空数组。 */
    const char *qt = strstr(json, "\"qt\"");
    if (qt) {
        const char *p = qt;
        while ((p = strchr(p, '[')) != NULL) {
            const char *e = strchr(p, ']');
            if (!e) break;
            float last = 0, prev = 0;
            if (arr_f(p, e, 3, &last) && arr_f(p, e, 4, &prev) && last > 0 && prev > 0) {
                meta->quote.last = last;
                meta->quote.prev_close = prev;
                arr_f(p, e, 5, &meta->quote.open);
                arr_f(p, e, 31, &meta->quote.chg);
                arr_f(p, e, 32, &meta->quote.chg_pct);
                arr_f(p, e, 33, &meta->quote.high);
                arr_f(p, e, 34, &meta->quote.low);
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
```

- [ ] **Step 4: 跑测试**

Run: `cd firmware; mingw32-make -C test run`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/logic/parse_meta.c firmware/components/wesr/include/wesr_logic.h firmware/test
git commit -m "feat(wesr): 分时响应内的快照与市场状态解析"
```

---

### Task 4: 数值与文本格式化（边界规则 7、8、10、13）

**Files:**
- Create: `firmware/components/wesr/logic/fmt_stock.c`
- Modify: `firmware/components/wesr/include/wesr_logic.h`（加声明）
- Modify: `firmware/test/test_logic.c`

**Interfaces:**
- Consumes: `wesr_stock_cfg_t`
- Produces:
  - `void wesr_fmt_price(char *out, size_t cap, float v, bool valid)`
  - `void wesr_fmt_pct(char *out, size_t cap, float pct, bool valid)`
  - `int  wesr_price_font_px(float v)`
  - `void wesr_first_char(const char *utf8, char *out, size_t cap)`
  - `void wesr_make_marks(wesr_stock_cfg_t *list, int n)`

- [ ] **Step 1: 加声明**

在 `wesr_logic.h` 末尾追加：

```c
void wesr_fmt_price(char *out, size_t cap, float v, bool valid);
void wesr_fmt_pct(char *out, size_t cap, float pct, bool valid);
int  wesr_price_font_px(float v);
void wesr_first_char(const char *utf8, char *out, size_t cap);
void wesr_make_marks(wesr_stock_cfg_t *list, int n);
```

- [ ] **Step 2: 写失败的测试**

```c
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
```

- [ ] **Step 3: 实现**

`firmware/components/wesr/logic/fmt_stock.c`：

```c
#include "wesr_logic.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DASH  "\xE2\x80\x94"        /* — */
#define UP    "\xE2\x96\xB2"        /* ▲ */
#define DOWN  "\xE2\x96\xBC"        /* ▼ */

void wesr_fmt_price(char *out, size_t cap, float v, bool valid)
{
    if (!valid || !isfinite(v)) { snprintf(out, cap, "%s", DASH); return; }
    snprintf(out, cap, "%.2f", v);
}

void wesr_fmt_pct(char *out, size_t cap, float pct, bool valid)
{
    if (!valid || !isfinite(pct)) { snprintf(out, cap, "%s", DASH); return; }
    if (fabsf(pct) < 0.005f) { snprintf(out, cap, "0.00%%"); return; }
    snprintf(out, cap, "%s%.2f%%", pct > 0 ? UP : DOWN, fabsf(pct));
}

int wesr_price_font_px(float v)
{
    float a = fabsf(v);
    return (a >= 1000.0f) ? 12 : 14;   /* 整数部分 ≥4 位 → 降一档 */
}

/* 取 UTF-8 第一个字符（汉字 3 字节，ASCII 1 字节） */
void wesr_first_char(const char *utf8, char *out, size_t cap)
{
    out[0] = 0;
    if (!utf8 || !utf8[0]) return;
    unsigned char c = (unsigned char)utf8[0];
    size_t len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    if (len + 1 > cap) return;
    for (size_t i = 0; i < len; i++) {
        if (!utf8[i]) { out[0] = 0; return; }
        out[i] = utf8[i];
    }
    out[len] = 0;
}

/* 首字重复时，冲突的那只退成前两个字（边界规则 13） */
void wesr_make_marks(wesr_stock_cfg_t *list, int n)
{
    for (int i = 0; i < n; i++) {
        wesr_first_char(list[i].name, list[i].mark, sizeof list[i].mark);
    }
    for (int i = 0; i < n; i++) {
        if (!list[i].mark[0]) continue;
        for (int j = 0; j < i; j++) {
            if (strcmp(list[i].mark, list[j].mark) != 0) continue;
            /* 前两个字：取到第二个 UTF-8 字符边界 */
            char two[WESR_MARK_LEN] = {0};
            wesr_first_char(list[i].name, two, sizeof two);
            size_t used = strlen(two);
            wesr_first_char(list[i].name + used, two + used, sizeof two - used);
            snprintf(list[i].mark, sizeof list[i].mark, "%s", two);
            break;
        }
    }
}
```

- [ ] **Step 4: 跑测试**

Run: `cd firmware; mingw32-make -C test run`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/logic/fmt_stock.c firmware/components/wesr/include/wesr_logic.h firmware/test
git commit -m "feat(wesr): 价格/涨跌/首字/字号格式化与首字冲突处理"
```

---

### Task 5: 横轴映射与交易时段判定（边界规则 9）

**Files:**
- Create: `firmware/components/wesr/logic/chart_map.c`、`firmware/components/wesr/logic/trade_time.c`
- Modify: `firmware/components/wesr/include/wesr_logic.h`
- Modify: `firmware/test/test_logic.c`

**Interfaces:**
- Produces:
  - `int  wesr_minute_index(uint16_t hhmm)` — 09:30→0 … 11:30→120、13:00→121 … 15:00→241，越界 clamp
  - `int  wesr_index_to_x(int idx, int x0, int x1)`
  - `bool wesr_in_trading(uint16_t hhmm)` — 本地兜底；服务端休市判定在 Task 3b 的 `wesr_parse_minute_meta`

- [ ] **Step 1: 加声明**

```c
int  wesr_minute_index(uint16_t hhmm);
int  wesr_index_to_x(int idx, int x0, int x1);
bool wesr_in_trading(uint16_t hhmm);
```

- [ ] **Step 2: 写失败的测试**

```c
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
```

- [ ] **Step 3: 实现**

`firmware/components/wesr/logic/chart_map.c`：

```c
#include "wesr_logic.h"

#define DAY_OPEN   930
#define AM_CLOSE   1130
#define PM_OPEN    1300
#define DAY_CLOSE  1500
#define AM_SPAN    (11 * 60 + 30 - (9 * 60 + 30))      /* 120 */
#define TOTAL_SPAN (AM_SPAN + 1 + (15 * 60 - 13 * 60)) /* 241 */

static int to_min(uint16_t hhmm) { return (hhmm / 100) * 60 + (hhmm % 100); }

int wesr_minute_index(uint16_t hhmm)
{
    if (hhmm <= DAY_OPEN) return 0;
    if (hhmm <= AM_CLOSE) return to_min(hhmm) - to_min(DAY_OPEN);
    if (hhmm < PM_OPEN)   return AM_SPAN;                 /* 午休压成一个点 */
    if (hhmm <= DAY_CLOSE) return AM_SPAN + 1 + (to_min(hhmm) - to_min(PM_OPEN));
    return TOTAL_SPAN;
}

int wesr_index_to_x(int idx, int x0, int x1)
{
    if (idx < 0) idx = 0;
    if (idx > TOTAL_SPAN) idx = TOTAL_SPAN;
    return x0 + (int)((long)(x1 - x0) * idx / TOTAL_SPAN);
}
```

`firmware/components/wesr/logic/trade_time.c`：

```c
#include "wesr_logic.h"

bool wesr_in_trading(uint16_t hhmm)
{
    return (hhmm >= 915 && hhmm <= 1130) || (hhmm >= 1300 && hhmm <= 1500);
}
```

- [ ] **Step 4: 跑测试**

Run: `cd firmware; mingw32-make -C test run`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/logic/chart_map.c firmware/components/wesr/logic/trade_time.c firmware/components/wesr/include/wesr_logic.h firmware/test
git commit -m "feat(wesr): 分时横轴映射与交易时段判定"
```

---

### Task 6: 页面/组/股票索引状态机（边界规则 2–5）

**Files:**
- Create: `firmware/components/wesr/logic/nav.c`
- Modify: `firmware/components/wesr/include/wesr_logic.h`
- Modify: `firmware/test/test_logic.c`

**Interfaces:**
- Produces:
  - `typedef struct { uint8_t page, group, idx, count; } wesr_nav_t;`
  - `void wesr_nav_init(wesr_nav_t *n, uint8_t count)`
  - `void wesr_nav_click(wesr_nav_t *n)`
  - `void wesr_nav_double(wesr_nav_t *n)`
  - `void wesr_nav_set_count(wesr_nav_t *n, uint8_t count)`
  - `uint8_t wesr_nav_group_start(const wesr_nav_t *n)` — 返回 0 或 4
  - `uint8_t wesr_nav_pages(void)` → 恒为 4

按键的去抖、单击/双击/长按判定**不自己写**：官方 `ExternLib/multi_button` 已经提供 `SINGLE_CLICK` / `DOUBLE_CLICK` / `LONG_PRESS_START` 事件，这里只处理事件语义（YAGNI）。

- [ ] **Step 1: 加声明**

```c
typedef struct { uint8_t page, group, idx, count; } wesr_nav_t;
void    wesr_nav_init(wesr_nav_t *n, uint8_t count);
void    wesr_nav_click(wesr_nav_t *n);
void    wesr_nav_double(wesr_nav_t *n);
void    wesr_nav_set_count(wesr_nav_t *n, uint8_t count);
uint8_t wesr_nav_group_start(const wesr_nav_t *n);
uint8_t wesr_nav_pages(void);
```

- [ ] **Step 2: 写失败的测试**

```c
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
```

- [ ] **Step 3: 实现**

`firmware/components/wesr/logic/nav.c`：

```c
#include "wesr_logic.h"

uint8_t wesr_nav_pages(void) { return 4; }

void wesr_nav_init(wesr_nav_t *n, uint8_t count)
{
    n->page = 1; n->group = 0; n->idx = 0;
    wesr_nav_set_count(n, count);
}

void wesr_nav_set_count(wesr_nav_t *n, uint8_t count)
{
    if (count > WESR_MAX_STOCKS) count = WESR_MAX_STOCKS;
    n->count = count;
    if (count <= 4) n->group = 0;                    /* 组 2 不存在 */
    if (count == 0) n->idx = 0;
    else if (n->idx >= count) n->idx = (uint8_t)(count - 1);
}

void wesr_nav_click(wesr_nav_t *n)
{
    n->page = (uint8_t)(n->page >= wesr_nav_pages() ? 1 : n->page + 1);
}

void wesr_nav_double(wesr_nav_t *n)
{
    if (n->page == 2) {
        if (n->count > 4) n->group = (uint8_t)(n->group ? 0 : 1);
    } else if (n->page == 3) {
        if (n->count > 0) n->idx = (uint8_t)((n->idx + 1) % n->count);
    }
    /* 第 1、4 页双击无动作 */
}

uint8_t wesr_nav_group_start(const wesr_nav_t *n)
{
    return (n->group == 1 && n->count > 4) ? 4 : 0;
}
```

- [ ] **Step 4: 跑测试**

Run: `cd firmware; mingw32-make -C test run`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/logic/nav.c firmware/components/wesr/include/wesr_logic.h firmware/test
git commit -m "feat(wesr): 页面/组/股票索引状态机"
```

---

### Task 7: 刷新调度与失败退避（边界规则 12）

**Files:**
- Create: `firmware/components/wesr/logic/sched.c`
- Modify: `firmware/components/wesr/include/wesr_logic.h`
- Modify: `firmware/test/test_logic.c`

**Interfaces:**
- Produces:
  - `typedef enum { WESR_FETCH_NONE = 0, WESR_FETCH_QUOTES, WESR_FETCH_MINUTES } wesr_fetch_t;`
  - `typedef struct { uint8_t page; uint16_t interval_s; uint32_t next_ms; uint8_t fail_streak; bool offline; } wesr_sched_t;`
  - `void wesr_sched_init(wesr_sched_t *s, uint16_t interval_s)`
  - `void wesr_sched_page(wesr_sched_t *s, uint8_t page)` — 切页后立刻允许拉取
  - `wesr_fetch_t wesr_sched_tick(wesr_sched_t *s, uint32_t now_ms, bool trading)`
  - `void wesr_sched_result(wesr_sched_t *s, bool ok, uint32_t now_ms)`

- [ ] **Step 1: 加声明**

```c
typedef enum { WESR_FETCH_NONE = 0, WESR_FETCH_QUOTES, WESR_FETCH_MINUTES } wesr_fetch_t;
typedef struct {
    uint8_t page;
    uint16_t interval_s;
    uint32_t next_ms;
    uint8_t fail_streak;
    bool offline;
} wesr_sched_t;
void         wesr_sched_init(wesr_sched_t *s, uint16_t interval_s);
void         wesr_sched_page(wesr_sched_t *s, uint8_t page);
wesr_fetch_t wesr_sched_tick(wesr_sched_t *s, uint32_t now_ms, bool trading);
void         wesr_sched_result(wesr_sched_t *s, bool ok, uint32_t now_ms);
```

- [ ] **Step 2: 写失败的测试**

```c
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
```

- [ ] **Step 3: 实现**

`firmware/components/wesr/logic/sched.c`：

```c
#include "wesr_logic.h"

#define RETRY_MIN_MS  5000u
#define IDLE_RECHECK_MS 60000u

static const uint32_t k_backoff[] = { 5000u, 10000u, 20000u, 30000u };

void wesr_sched_init(wesr_sched_t *s, uint16_t interval_s)
{
    s->page = 1;
    s->interval_s = interval_s ? interval_s : 15;
    s->next_ms = 0;
    s->fail_streak = 0;
    s->offline = false;
}

void wesr_sched_page(wesr_sched_t *s, uint8_t page)
{
    s->page = page;
    s->next_ms = 0;                 /* 立即到期，切页不等网络：先用缓存画 */
}

static uint32_t interval_ms(const wesr_sched_t *s)
{
    return (uint32_t)s->interval_s * 1000u;
}

wesr_fetch_t wesr_sched_tick(wesr_sched_t *s, uint32_t now_ms, bool trading)
{
    if (!trading) {
        s->next_ms = now_ms + IDLE_RECHECK_MS;   /* 休市：一分钟后再看 */
        return WESR_FETCH_NONE;
    }
    if ((int32_t)(now_ms - s->next_ms) < 0) return WESR_FETCH_NONE;
    if (s->page == 4) {
        s->next_ms = now_ms + interval_ms(s);
        return WESR_FETCH_NONE;
    }
    return (s->page == 1) ? WESR_FETCH_QUOTES : WESR_FETCH_MINUTES;
}

void wesr_sched_result(wesr_sched_t *s, bool ok, uint32_t now_ms)
{
    if (ok) {
        s->fail_streak = 0;
        s->offline = false;
        s->next_ms = now_ms + interval_ms(s);
        return;
    }
    if (s->fail_streak < 255) s->fail_streak++;
    uint8_t idx = (uint8_t)(s->fail_streak - 1);
    if (idx > 3) idx = 3;
    uint32_t wait = k_backoff[idx];
    if (wait < RETRY_MIN_MS) wait = RETRY_MIN_MS;
    s->next_ms = now_ms + wait;
    s->offline = (s->fail_streak >= 5);
}
```

- [ ] **Step 4: 跑测试**

Run: `cd firmware; mingw32-make -C test run`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/logic/sched.c firmware/components/wesr/include/wesr_logic.h firmware/test
git commit -m "feat(wesr): 按页刷新调度与失败退避"
```

---

### Task 8: 1-bit 光栅化（分时图的地基，宿主机可测）

**Files:**
- Create: `firmware/components/wesr/logic/chart_draw.c`
- Modify: `firmware/components/wesr/include/wesr_logic.h`
- Modify: `firmware/test/test_logic.c`

**Interfaces:**
- Consumes: `wesr_minute_t`、`wesr_minute_index`（Task 5）、`wesr_index_to_x`（Task 5）
- Produces:
  - `typedef struct { uint8_t *buf; int w, h, stride; } wesr_bmp_t;`（1bpp，行按 `(w+7)/8` 字节对齐，MSB 在左）
  - `void wesr_bmp_init(wesr_bmp_t *b, uint8_t *buf, int w, int h)`
  - `void wesr_bmp_clear(wesr_bmp_t *b)` / `void wesr_bmp_px(...)` / `bool wesr_bmp_get(...)`
  - `void wesr_bmp_hline(...)` / `void wesr_bmp_line(...)` / `void wesr_bmp_dash_hline(...)` / `void wesr_bmp_vbar(...)`
  - `void wesr_bmp_hatch45(wesr_bmp_t *b, const int *y_line, int x0, int x1, int y_base, int period, int on)`
  - `typedef struct { int pad_top, pad_bottom, pad_x, price_h, vol_top, vol_h; bool hatch, grid; float span_ratio; } wesr_chart_opts_t;`
  - `void wesr_chart_render(wesr_bmp_t *b, const wesr_minute_t *m, float prev_close, const wesr_chart_opts_t *o)`

为什么自己画而不是用 LVGL 图元：斜纹填充和"价格线与昨收之间的区域"用 LVGL 对象拼不出来，而自己画成 1bpp 位图后**能在宿主机上用断言验证**，比盯着屏幕猜快得多。

- [ ] **Step 1: 加声明**

```c
typedef struct { uint8_t *buf; int w, h, stride; } wesr_bmp_t;
void wesr_bmp_init(wesr_bmp_t *b, uint8_t *buf, int w, int h);
void wesr_bmp_clear(wesr_bmp_t *b);
void wesr_bmp_px(wesr_bmp_t *b, int x, int y);
bool wesr_bmp_get(const wesr_bmp_t *b, int x, int y);
void wesr_bmp_hline(wesr_bmp_t *b, int x0, int x1, int y);
void wesr_bmp_line(wesr_bmp_t *b, int x0, int y0, int x1, int y1);
void wesr_bmp_dash_hline(wesr_bmp_t *b, int x0, int x1, int y, int on, int off);
void wesr_bmp_vbar(wesr_bmp_t *b, int x, int w, int y_top, int y_bot);
void wesr_bmp_hatch45(wesr_bmp_t *b, const int *y_line, int x0, int x1, int y_base,
                      int period, int on);

typedef struct {
    int  pad_top, pad_bottom, pad_x;
    int  price_h;            /* 价格区高度 */
    int  vol_top, vol_h;     /* 成交量条：vol_h = 0 表示不画 */
    bool hatch;              /* 价格与昨收之间填斜纹 */
    bool grid;               /* 10:30 / 11:30·13:00 / 14:00 竖虚线 */
    float span_ratio;        /* 纵轴留白系数，用 1.1；<=0 时按 1.1 */
} wesr_chart_opts_t;

void wesr_chart_render(wesr_bmp_t *b, const wesr_minute_t *m, float prev_close,
                       const wesr_chart_opts_t *o);
```

- [ ] **Step 2: 写失败的测试**

```c
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
    assert(buf[15] == 0x01);                       /* y=7 → 第 7 行第 2 个字节，x=15 是它的最低位 */
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

    /* 昨收基准线（10.00）在价格区上半部分，且是虚线：必然有断开 */
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
    assert(gaps > 20);                       /* 虚线而非实线 */

    /* 末点 1500 落在右边界：最右一列在价格区里必须有像素 */
    int last_col = 0;
    for (int y = o.pad_top; y < o.price_h; y++) {
        if (wesr_bmp_get(&b, 380 - 1, y)) last_col++;
    }
    assert(last_col > 0);

    /* 不开斜纹时，价格线下方、基准线上方的区域应当是空的 */
    int empty = 0;
    for (int x = 10; x < 300; x++)
        for (int y = y_base + 2; y < y_base + 20; y++)
            if (!wesr_bmp_get(&b, x, y)) empty++;
    assert(empty > 1000);

    /* 成交量条区域有像素（n=3，只有 3 根柱，位置按分钟序号映射） */
    int vol_px = 0;
    for (int x = 0; x < 380; x++)
        for (int y = o.vol_top; y < o.vol_top + o.vol_h; y++)
            if (wesr_bmp_get(&b, x, y)) vol_px++;
    assert(vol_px > 0);

    /* 3 个点也要能画：点数不足时右侧留白，不崩 */
    wesr_bmp_clear(&b);
    o.hatch = true;
    wesr_chart_render(&b, &m, 10.00f, &o);
    int right_empty = 0;
    for (int x = 360; x < 380; x++)
        for (int y = o.pad_top; y < o.price_h; y++)
            if (!wesr_bmp_get(&b, x, y)) right_empty++;
    assert(right_empty > 100);               /* 点数不足，右侧留白（边界规则 9） */

    /* hatch 打开后，价格线上方的区域被斜纹填了一部分但不全填 */
    int filled = 0, total = 0;
    for (int x = 20; x < 200; x++) {
        for (int y = o.pad_top; y < y_base; y++) {
            if (y < o.pad_top + 4) continue;
            total++;
            if (wesr_bmp_get(&b, x, y)) filled++;
        }
    }
    assert(filled > 0 && filled < total);    /* 有图案，不是实心块 */

    /* prev_close = 0 时不能崩、不能出 inf：退化成不画基准线 */
    wesr_bmp_clear(&b);
    wesr_chart_render(&b, &m, 0.0f, &o);
}
```

Run: `cd firmware; mingw32-make -C test run`
Expected: FAIL —— `undefined reference to wesr_bmp_init`

- [ ] **Step 3: 实现**

`firmware/components/wesr/logic/chart_draw.c`：

```c
#include "wesr_logic.h"
#include <math.h>
#include <stdlib.h>          /* abs() */
#include <string.h>

void wesr_bmp_init(wesr_bmp_t *b, uint8_t *buf, int w, int h)
{
    b->buf = buf; b->w = w; b->h = h;
    b->stride = (w + 7) / 8;
    wesr_bmp_clear(b);
}

void wesr_bmp_clear(wesr_bmp_t *b)
{
    memset(b->buf, 0, (size_t)b->stride * (size_t)b->h);
}

void wesr_bmp_px(wesr_bmp_t *b, int x, int y)
{
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return;
    b->buf[y * b->stride + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
}

bool wesr_bmp_get(const wesr_bmp_t *b, int x, int y)
{
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return false;
    return (b->buf[y * b->stride + (x >> 3)] >> (7 - (x & 7))) & 1u;
}

void wesr_bmp_hline(wesr_bmp_t *b, int x0, int x1, int y)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) wesr_bmp_px(b, x, y);
}

void wesr_bmp_line(wesr_bmp_t *b, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        wesr_bmp_px(b, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void wesr_bmp_dash_hline(wesr_bmp_t *b, int x0, int x1, int y, int on, int off)
{
    if (on < 1) on = 1;
    if (off < 1) off = 1;
    int period = on + off;
    for (int x = x0; x <= x1; x++) {
        if (((x - x0) % period) < on) wesr_bmp_px(b, x, y);
    }
}

void wesr_bmp_vbar(wesr_bmp_t *b, int x, int w, int y_top, int y_bot)
{
    for (int i = 0; i < w; i++)
        for (int y = y_top; y <= y_bot; y++) wesr_bmp_px(b, x + i, y);
}

void wesr_bmp_hatch45(wesr_bmp_t *b, const int *y_line, int x0, int x1, int y_base,
                      int period, int on)
{
    if (period < 2) period = 2;
    for (int x = x0; x <= x1; x++) {
        int yl = y_line[x - x0];
        int lo = yl < y_base ? yl : y_base;
        int hi = yl < y_base ? y_base : yl;
        if (lo < 0) lo = 0;
        if (hi >= b->h) hi = b->h - 1;
        for (int y = lo; y <= hi; y++) {
            int d = (x + y) % period;
            if (d < 0) d += period;
            if (d < on) wesr_bmp_px(b, x, y);
        }
    }
}

void wesr_chart_render(wesr_bmp_t *b, const wesr_minute_t *m, float prev_close,
                       const wesr_chart_opts_t *o)
{
    if (!b || !m || !o || m->n == 0) return;
    int x0 = o->pad_x, x1 = b->w - o->pad_x;
    int price_h = o->price_h > 0 ? o->price_h : b->h;
    float ratio = o->span_ratio > 0 ? o->span_ratio : 1.1f;
    if (!(prev_close > 0)) prev_close = m->pts[0].price;
    if (!(prev_close > 0)) return;                        /* 全无数据：留白 */

    /* 纵轴以昨收为中心上下对称 */
    float hi = 1.0f, lo = 1.0f;
    for (uint16_t i = 0; i < m->n; i++) {
        float r = m->pts[i].price / prev_close;
        if (r > hi) hi = r;
        if (r < lo) lo = r;
    }
    float span = fmaxf(hi - 1.0f, 1.0f - lo) * ratio;
    if (span < 0.002f) span = 0.002f;

    int top = o->pad_top, bot = price_h - o->pad_bottom;
    float units = 2.0f * span;
    int y_base = top + (int)((float)(bot - top) * (span / units));

    /* 时间刻度竖虚线 */
    if (o->grid) {
        const int marks[3] = { 60, 120, 181 };            /* 10:30 / 11:30·13:00 / 14:00 */
        for (int k = 0; k < 3; k++) {
            int xg = wesr_index_to_x(marks[k], x0, x1);
            for (int y = top; y < bot; y++) {
                if (((y - top) % 4) < 1) wesr_bmp_px(b, xg, y);
            }
        }
    }

    /* 每个点的 x 与 y */
    int y_line[WESR_MAX_POINTS];
    int xs[WESR_MAX_POINTS];
    if (m->n > WESR_MAX_POINTS) return;      /* 守住数组边界（解析器本来就封顶 300） */
    for (uint16_t i = 0; i < m->n; i++) {
        float r = m->pts[i].price / prev_close;
        int idx = wesr_minute_index(m->pts[i].hhmm);
        xs[i] = wesr_index_to_x(idx, x0, x1);
        float up = (1.0f + span) - r;                     /* r=1 → span */
        int y = top + (int)((float)(bot - top) * up / units);
        if (y < top) y = top;
        if (y > bot) y = bot;
        y_line[i] = y;
    }

    /* 涨/跌面积：斜纹。
       注意 y_line 是按"点"存的，而 hatch45 按"列"取 y；点数少时列数远大于点数，
       直接传 y_line 会越界读。这里先按列线性插值出一份稠密的逐列 y（最大 400 列）。 */
    if (o->hatch) {
        if (b->w > 400) return;
        int col_y[400];
        int x_l = xs[0], x_r = xs[m->n - 1];
        uint16_t seg = 0;
        for (int x = x_l; x <= x_r; x++) {
            while (seg + 1 < m->n && x > xs[seg + 1]) seg++;
            if (seg + 1 >= m->n) { col_y[x - x_l] = y_line[m->n - 1]; continue; }
            int x0 = xs[seg], x1 = xs[seg + 1];
            int y0 = y_line[seg], y1 = y_line[seg + 1];
            int dx = x1 - x0;
            col_y[x - x_l] = (dx <= 0) ? y1 : (y0 + (y1 - y0) * (x - x0) / dx);
        }
        wesr_bmp_hatch45(b, col_y, x_l, x_r, y_base, 4, 1);
    }

    /* 昨收基准线（3 on / 2 off） */
    wesr_bmp_dash_hline(b, x0, x1, y_base, 3, 2);

    /* 均价线（虚线 2.5/2，用 2 on 2 off 近似） */
    for (uint16_t i = 1; i < m->n; i++) {
        if ((i % 4) >= 2) continue;
        float r = m->pts[i].avg / prev_close;
        float up = (1.0f + span) - r;
        int y1 = top + (int)((float)(bot - top) * up / units);
        float r0 = m->pts[i - 1].avg / prev_close;
        int y0 = top + (int)((float)(bot - top) * ((1.0f + span) - r0) / units);
        wesr_bmp_line(b, xs[i - 1], y0, xs[i], y1);
    }

    /* 价格线（实线） */
    for (uint16_t i = 1; i < m->n; i++) {
        wesr_bmp_line(b, xs[i - 1], y_line[i - 1], xs[i], y_line[i]);
    }

    /* 成交量柱 */
    if (o->vol_h > 0) {
        float vmax = 0.0f;
        for (uint16_t i = 0; i < m->n; i++) if (m->pts[i].vol > vmax) vmax = m->pts[i].vol;
        if (vmax > 0) {
            for (uint16_t i = 0; i < m->n; i++) {
                int h = (int)((float)o->vol_h * (m->pts[i].vol / vmax));
                if (h < 1 && m->pts[i].vol > 0) h = 1;
                if (h > 0) wesr_bmp_vbar(b, xs[i], 1, o->vol_top + o->vol_h - h,
                                         o->vol_top + o->vol_h - 1);
            }
        }
    }
}
```

- [ ] **Step 4: 跑测试**

Run: `cd firmware; mingw32-make -C test run`
Expected: PASS（断言失败**一律先查实现**：这些阈值是设计意图的量化，放松阈值会把真 bug 掩盖掉；查不出原因就报 BLOCKED，不要改断言）

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/logic/chart_draw.c firmware/components/wesr/include/wesr_logic.h firmware/test
git commit -m "feat(wesr): 1-bit 分时光栅化（实线/虚线/斜纹/量柱）"
```

---

### Task 9: 配置存储与默认值

**Files:**
- Create: `firmware/components/wesr/app/cfg_store.c`（NVS 读写）
- Create: `firmware/components/wesr/logic/defaults.c`（纯逻辑：默认配置 + 代码校验）
- Modify: `firmware/components/wesr/include/wesr_logic.h`、`firmware/test/test_logic.c`

**Interfaces:**
- Produces:
  - `typedef struct { char ssid[33], pass[65]; wesr_stock_cfg_t stocks[WESR_MAX_STOCKS]; uint8_t count; uint16_t refresh_sec; } wesr_app_cfg_t;`
  - `void wesr_cfg_defaults(wesr_app_cfg_t *cfg)`（纯逻辑，宿主机可测）
  - `bool wesr_code_valid(const char *code)`（`sh|sz|bj` + 6 位数字）
  - `esp_err_t wesr_cfg_load(wesr_app_cfg_t *cfg)` / `esp_err_t wesr_cfg_save(const wesr_app_cfg_t *cfg)`（NVS，namespace `wesr`）

- [ ] **Step 1: 写失败的测试（默认值与代码校验）**

```c
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
```

- [ ] **Step 2: 实现纯逻辑部分**

`firmware/components/wesr/logic/defaults.c`：

```c
#include "wesr_logic.h"
#include <ctype.h>
#include <string.h>

static const struct { const char *code; const char *name; } k_defaults[WESR_MAX_STOCKS] = {
    { "sh600519", "贵州茅台" }, { "sz300750", "宁德时代" },
    { "sz002594", "比亚迪"   }, { "sh600036", "招商银行" },
    { "sh601318", "中国平安" }, { "sh000300", "沪深300"  },
    { "sz000858", "五粮液"   }, { "sh600900", "长江电力" },
};

void wesr_cfg_defaults(wesr_app_cfg_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->refresh_sec = 15;
    cfg->count = WESR_MAX_STOCKS;
    for (int i = 0; i < WESR_MAX_STOCKS; i++) {
        strncpy(cfg->stocks[i].code, k_defaults[i].code, WESR_CODE_LEN - 1);
        strncpy(cfg->stocks[i].name, k_defaults[i].name, WESR_NAME_LEN - 1);
    }
    wesr_make_marks(cfg->stocks, cfg->count);
    /* WiFi 默认值来自 menuconfig（见 Step 3），这里留空由调用方填 */
}

bool wesr_code_valid(const char *code)
{
    if (!code || strlen(code) != 8) return false;
    const char *mkt = code;
    if (!(strncmp(mkt, "sh", 2) == 0 || strncmp(mkt, "sz", 2) == 0 ||
          strncmp(mkt, "bj", 2) == 0)) return false;
    for (int i = 2; i < 8; i++) if (!isdigit((unsigned char)code[i])) return false;
    return true;
}
```

`wesr_logic.h` 追加：

```c
typedef struct {
    char ssid[33];
    char pass[65];
    wesr_stock_cfg_t stocks[WESR_MAX_STOCKS];
    uint8_t count;
    uint16_t refresh_sec;
} wesr_app_cfg_t;

void wesr_cfg_defaults(wesr_app_cfg_t *cfg);
bool wesr_code_valid(const char *code);
```

- [ ] **Step 3: 实现 NVS 部分**

`firmware/components/wesr/app/cfg_store.c`：用 `nvs_open("wesr", NVS_READWRITE, &h)`，把整个 `wesr_app_cfg_t` 当一个 blob 存取；版本号用 key `"ver"`（uint8），结构体变化时靠它判断是否需要写默认值。

```c
#include "wesr_logic.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

#define CFG_KEY "cfg"
#define CFG_VER_KEY "ver"
#define CFG_VERSION 1

esp_err_t wesr_cfg_load(wesr_app_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wesr", NVS_READONLY, &h);
    if (err != ESP_OK) { wesr_cfg_defaults(cfg); return err; }
    uint8_t ver = 0;
    size_t len = sizeof *cfg;
    err = nvs_get_u8(h, CFG_VER_KEY, &ver);
    if (err == ESP_OK && ver == CFG_VERSION) err = nvs_get_blob(h, CFG_KEY, cfg, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof *cfg) { wesr_cfg_defaults(cfg); return ESP_ERR_NVS_NOT_FOUND; }
    return ESP_OK;
}

esp_err_t wesr_cfg_save(const wesr_app_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wesr", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, CFG_KEY, cfg, sizeof *cfg);
    if (err == ESP_OK) err = nvs_set_u8(h, CFG_VER_KEY, CFG_VERSION);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
```

- [ ] **Step 4: 跑测试 + 编译**

Run: `cd firmware; mingw32-make -C test run && idf.py build`
Expected: 测试 PASS；固件编译通过

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/app/cfg_store.c firmware/components/wesr/logic/defaults.c firmware/components/wesr/include/wesr_logic.h firmware/test
git commit -m "feat(wesr): 配置默认值、代码校验与 NVS 读写"
```

---

### Task 10: 板级初始化 + UI 骨架（顶栏 + 页面容器）

**Files:**
- Create: `firmware/components/wesr/port/board_init.cpp`（唯一写 C++ 的地方 —— 官方 BSP 是 C++ 类）
- Create: `firmware/components/wesr/ui/ui_theme.h`、`firmware/components/wesr/ui/ui_root.c`
- Modify: `firmware/main/main.cpp`（改成调 `Board_Init()` + `Ui_Init()`）

**Interfaces:**
- Consumes: 官方 `DisplayPort`、`Lvgl_PortInit/Lvgl_lock/Lvgl_unlock`、`I2cMasterBus`、`Adc_PortInit`、`Rtc_Setup`
- Produces:
  - `extern "C" void Board_Init(void)` / `extern "C" I2cMasterBus *Board_I2c(void)`
  - `void Ui_Init(void)` / `void Ui_UpdateHeader(const wesr_header_t *h)` / `void Ui_ShowPage(uint8_t page)`

**关键事实（照抄官方，不自己发明）**：
- 官方 `main.cpp` 是 `DisplayPort RlcdPort(12,11,5,40,41,400,300)`（参数顺序 mosi, scl, dc, cs, rst, w, h），flush 回调把 16 位色按 `*buffer < 0x7fff ? 黑 : 白` 阈值化后 `RLCD_SetPixel`，最后 `RLCD_Display()`。
- 官方 `Lvgl_PortInit()` 已经建了 tick 定时器、互斥锁、LVGL 任务线程和两块 PSRAM 全屏缓冲 —— **不要再初始化一遍**。
- 官方 `I2cMasterBus` 构造是 `(scl, sda, port)`，我们用 `(14, 13, 0)`。

- [ ] **Step 1: 写板级初始化**

`firmware/components/wesr/port/board_init.cpp`：

```cpp
#include "board_init.h"
#include "display_bsp.h"
#include "i2c_bsp.h"
#include "lvgl_bsp.h"
#include "adc_bsp.h"
#include "i2c_equipment.h"
#include "user_config.h"

static DisplayPort *s_lcd = nullptr;
static I2cMasterBus *s_i2c = nullptr;

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint16_t *buffer = (uint16_t *)color_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            s_lcd->RLCD_SetPixel(x, y, (*buffer < 0x7fff) ? ColorBlack : ColorWhite);
            buffer++;
        }
    }
    s_lcd->RLCD_Display();
    lv_disp_flush_ready(drv);
}

extern "C" void Board_Init(void)
{
    static I2cMasterBus i2c(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, 0);
    s_i2c = &i2c;
    static DisplayPort lcd(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN, RLCD_CS_PIN,
                           RLCD_RST_PIN, 400, 300);
    s_lcd = &lcd;
    lcd.RLCD_Init();
    Lvgl_PortInit(400, 300, Lvgl_FlushCallback);
    Adc_PortInit();
    Rtc_Setup(&i2c, 0x51);
}

extern "C" I2cMasterBus *Board_I2c(void) { return s_i2c; }
```

`firmware/components/wesr/include/board_init.h`：

```c
#pragma once
#ifdef __cplusplus
class I2cMasterBus;
extern "C" {
#else
typedef struct I2cMasterBus I2cMasterBus;
#endif
void Board_Init(void);
I2cMasterBus *Board_I2c(void);
#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: 写 UI 主题常量与骨架**

`ui_theme.h`（把设计稿的尺寸固化成常量，后面所有页面只引用这里）：

```c
#pragma once
#define UI_W        400
#define UI_H        300
#define UI_HEADER_H 24
#define UI_BAR_H    14          /* 状态条 */
#define UI_P1_LEFT_W 300
#define UI_P1_RIGHT_W 100
#define UI_P1_ROWS  8
#define UI_Q_W      200
#define UI_Q_H      138
#define UI_TREND_W  184
#define UI_TREND_H  86
#define UI_BIG_W    380
#define UI_BIG_H    168
LV_FONT_DECLARE(font_num78);
LV_FONT_DECLARE(font_num25);
LV_FONT_DECLARE(font_num19);
LV_FONT_DECLARE(font_num16);
LV_FONT_DECLARE(font_num14);
LV_FONT_DECLARE(font_num12);
LV_FONT_DECLARE(font_num11);
LV_FONT_DECLARE(font_num9);
LV_FONT_DECLARE(font_cn16);
```

`ui_root.c` 建顶栏 + 状态条 + 页面容器：

```c
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_pages.h"

static lv_obj_t *s_header, *s_dots[4], *s_bank[2], *s_batt_label, *s_bar, *s_pages[4];

static void build_header(void)
{
    s_header = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_header, UI_W, UI_HEADER_H);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_style_border_width(s_header, 0, 0);
    lv_obj_set_style_border_side(s_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_header, 1, 0);
    lv_obj_set_style_radius(s_header, 0, 0);
    lv_obj_set_style_bg_color(s_header, lv_color_white(), 0);
    lv_obj_set_style_pad_all(s_header, 0, 0);
    lv_obj_clear_flag(s_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *wifi = lv_label_create(s_header);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);          /* 内置符号字体，省一份图标资源 */
    lv_obj_set_style_text_font(wifi, &lv_font_montserrat_14, 0);
    lv_obj_align(wifi, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *bt = lv_label_create(s_header);
    lv_label_set_text(bt, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(bt, &lv_font_montserrat_14, 0);
    lv_obj_align(bt, LV_ALIGN_LEFT_MID, 22, 0);

    for (int i = 0; i < 4; i++) {
        s_dots[i] = lv_obj_create(s_header);
        lv_obj_set_size(s_dots[i], 5, 5);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dots[i], 1, 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_black(), 0);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_white(), 0);
        lv_obj_set_pos(s_dots[i], 150 + i * 8, 10);
    }
    for (int i = 0; i < 2; i++) {
        s_bank[i] = lv_obj_create(s_header);
        lv_obj_set_size(s_bank[i], 7, 3);
        lv_obj_set_style_radius(s_bank[i], 0, 0);
        lv_obj_set_style_border_width(s_bank[i], 1, 0);
        lv_obj_set_style_bg_color(s_bank[i], lv_color_white(), 0);
        lv_obj_set_pos(s_bank[i], 190 + i * 9, 11);
    }
    s_batt_label = lv_label_create(s_header);
    lv_obj_set_style_text_font(s_batt_label, &font_num11, 0);
    lv_obj_align(s_batt_label, LV_ALIGN_RIGHT_MID, -26, 0);
    lv_obj_t *bat = lv_label_create(s_header);
    lv_label_set_text(bat, LV_SYMBOL_BATTERY_3);
    lv_obj_align(bat, LV_ALIGN_RIGHT_MID, -6, 0);
}

static void build_statusbar(void)
{
    s_bar = lv_label_create(lv_scr_act());
    lv_obj_set_size(s_bar, UI_W, UI_BAR_H);
    lv_obj_set_pos(s_bar, 0, UI_HEADER_H);
    lv_obj_set_style_text_align(s_bar, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_bar, &font_cn16, 0);
    lv_obj_set_style_border_width(s_bar, 1, 0);
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);        /* 没状态时不占位 */
}

void Ui_ShowPage(uint8_t page)
{
    for (int i = 0; i < 4; i++) {
        bool on = (i + 1) == page;
        lv_obj_set_style_bg_color(s_dots[i], on ? lv_color_black() : lv_color_white(), 0);
        if (s_pages[i]) {
            if (on) lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void Ui_Init(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), 0);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    build_header();
    build_statusbar();
    s_pages[0] = Ui_Page1Create();
    s_pages[1] = Ui_Page2Create();
    s_pages[2] = Ui_Page3Create();
    s_pages[3] = Ui_Page4Create();
    Ui_ShowPage(1);
}
```

`ui_pages.h` 声明四页的创建/更新函数（各页在 Task 11–13 实现）：

```c
#pragma once
#include "lvgl.h"
#include "wesr_logic.h"

lv_obj_t *Ui_Page1Create(void);
void      Ui_Page1Update(const wesr_quote_t *q, const wesr_stock_cfg_t *cfg, uint8_t count);
lv_obj_t *Ui_Page2Create(void);
void      Ui_Page2Update(const wesr_minute_t *m, const wesr_quote_t *q,
                         const wesr_stock_cfg_t *cfg, uint8_t start);
lv_obj_t *Ui_Page3Create(void);
void      Ui_Page3Update(const wesr_minute_t *m, const wesr_quote_t *q,
                         const wesr_stock_cfg_t *cfg, uint8_t idx, uint8_t count);
lv_obj_t *Ui_Page4Create(void);
void      Ui_Page4Update(const struct wesr_status *st);

void Ui_StatusBar(const char *text, bool alert);   /* alert=true → 反白 */
```

- [ ] **Step 3: 改 main.cpp**

```cpp
extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    Board_Init();
    if (Lvgl_lock(-1)) { Ui_Init(); Lvgl_unlock(); }
    ESP_LOGI("wesr", "ui up");
    while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
```

- [ ] **Step 4: 编译并上板**

Run: `cd firmware; idf.py build; idf.py -p COM3 flash monitor`
Expected: 屏幕上出现 24px 顶栏（WiFi 图标、蓝牙图标、4 个页码点、2 个组方块、电量文本 + 电池图标），下面 3 个空页面占位；串口打印 `ui up`，无重启。

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/port firmware/components/wesr/ui firmware/components/wesr/include firmware/main/main.cpp
git commit -m "feat(wesr): 板级初始化与 UI 骨架（顶栏/状态条/页面容器）"
```

---

### Task 11: 中英文字体 + 第 1 页（时钟看盘）

**Files:**
- Create: `firmware/tools/gen_fonts.ps1`（生成脚本，**生成物不入 git**）
- Create: `firmware/components/wesr/ui/font_num*.c`、`font_cn16.c`（生成物，gitignore）
- Create: `firmware/components/wesr/ui/ui_page1.c`
- Modify: `.gitignore`、`firmware/components/wesr/CMakeLists.txt`（加 SRCS）

**Interfaces:**
- Consumes: `Ui_Page1Create/Ui_Page1Update`（Task 10 声明）、`wesr_fmt_price/wesr_fmt_pct/wesr_price_font_px`（Task 4）
- Produces: 第 1 页完整实现

**字体策略（含两处对设计稿的有意简化）**：
- 中文只用 **一个字号 16px**（设计稿里第 2 页名称 14px / 第 3 页 17px 都折到 16px）—— 位图字体不能平滑缩放，多一档就多一份全字库。
- 用系统自带 `msyh.ttc` 生成 **全 CJK 区段**（`0x4E00-0x9FA5`，约 2.1 万字 → 16px 1bpp ≈ 672KB Flash，16MB 里够用），这样小程序以后填任何股票名都不会缺字；生成物按字体许可**不提交仓库**，用脚本重建。
- 数字/符号单独生成小字号字体（`0-9 . % + - : ° ▲▼—`），避免为几个字符再吃一份中文全字库。

- [ ] **Step 1: 字体生成脚本**

`firmware/tools/gen_fonts.ps1`：

```powershell
$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot '..\components\wesr\ui'
$sys = 'C:\Windows\Fonts\msyh.ttc'          # 微软雅黑，本地自用；生成的 .c 不入仓库
$num  = '0123456789.:%+-'

# 中文（全 CJK 区段，16px）
npx --yes lv_font_conv --font $sys --size 16 --bpp 1 --no-compress `
  --range 0x4E00-0x9FA5 --symbols "▲▼—°" `
  --format lvgl --lv-include lvgl.h -o "$out\font_cn16.c" --force-fast-kern-format

# 数字/符号各字号
foreach ($sz in 78,25,19,16,14,12,11,9) {
  npx --yes lv_font_conv --font $sys --size $sz --bpp 1 --no-compress `
    --symbols $num --format lvgl --lv-include lvgl.h `
    -o "$out\font_num$sz.c" --force-fast-kern-format
}
```

Generator 里 `--symbols` 用的是字符串，符号 `▲▼—` 直接从命令行传（源码文件是 UTF-8）。

`.gitignore` 追加：

```
firmware/components/wesr/ui/font_*.c
```

Run: `powershell -File firmware/tools/gen_fonts.ps1`
Expected: 生成 8 个 `.c` 文件；`font_cn16.c` 约 2–4MB 源码（编译慢是正常的）。

- [ ] **Step 2: 把字体加进组件**

`firmware/components/wesr/CMakeLists.txt` 的 `SRCS` 加上：

```cmake
    "ui/font_cn16.c"
    "ui/font_num78.c" "ui/font_num25.c" "ui/font_num19.c"
    "ui/font_num16.c" "ui/font_num14.c" "ui/font_num12.c"
    "ui/font_num11.c" "ui/font_num9.c"
```

另外把 `components/wesr/ui` 也加进 `INCLUDE_DIRS`，让 `LV_FONT_DECLARE` 能找到。

- [ ] **Step 3: 实现第 1 页**

`firmware/components/wesr/ui/ui_page1.c`（布局严格按 spec §7：左 300 / 右 100）：

```c
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_pages.h"
#include "ui_page1.h"
#include <string.h>

static lv_obj_t *s_clock, *s_date, *s_temp, *s_humi;
static lv_obj_t *s_mark[UI_P1_ROWS], *s_pct[UI_P1_ROWS], *s_price[UI_P1_ROWS];

lv_obj_t *Ui_Page1Create(void)
{
    lv_obj_t *page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(page, UI_W, UI_H - UI_HEADER_H);
    lv_obj_set_pos(page, 0, UI_HEADER_H);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* 左栏 300px */
    lv_obj_t *left = lv_obj_create(page);
    lv_obj_set_size(left, UI_P1_LEFT_W, LV_PCT(100));
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_style_radius(left, 0, 0);
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    s_clock = lv_label_create(left);
    lv_obj_set_style_text_font(s_clock, &font_num78, 0);
    lv_label_set_text(s_clock, "09:41");
    lv_obj_set_pos(s_clock, 14, 16);

    s_date = lv_label_create(left);
    lv_obj_set_style_text_font(s_date, &font_cn16, 0);
    lv_label_set_text(s_date, "9月27日");
    lv_obj_set_pos(s_date, 16, 104);

    lv_obj_t *temp_l = lv_label_create(left);
    lv_obj_set_style_text_font(temp_l, &font_cn16, 0);
    lv_label_set_text(temp_l, "温度");
    lv_obj_set_pos(temp_l, 30, 252);
    s_temp = lv_label_create(left);
    lv_obj_set_style_text_font(s_temp, &font_num19, 0);
    lv_label_set_text(s_temp, "--.-");
    lv_obj_set_pos(s_temp, 18, 272);

    lv_obj_t *humi_l = lv_label_create(left);
    lv_obj_set_style_text_font(humi_l, &font_cn16, 0);
    lv_label_set_text(humi_l, "湿度");
    lv_obj_set_pos(humi_l, 178, 252);
    s_humi = lv_label_create(left);
    lv_obj_set_style_text_font(s_humi, &font_num19, 0);
    lv_label_set_text(s_humi, "--");
    lv_obj_set_pos(s_humi, 168, 272);

    /* 右栏 100px：8 行，行间 1px 分隔线 */
    lv_obj_t *right = lv_obj_create(page);
    lv_obj_set_size(right, UI_P1_RIGHT_W, LV_PCT(100));
    lv_obj_set_pos(right, UI_P1_LEFT_W, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_radius(right, 0, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < UI_P1_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(right);
        lv_obj_set_size(row, UI_P1_RIGHT_W, (UI_H - UI_HEADER_H) / UI_P1_ROWS);
        lv_obj_set_pos(row, 0, i * ((UI_H - UI_HEADER_H) / UI_P1_ROWS));
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        if (i < UI_P1_ROWS - 1) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row, 1, 0);
        } else {
            lv_obj_set_style_border_width(row, 0, 0);
        }
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        s_mark[i] = lv_label_create(row);
        lv_obj_set_style_text_font(s_mark[i], &font_cn16, 0);
        lv_obj_set_pos(s_mark[i], 7, 2);

        s_pct[i] = lv_label_create(row);
        lv_obj_set_style_text_font(s_pct[i], &font_num9, 0);
        lv_obj_align(s_pct[i], LV_ALIGN_TOP_RIGHT, -6, 5);

        s_price[i] = lv_label_create(row);
        lv_obj_set_style_text_font(s_price[i], &font_num14, 0);
        lv_obj_set_pos(s_price[i], 7, 18);
    }
    return page;
}

void Ui_Page1SetClock(const char *hhmm, const char *date)
{
    lv_label_set_text(s_clock, hhmm);
    lv_label_set_text(s_date, date);
}

void Ui_Page1SetEnv(float temp_c, float humi_pct)
{
    char b[16];
    snprintf(b, sizeof b, "%.1f", temp_c);
    lv_label_set_text(s_temp, b);
    snprintf(b, sizeof b, "%.0f%%", humi_pct);
    lv_label_set_text(s_humi, b);
}

void Ui_Page1Update(const wesr_quote_t *q, const wesr_stock_cfg_t *cfg, uint8_t count)
{
    char b[32];
    for (int i = 0; i < UI_P1_ROWS; i++) {
        if (i >= count || !q) {
            lv_label_set_text(s_mark[i], "");
            lv_label_set_text(s_pct[i], "");
            lv_label_set_text(s_price[i], "");
            continue;
        }
        lv_label_set_text(s_mark[i], cfg[i].mark);
        wesr_fmt_pct(b, sizeof b, q[i].chg_pct, q[i].valid);
        lv_label_set_text(s_pct[i], b);
        wesr_fmt_price(b, sizeof b, q[i].last, q[i].valid);
        lv_label_set_text(s_price[i], b);
        lv_obj_set_style_text_font(s_price[i],
            q[i].valid && wesr_price_font_px(q[i].last) == 12 ? &font_num12 : &font_num14, 0);
    }
}
```

- [ ] **Step 4: 编译并上板看第 1 页**

Run: `cd firmware; idf.py build; idf.py -p COM3 flash monitor`
Expected: 左栏 300px 显示 78px 大字时间 + 日期 + 温度/湿度占位（`--.-`）；右栏 100px 有 8 行、行间有细线、每行是「首字（假数据先留空）+ 涨跌」与「价格」。

- [ ] **Step 5: Commit**

```bash
git add .gitignore firmware/tools/gen_fonts.ps1 firmware/components/wesr/ui/ui_page1.c firmware/components/wesr/include/ui_page1.h firmware/components/wesr/CMakeLists.txt
git commit -m "feat(wesr): 中英文字体生成脚本与第 1 页时钟看盘"
```

---

### Task 12: 分时图控件 + 第 2、3 页

**Files:**
- Create: `firmware/components/wesr/ui/ui_trend.c`、`firmware/components/wesr/ui/ui_page2.c`、`firmware/components/wesr/ui/ui_page3.c`
- Modify: `firmware/components/wesr/CMakeLists.txt`

**Interfaces:**
- Consumes: `wesr_chart_render`（Task 8）、`Ui_Page2Create/Update`、`Ui_Page3Create/Update`（Task 10 声明）
- Produces: `lv_obj_t *Ui_TrendCreate(lv_obj_t *parent, int w, int h, bool hatch, bool volume, bool grid)` 与 `void Ui_TrendRender(lv_obj_t *trend, const wesr_minute_t *m, float prev_close)`

- [ ] **Step 1: 画布控件**

`ui_trend.c`：用 LVGL canvas + `LV_IMG_CF_ALPHA_1BIT`，把 Task 8 的位图直接写进 canvas 缓冲。

```c
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_pages.h"
#include "wesr_logic.h"
#include <stdlib.h>

typedef struct {
    lv_obj_t *canvas;
    uint8_t *buf;
    int w, h;
    bool hatch, volume, grid;
} ui_trend_t;

static ui_trend_t *s_trends[8];
static int s_trend_n;

lv_obj_t *Ui_TrendCreate(lv_obj_t *parent, int w, int h, bool hatch, bool volume, bool grid)
{
    ui_trend_t *t = calloc(1, sizeof *t);
    t->w = w; t->h = h; t->hatch = hatch; t->volume = volume; t->grid = grid;
    t->buf = heap_caps_calloc(1, (size_t)((w + 7) / 8) * h, MALLOC_CAP_SPIRAM);
    t->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(t->canvas, t->buf, w, h, LV_IMG_CF_ALPHA_1BIT);
    s_trends[s_trend_n++] = t;
    return t->canvas;
}

void Ui_TrendRender(lv_obj_t *canvas, const wesr_minute_t *m, float prev_close)
{
    for (int i = 0; i < s_trend_n; i++) {
        if (s_trends[i]->canvas != canvas) continue;
        ui_trend_t *t = s_trends[i];
        wesr_bmp_t b;
        wesr_bmp_init(&b, t->buf, t->w, t->h);
        wesr_chart_opts_t o = {
            .pad_top = 8, .pad_bottom = 5, .pad_x = 2,
            .price_h = t->volume ? t->h - 52 : t->h - 10,
            .vol_top = t->h - 40, .vol_h = t->volume ? 20 : 0,
            .hatch = t->hatch, .grid = t->grid, .span_ratio = 1.1f,
        };
        if (m && m->valid) wesr_chart_render(&b, m, prev_close, &o);
        lv_obj_invalidate(canvas);
        return;
    }
}
```

> 上板第一件事就是确认 `LV_IMG_CF_ALPHA_1BIT` 画出来是清晰的 1 位图案（斜纹是斜纹、不是灰块）。若不对，改 `lv_canvas_set_buffer(..., LV_IMG_CF_TRUE_COLOR_ALPHA)`，`buf` 改成 `w*h*3` 字节（PSRAM 191KB）并在渲染后把 1bpp 展开成 3 字节像素（多写 20 行）；这是这一段唯一有不确定性的地方，所以单独抽成一个函数。

- [ ] **Step 2: 第 2 页四宫格**

`ui_page2.c`：2×2 网格，每格 200×138，格线用容器边框；每格内容 = 名称 16px + 涨跌 9px + 价格 16px + 分时画布 184×86。`Ui_Page2Update(m, q, cfg, start)` 里 `start` 是组起点（0 或 4），逐格调用 `Ui_TrendRender`，`count - start` 之外的格子留空（**不要画空坐标系**，边界规则 4）。

关键位置（照抄设计稿）：

```c
    int idx = start + cell;
    if (idx >= count) { /* 整格留空 */ return; }
    lv_label_set_text(o->name, cfg[idx].name);
    wesr_fmt_pct(b, sizeof b, q[idx].chg_pct, q[idx].valid);
    lv_label_set_text(o->pct, b);
    wesr_fmt_price(b, sizeof b, q[idx].last, q[idx].valid);
    lv_label_set_text(o->price, b);
    Ui_TrendRender(o->canvas, &m[cell], q[idx].prev_close);
```

- [ ] **Step 3: 第 3 页个股分时**

`ui_page3.c`：名称 16px + `n/8` 9px + 现价 25px + 昨收 9px + 图例（实线=价格、虚线=均价、虚线=昨收，用三个 16px 宽的小线段 + 9px 标签）+ 大画布 380×168（`hatch=true, volume=true, grid=true`）+ 底部 5 个时间标签（09:30 / 10:30 / 11:30·13:00 / 14:00 / 15:00，按 `wesr_index_to_x(idx, 0, 380)` 绝对定位）+ 开高低收量一行。

- [ ] **Step 4: 编译上板**

Run: `cd firmware; idf.py build; idf.py -p COM3 flash monitor`
Expected: 第 2 页四宫格假数据分时形状正确、昨收虚线居中；第 3 页大图能看到斜纹填充、均价虚线、量柱、时间轴标签；**斜纹必须是斜线而不是灰块**（ALPHA_1BIT 验证点）。

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/ui/ui_trend.c firmware/components/wesr/ui/ui_page2.c firmware/components/wesr/ui/ui_page3.c firmware/components/wesr/CMakeLists.txt
git commit -m "feat(wesr): 分时画布控件与第 2、3 页"
```

---

### Task 13: 共享状态 + 第 4 页系统状态 + 状态条

**Files:**
- Create: `firmware/components/wesr/include/app_state.h`、`firmware/components/wesr/app/app_state.c`
- Create: `firmware/components/wesr/ui/ui_page4.c`

**Interfaces:**
- Produces:
  - `typedef struct { ... } wesr_status_t;`（字段见下）
  - `void AppState_Lock(void)` / `void AppState_Unlock(void)` / `wesr_status_t *AppState_Status(void)`
  - `void AppState_SetPages(const wesr_app_cfg_t *cfg, const wesr_nav_t *nav)`
  - `Ui_Page4Update(const wesr_status_t *st)`、`Ui_StatusBar(const char *text, bool alert)`

- [ ] **Step 1: 状态结构**

```c
typedef struct {
    /* 网络 */
    bool wifi_connected; char ssid[33]; char ip[25]; int8_t rssi; bool bt_connected;
    /* 电源与环境 */
    float battery_v; uint8_t battery_pct; bool charging; float temp_c, humi_pct; bool sd_mounted;
    /* 行情 */
    bool offline; bool closed; uint32_t data_day; uint32_t last_ok_ms;
    uint32_t today_fail; uint16_t refresh_sec;
    uint8_t stock_count, page, group, idx;
    /* 系统 */
    char fw[16]; uint32_t uptime_s; uint32_t heap_kb, psram_kb;
} wesr_status_t;
```

`app_state.c` 用一个 `SemaphoreHandle_t` 保护这个结构；`AppState_Status()` 返回指针（调用方必须在 `AppState_Lock/Unlock` 之间使用）。**晚到的风险**：LVGL 线程和网络线程是不同任务，必须成对加锁，不许在锁外读。

- [ ] **Step 2: 第 4 页 + 状态条**

`ui_page4.c` 用双栏（各 190px）渲染 spec §7 的 11 行，行格式 = 左标签 10px + 右值 10px 加粗；`Ui_Page4Update` 每行 `snprintf` 填值。

`Ui_StatusBar(text, alert)` 实现（这是方案 C 的落点）：

```c
void Ui_StatusBar(const char *text, bool alert)
{
    if (!text || !text[0]) { lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_bar, text);
    lv_obj_set_style_bg_opa(s_bar, alert ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_black(), 0);
    lv_obj_set_style_text_color(s_bar, alert ? lv_color_white() : lv_color_black(), 0);
}
```

- [ ] **Step 3: 编译上板**

Run: `cd firmware; idf.py build; idf.py -p COM3 flash monitor`
Expected: 第 4 页双栏字段齐全（未联网/未刷卡等先显示默认值）；用临时 `Ui_StatusBar("休市 · 显示 09-24 收盘数据", false)` 验证细线状态条，用 `(..., true)` 验证反白状态条，然后删掉临时调用。

- [ ] **Step 4: Commit**

```bash
git add firmware/components/wesr/include/app_state.h firmware/components/wesr/app/app_state.c firmware/components/wesr/ui/ui_page4.c
git commit -m "feat(wesr): 共享状态、第 4 页系统状态与状态条"
```

---

### Task 14: 传感器任务（SHTC3 + 电池）

**Files:**
- Create: `firmware/components/wesr/app/sensor_task.c`

**Interfaces:**
- Consumes: 官方 `Shtc3Port`（`port_bsp/i2c_equipment.h`）、`Adc_GetBatteryVoltage()` / `Adc_GetBatteryLevel()`（`port_bsp/adc_bsp.h`）、`Board_I2c()`（Task 10）
- Produces: `void Sensor_TaskStart(void)`；每 10 秒更新 AppState 的 `temp_c/humi_pct/battery_v/battery_pct/charging`

- [ ] **Step 1: 实现**

```c
#include "app_state.h"
#include "i2c_equipment.h"
#include "adc_bsp.h"
#include "board_init.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void sensor_task(void *arg)
{
    Shtc3Port shtc3(*Board_I2c());
    shtc3.Shtc3_Wakeup();
    float prev_v = 0.0f;
    while (true) {
        float t = 0, h = 0;
        if (shtc3.Shtc3_ReadTempHumi(&t, &h) == 0) {      /* 0 = 成功 */
            AppState_Lock();
            AppState_Status()->temp_c = t;
            AppState_Status()->humi_pct = h;
            AppState_Unlock();
        }
        float v = Adc_GetBatteryVoltage();
        uint8_t pct = Adc_GetBatteryLevel();
        AppState_Lock();
        wesr_status_t *st = AppState_Status();
        /* 板子读不到 CHG 引脚，充电状态用"电压在涨"推断（ponytail: 粗略启发式，
           够用即可；要精确就找 CHG 测试点接 ADC） */
        st->charging = (prev_v > 0.0f && v > prev_v + 0.01f);
        st->battery_v = v;
        st->battery_pct = pct;
        st->sd_mounted = false;                            /* v1 不用 SD，第 4 页显示"未插入" */
        AppState_Unlock();
        prev_v = v;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void Sensor_TaskStart(void)
{
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 3, NULL);
}
```

- [ ] **Step 2: 编译上板**

Run: `cd firmware; idf.py build; idf.py -p COM3 flash monitor`
Expected: 串口每 10 秒能打印/或第 4 页显示温湿度与电池电压；用手捂住 SHTC3，湿度读数上升；插/拔 USB 时 `charging` 会跟着变（启发式，允许有延迟）。

- [ ] **Step 3: Commit**

```bash
git add firmware/components/wesr/app/sensor_task.c
git commit -m "feat(wesr): 温湿度与电池采集任务"
```

---

### Task 15: 联网 + 行情服务（真实数据上屏）

**Files:**
- Create: `firmware/components/wesr/app/net_task.c`、`firmware/components/wesr/app/quote_service.c`
- Modify: `firmware/components/wesr/include/app_state.h`（加缓存字段：`wesr_quote_t quotes[8]`、`wesr_minute_t minutes[4]`、`uint32_t minutes_ms`）

**Interfaces:**
- Consumes: `wesr_sched_*`（Task 7）、`wesr_parse_quote_line`（Task 2）、`wesr_parse_minute_json` + `wesr_parse_minute_meta`（Task 3/3b）、`wesr_nav_group_start`（Task 6）
- Produces: `void Net_TaskStart(void)`；`bool Quote_RefreshNow(wesr_fetch_t what, const wesr_app_cfg_t *cfg, const wesr_nav_t *nav)`

**实测约束（照抄 spec §4）**：快照走 `https://qt.gtimg.cn/q=<8 个 code 逗号分隔>`；分时走 `https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=<单个 code>`，**HTTP 会 302 到 HTTPS，所以直接写 https**；两者都要 `esp_crt_bundle_attach`。

- [ ] **Step 1: WiFi + SNTP + RTC**

```c
#include "esp_wifi_bsp.h"
#include "esp_sntp.h"
#include "i2c_equipment.h"
#include <time.h>

static void on_time_synced(struct timeval *tv)
{
    struct tm t;
    localtime_r(&tv->tv_sec, &t);
    Rtc_SetTime((uint16_t)(t.tm_year + 1900), (uint8_t)(t.tm_mon + 1), (uint8_t)t.tm_mday,
                (uint8_t)t.tm_hour, (uint8_t)t.tm_min, (uint8_t)t.tm_sec);
}

void Net_StartWifi(void)
{
    espwifi_init();
    xEventGroupWaitBits(wifi_even_, 0x01, pdTRUE, pdTRUE, pdMS_TO_TICKS(20000));
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(on_time_synced);
    esp_sntp_init();
}
```

- [ ] **Step 2: HTTP 拉取（PSRAM 缓冲 + 位置解析）**

```c
static char *http_get(const char *url, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "Mozilla/5.0",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    char *buf = NULL;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int len = esp_http_client_fetch_headers(c);
        if (len > 0 && len < 65536) {
            buf = heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM);
            if (buf) {
                int got = esp_http_client_read(c, buf, len);
                buf[got > 0 ? got : 0] = 0;
                if (got <= 0) { free(buf); buf = NULL; }
                else if (out_len) *out_len = got;
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}
```

- [ ] **Step 3: 按页拉取 + 发布**

`quote_service.c` 的循环（200ms tick）：

```c
void net_loop(void *arg)
{
    wesr_sched_t sched;
    wesr_sched_init(&sched, g_cfg.refresh_sec);
    uint32_t last_page = 0;
    for (;;) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint16_t hhmm = local_hhmm();
        AppState_Lock();
        uint8_t page = AppState_Status()->page;
        bool srv_closed = AppState_Status()->closed;
        AppState_Unlock();
        if (page != last_page) { wesr_sched_page(&sched, page); last_page = page; }

        bool may_fetch = wesr_in_trading(hhmm) && !srv_closed;
        wesr_fetch_t what = wesr_sched_tick(&sched, now, may_fetch);
        if (what == WESR_FETCH_QUOTES) {
            bool ok = fetch_quotes(&g_cfg, &sched, now);
            wesr_sched_result(&sched, ok, now);
        } else if (what == WESR_FETCH_MINUTES) {
            bool ok = fetch_minutes(&g_cfg, &g_nav, &sched, now);
            wesr_sched_result(&sched, ok, now);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
```

`fetch_quotes`：拼 `https://qt.gtimg.cn/q=` + 逗号拼接 count 个 code → `http_get` → 按 `\n` 切行，对每行找 `v_<code>=` 前缀匹配下标，`wesr_parse_quote_line` 成功后填 `quotes[i]`；**匹配不到的股票 `valid=false`（规则 6）**。整批只有 1 条成功也算部分成功（`ok = 至少一条成功`）。

`fetch_minutes`：对本页需要的股票逐只 URL 拉取 → `wesr_parse_minute_json` + `wesr_parse_minute_meta` → 填 `minutes[cell]` 与 `quotes[idx]`（分时响应自带快照，**不用再拉一次快照**）；把 `meta.quote.prev_close` 作为画昨收基线的值；`meta.closed` 写回状态并触发状态条。

- [ ] **Step 4: 状态条联动**

```c
    if (st->closed) {
        snprintf(b, sizeof b, "休市 · 显示 %02u-%02u 收盘数据",
                 (st->data_day / 100) % 100, st->data_day % 100);
        Ui_StatusBar(b, false);
    } else if (st->offline) {
        Ui_StatusBar("未联网 · 显示最后数据", true);
    } else {
        Ui_StatusBar("", false);
    }
```

- [ ] **Step 5: 编译上板（关键验收）**

Run: `cd firmware; idf.py build; idf.py -p COM3 flash monitor`
Expected: 第 1 页出现真实的 8 只行情（现价/涨跌/首字），第 3 页出现真实分时曲线；串口能看到请求耗时；因为抓取当天是休市（实测 `qt.market` 报「中秋节休市」），状态条应显示细线的「休市 · 显示 09-24 收盘数据」，且**不发重复请求**。

- [ ] **Step 6: Commit**

```bash
git add firmware/components/wesr/app/net_task.c firmware/components/wesr/app/quote_service.c firmware/components/wesr/include/app_state.h
git commit -m "feat(wesr): 联网、SNTP 校时与按页行情拉取"
```

---

### Task 16: 按键接入 + 冒烟回归 + README

**Files:**
- Create: `firmware/components/wesr/app/input_task.c`
- Modify: `firmware/main/main.cpp`（起各任务）、`firmware/README.md`

**Interfaces:**
- Consumes: `Custom_ButtonInit()`、`GP18ButtonGroups`（官方 `port_bsp/button_bsp.h`，位定义：bit0=单击、bit1=双击、bit2=长按）、`wesr_nav_click/double`（Task 6）
- Produces: `void Input_TaskStart(void)`

- [ ] **Step 1: 按键任务**

**不自己写去抖/单击/双击/长按判定** —— 官方 `multi_button` 已经通过 `GP18ButtonGroups` 报了三种事件，这里只做事件到语义的映射：

```c
#include "button_bsp.h"
#include "app_state.h"
#include "ui_pages.h"
#include "wesr_logic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void input_task(void *arg)
{
    wesr_nav_t *nav = AppState_Nav();
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(GP18ButtonGroups,
            set_bit_button(0) | set_bit_button(1) | set_bit_button(2),
            pdTRUE, pdFALSE, pdMS_TO_TICKS(200));
        if (bits & set_bit_button(2)) {
            Ui_StatusBar("配网模式 · 等待小程序", true);   /* M4 才真正开 BLE */
            continue;
        }
        if (bits & set_bit_button(0)) wesr_nav_click(nav);
        if (bits & set_bit_button(1)) wesr_nav_double(nav);
        if (bits) {
            AppState_Lock();
            AppState_Status()->page = nav->page;
            AppState_Status()->group = nav->group;
            AppState_Status()->idx = nav->idx;
            AppState_Unlock();
            if (Lvgl_lock(100)) { Ui_ShowPage(nav->page); Ui_Refresh(); Lvgl_unlock(); }
        }
    }
}

void Input_TaskStart(void)
{
    Custom_ButtonInit();
    xTaskCreate(input_task, "input", 4096, NULL, 4, NULL);
}
```

- [ ] **Step 2: main 起全部任务**

```cpp
    Board_Init();
    if (Lvgl_lock(-1)) { Ui_Init(); Lvgl_unlock(); }
    Sensor_TaskStart();
    Input_TaskStart();
    Net_TaskStart();          /* 内部先 Net_StartWifi()，再起 net_loop */
    ESP_LOGI("wesr", "all tasks up");
```

- [ ] **Step 3: 跑完整冒烟清单（spec §15 的 8 项）**

逐项记录结果，任何一项不过就停下修：

1. 未配网首屏：状态条反白、时钟在走、温湿度有值、右栏留空
2. 配网（本计划不含 BLE，用 `menuconfig` 里预置 SSID 验证联网路径）：连上后 RTC 被 SNTP 校准
3. 第 1 页：8 只快照、行分隔线、首字正确
4. 第 2 页：四格分时 + 双击切组（组方块跟着变）
5. 第 3 页：分时大图（斜纹/均价/成交量/时间轴）+ 双击换股 + `n/8`
6. 第 4 页：字段齐全，失败计数会动
7. 断网（关路由器）：状态条「未联网」→ 恢复后自动回正常
8. 边界：把 `cfg.count` 临时改成 2、再改成 1 只停牌股，屏幕不崩也不画假线

- [ ] **Step 4: 写 README**

`firmware/README.md` 必须包含：依赖版本（ESP-IDF 5.5.x）、`idf.py set-target esp32s3 && idf.py build flash monitor` 的完整流程、`tools/gen_fonts.ps1` 何时需要重跑（换字号/换字体）、**为什么生成的字体文件不入 git**（字体许可）、`menuconfig` 里 WiFi 默认值的位置、已知限制（无 BLE 配网、无 OTA、SD 未使用）、以及 `references/` 里官方资料的指路。

- [ ] **Step 5: Commit**

```bash
git add firmware/components/wesr/app/input_task.c firmware/main/main.cpp firmware/README.md
git commit -m "feat(wesr): 按键切页换股、任务编排与 README"
```

---

## Self-Review（写完计划后对 spec 的复查）

**1. Spec 覆盖**

| spec 章节 | 落在哪个任务 |
| --- | --- |
| §2 硬件与约束 | Task 10（引脚与屏初始化）、Task 14（传感器/ADC）、Task 15（RTC） |
| §3 架构 | Task 10（BSP 复用）、Task 16（任务编排） |
| §4.1 快照字段 | Task 2（解析）、Task 15（拉取） |
| §4.2 分时字段与均价 | Task 3（分时/均价/每分钟量）、Task 3b（快照与市场状态） |
| §4.3 休市判定 | Task 5（本地时段）、Task 3b（服务端 market）、Task 15（状态条联动） |
| §4.4 容错 | Task 2/3（丢弃坏数据）、Task 7（退避）、Task 15（crt_bundle） |
| §5 刷新策略 | Task 7（调度）、Task 15（按页拉取 + 切页不等网络） |
| §6 数据模型 | Task 2（头文件）、Task 9（配置与 NVS）、Task 13（运行时状态） |
| §7 界面规格 | Task 10（顶栏）、Task 11（第 1 页）、Task 12（第 2/3 页）、Task 13（第 4 页与状态条） |
| §8 交互 | Task 16（按键映射） |
| §9 边界规则 1–14 | 1–5 → Task 6；6 → Task 2/15；7、8、10、13 → Task 4；9 → Task 5/8；11 → Task 8；12 → Task 7；14 → Task 16 |
| §12 时间与 RTC | Task 15（SNTP + RTC 写入；60 秒刷时间在 Task 11 的 UI 更新里） |
| §13 错误处理与可观测 | Task 13（第 4 页）、Task 15（失败计数）；低电闪烁与看门狗 → 见下方"待补" |
| §15 测试与验收 | Task 2–9（宿主机单测）、Task 16（上板冒烟 8 项） |
| §16 里程碑 | M1 ≈ Task 1+10；M2 ≈ Task 2–9+15；M3 ≈ Task 11–13；M4（BLE + 小程序）不在本计划 |

**两处 spec 有、本计划没覆盖的，明确记在这里（下一份计划或后续任务补）**：

- §13 的 `esp_task_wdt` 看门狗与低电反白闪烁 —— 属于打磨项，放实施时最后加。
- **M4（BLE 配网 + 微信小程序）单独一份计划**：本计划产出的是"不依赖小程序就能跑的板子"，协议已在 spec §10 冻结，两边可以并行。

**2. 占位符扫描**：全文没有 TBD/TODO/"类似 Task N"/"自行补充"。每处代码步骤都给了可编译的实现与可运行的命令。

**3. 类型一致性抽查**

- `wesr_quote_t` 的 `stamp` 是 `uint64_t`（`20260924161444` 放不进 uint32），Task 2 定义、Task 15 使用一致。
- `wesr_parse_minute_json(json, out)` 签名在 Task 3b 之后**保持不变**（元数据走独立的 `wesr_parse_minute_meta`），Task 15 的调用点与之一致。
- `wesr_point_t` 含 `vol`（本分钟量），Task 3 的解析器填它、Task 8 的 `wesr_chart_render` 用它画量柱。
- `Ui_Page2Update(..., uint8_t start)` 的 `start` 来自 `wesr_nav_group_start()`（Task 6 返回 0 或 4），Task 12 与 Task 16 用法一致。
- `s_bar` 在 Task 10 创建、Task 13 的 `Ui_StatusBar` 使用，同一个静态变量名。

**4. 两处对设计稿的有意偏离（实施时按这里，不按设计稿）**

- 顶栏的 WiFi / 蓝牙 / 电池图标用 LVGL 内置 `LV_SYMBOL_WIFI` / `LV_SYMBOL_BLUETOOTH` / `LV_SYMBOL_BATTERY_3`，不自己在画布里画 SVG 形状 —— 省一份图标资源，形状与设计稿略有差异。
- 中文只生成 16px 一档（设计稿第 2 页 14px、第 3 页 17px 都折到 16px）—— 位图字体不能平滑缩放，多一档就多一份全字库。
