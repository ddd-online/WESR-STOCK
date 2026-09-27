# WESR-STOCK M4（BLE 配网 + 微信小程序）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 长按 KEY 进配网模式后，手机微信小程序通过 BLE 改 WiFi、改这 8 只股票、对时，板子当场生效并显示。

**Architecture:** 板端用 NimBLE 起一个 Nordic UART Service（NUS）外设，**只在配网模式广播**（其余时间蓝牙关着，省电也不和 WiFi 抢天线）；协议是 UTF-8 JSON + `\n` 结尾，双向固定 20 字节分片。编解码/校验放纯逻辑层（`logic/ble_proto.c`，宿主机可测），命令执行放 app 层（`app/ble_task.cpp`），复用已有的 `net_task` / `cfg_store` / `AppState`。小程序三屏只用 BLE API。

**Tech Stack:** ESP-IDF v6.1 + NimBLE（外设）/ 微信小程序原生 BLE API / 宿主机 gcc + assert 测试（已有 `firmware/test`）/ PC 端自检用 Python + bleak。

**Spec:** `docs/superpowers/specs/2026-09-27-wesr-stock-design.md` §8（长按进配网）、§10（BLE 协议，已冻结）、§11（小程序三屏）、§12（对时）

## Global Constraints

- 屏幕 400×300、**1-bit**：只能纯黑/纯白（阈值 `<0x7fff` 算黑），不能用灰度/颜色表达状态。
- 单包**固定 ≤ 20 字节**（iOS 不支持 `wx.setBLEMTU`）；单条报文上限 **4KB**，超限丢弃本次会话。
- 报文 UTF-8 JSON，`\n` 结尾；命令/事件名与错误码严格照 spec §10，不许改名。
- WiFi 密码只允许小程序→板子单向；**不支持回读**，任何回包里不得出现 `pass` 字段。
- 配网模式：长按 KEY 3 秒进入/退出，3 分钟无操作自动退出（已有 `quote_service` 的超时逻辑）。
- 蓝牙只在配网模式开启；退出时必须停广播并释放。
- 纯逻辑层（`firmware/components/wesr/logic/*.c`）**不许 include 任何 ESP-IDF 头**，只能用 C 标准库 —— 否则宿主机测试编不过。
- 每改一处逻辑层就跑 `mingw32-make -C firmware/test run`；每改板端就烧板 + 回读显存自检（`CONFIG_WESR_DEBUG_FB_DUMP`）。
- 提交粒度：一个逻辑改动一个提交，message 用中文写清"问题 + 修法 + 代价"。

## 文件结构

| 文件 | 职责 |
| --- | --- |
| `firmware/components/wesr/logic/ble_proto.c` | **新建**。分片重组、极简 JSON 取值、报文构造、股票列表校验。纯 C，无 IDF 依赖。 |
| `firmware/components/wesr/include/wesr_logic.h` | 追加 `ble_proto.c` 的类型与函数声明。 |
| `firmware/test/test_logic.c` | 追加断言。 |
| `firmware/components/wesr/app/ble_task.cpp` | **新建**。NimBLE NUS 外设 + 命令分发（调 net/cfg/RTC/AppState）。 |
| `firmware/components/wesr/include/ble_task.h` | **新建**。`Ble_Enable(bool)` / `Ble_Start()`。 |
| `firmware/components/wesr/app/quote_service.cpp` | 配网模式开关处调 `Ble_Enable()`。 |
| `firmware/components/wesr/app/net_task.cpp` | 加一个"扫描返回数组"的函数（现有 `Net_WifiScanLog` 只打日志）。 |
| `firmware/tools/ble_probe.py` | **新建**。PC 端自检客户端（bleak），不是产品，是验收工具。 |
| `miniprogram/**` | **新建**。微信小程序三屏。 |

---

### Task 17: BLE 协议编解码层（纯逻辑 + 宿主机测试）

**Files:**
- Create: `firmware/components/wesr/logic/ble_proto.c`
- Modify: `firmware/components/wesr/include/wesr_logic.h`（在 `wesr_chart_render` 之后追加）
- Modify: `firmware/components/wesr/CMakeLists.txt`（`logic/ble_proto.c` 加进 SRCS）
- Test: `firmware/test/test_logic.c`

**Interfaces:**
- Produces（后面几个任务全靠这几个签名）:

```c
#define WESR_BLE_CHUNK   20      /* 单包字节数上限 */
#define WESR_BLE_MSG_MAX 4096    /* 单条报文上限 */

/* —— 接收：字节流 → 整条报文（去掉 '\n'）—— */
typedef struct { char buf[WESR_BLE_MSG_MAX + 1]; uint16_t len; bool drop; } wesr_ble_rx_t;
void   wesr_ble_rx_reset(wesr_ble_rx_t *rx);
bool   wesr_ble_rx_push(wesr_ble_rx_t *rx, const uint8_t *d, uint16_t n); /* 攒到一条完整报文返回 true */
uint16_t wesr_ble_rx_take(wesr_ble_rx_t *rx, char *out, uint16_t cap);    /* 取出一条，0 = 没有 */

/* —— 发送：整条报文 → 20 字节一片 —— */
uint16_t wesr_ble_chunk_len(uint16_t total, uint16_t sent);  /* 0 表示发完了 */

/* —— 极简 JSON 取值（报文是我们自己定的，不做通用解析）—— */
bool wesr_ble_cmd_is(const char *json, const char *cmd);                    /* "cmd":"<cmd>" ? */
bool wesr_ble_get_str(const char *json, const char *key, char *out, uint16_t cap);
bool wesr_ble_get_int(const char *json, const char *key, long *out);

/* —— setStocks 列表校验 —— */
int  wesr_ble_parse_items(const char *json, wesr_stock_cfg_t *out, uint8_t max,
                          const char **err);   /* 返回只数；出错返回 -1 并写 *err */
/* 代码格式校验直接用已有的 wesr_code_valid()（logic/defaults.c，Task 9 就有），不重复实现 */

/* —— 回包构造：返回字节数，放不下返回 -1 —— */
typedef struct { char ssid[33]; int8_t rssi; } wesr_ap_t;
typedef struct {
    const char *fw, *mac, *ssid, *ip, *wifi_state;   /* wifi_state: connected/connecting/idle */
    int   batt_pct, batt_mv, rssi;
    uint8_t cfg_count, page, group, idx;
} wesr_ble_status_t;
int wesr_ble_fmt_status(char *out, uint16_t cap, const wesr_ble_status_t *s);
int wesr_ble_fmt_ack (char *out, uint16_t cap, const char *cmd);
int wesr_ble_fmt_err (char *out, uint16_t cap, const char *code, const char *msg);
int wesr_ble_fmt_scan(char *out, uint16_t cap, const wesr_ap_t *aps, uint8_t n);
int wesr_ble_fmt_cfg (char *out, uint16_t cap, const wesr_app_cfg_t *cfg);
```

错误码（spec §10，写成字符串常量）：`E_CODE_FMT` / `E_TOO_MANY` / `E_SSID` / `E_PASS` / `E_NVS` / `E_WIFI` / `E_ARG`（参数缺失或非法，协议里没写但必要，小程序要能区分"我不知道这条命令"和"参数不对"）。

- [ ] **Step 1: 写失败测试**（追加到 `test_logic.c` 的 `test_nav` 之后）

```c
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

    /* 6) 股票代码校验（复用 Task 9 的 wesr_code_valid） */
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

    /* 9) 回包：能被 JSON 解析回去（这里只校验关键字段在不在、长度对不对） */
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
    cfg.count = 2; snprintf(cfg.stocks[0].code, sizeof cfg.stocks[0].code, "sh600519");
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
```

  并在 `main()` 里 `test_ble_proto();`（跟在 `test_nav();` 后面）。

- [ ] **Step 2: 跑测试确认失败**

Run: `mingw32-make -C firmware/test run`
Expected: 编译失败（`wesr_ble_rx_t` 未定义等）

- [ ] **Step 3: 实现** `logic/ble_proto.c`

要点（照写即可）：
1. `wesr_ble_rx_push`：把字节**含 `\n`** 追加进 `buf`（`\n` 是分隔符，删掉它 `take` 就不知道第一条到哪结束 —— 这里踩过一次，测试第 1 条卡住）。追加前判断 `len >= WESR_BLE_MSG_MAX` → 清空缓冲并 `drop = true`；`drop` 状态下继续吃字节直到遇到 `\n` 再复位（否则残片的尾巴会粘到下一条报文头上）。
2. `wesr_ble_rx_take`：从 `buf` 里找第一个 `\n`，把前面那段（不含 `\n`）拷到 `out`，剩下的前移；空行和"调用者缓冲装不下"都跳过继续找下一条。没找到返回 0。
3. `wesr_ble_get_str`：用 `snprintf(pat, "\"%s\":", key)` 拼出键，`strstr` 找**第一次出现**；跳过空白；必须是 `"` 开头；逐字节拷贝直到未转义的 `"`；`\\` + 下一字节原样收（解 `\"` `\\` `\/`）；遇到 `\uXXXX` 不做解码，按 UTF-8 原样收（我们只传中文和 ASCII）。
   **不能**用 `strstr(json, key)` 直接找 —— 必须带引号和冒号，否则 `name` 会命中 `nickname`（测试第 5 条卡这个）。
4. `wesr_ble_get_int`：同法找键，允许负号，用 `strtol`，末尾必须是 `,` `}` 或空白，否则算失败。
5. `wesr_ble_parse_items`：先找 `"items":[`，然后循环找 `{`…`}`（对象内不嵌套 `{}`，直接找下一个 `}`），每个对象里取 `code`/`name`/`mark`；`code` 不合法（用**已有的** `wesr_code_valid`）→ `E_CODE_FMT`；个数超过 `max` → `E_TOO_MANY`；`items` 缺失或 `name` 为空 → `E_ARG`；`mark` 缺省时调**已有的** `wesr_first_char(name, mark, sizeof mark)`。
6. 回包构造：一律 `snprintf` 到一个临时 `char tmp[512]`，最后 `if (len >= cap) return -1; memcpy(out, tmp, len + 1);`。`wesr_ble_fmt_status` 的 `ssid/ip` 为空时写 `""`。

- [ ] **Step 4: 跑测试确认通过**

Run: `mingw32-make -C firmware/test run`
Expected: `all logic tests passed`（含 `ble_proto ok`）

- [ ] **Step 5: 提交**

```bash
git add firmware/components/wesr/logic/ble_proto.c firmware/components/wesr/include/wesr_logic.h \
        firmware/components/wesr/CMakeLists.txt firmware/test/test_logic.c
git commit -m "feat(wesr): BLE 配网协议编解码层（分片重组 + 报文构造 + 股票校验，宿主机可测）"
```

---

### Task 18: NimBLE NUS 外设 + 配网模式生命周期

**Files:**
- Create: `firmware/components/wesr/app/ble_task.cpp`、`firmware/components/wesr/include/ble_task.h`
- Modify: `firmware/components/wesr/CMakeLists.txt`（加 `app/ble_task.cpp`，`PRIV_REQUIRES` 加 `bt`）
- Modify: `firmware/components/wesr/app/quote_service.cpp`（`Quote_NavSetPairing` 里开/关）
- Modify: `firmware/sdkconfig.defaults`（打开 BT + NimBLE 外设）

**Interfaces:**
- Consumes: Task 17 的 `wesr_ble_rx_*` / `wesr_ble_fmt_*`。
- Produces: `void Ble_Enable(bool on);`（幂等；`on=true` 起栈+广播，`false` 停广播）、`void Ble_Start(void);`（开机时读一次配网标志，通常什么都不做）。

- [ ] **Step 1: sdkconfig.defaults 打开 NimBLE**

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_NVS_PERSIST=n
```

- [ ] **Step 2: 实现 NUS 服务**

UUID 直接照 spec §10（`6E400001/2/3-B5A3-F393-E0A9-E50E24DCCA9E`）。要点：
1. `Ble_Enable(true)`：`nimble_port_init()` → `ble_svc_gap_init()` / `ble_svc_gatt_init()` → 注册 NUS 服务（RX = WRITE，TX = NOTIFY）→ 设设备名 `WESR-STOCK` → `nimble_port_freertos_init(host_task)` → 广播。
2. `Ble_Enable(false)`：停广播（`ble_gap_adv_stop`）→ 断开连接（`ble_gap_terminate`）→ `nimble_port_stop()`。**重复调用要安全**（用一个 `static bool s_on` 挡）。
3. RX 回调：把字节喂 `wesr_ble_rx_push`，攒够一条就取出来交给 `Ble_OnLine()`（Task 19 实现，先留 `ESP_LOGI`）。
4. 发送：`Ble_SendLine(const char *msg)` 按 `wesr_ble_chunk_len` 分片逐包 `ble_gatts_notify_custom`，每片之间 `vTaskDelay(1)`（20 字节连发会让手机侧丢包）。
5. 连接事件里把 `AppState.bt_connected` 置 true/false —— 顶栏那个蓝牙图标就是靠它（未连显示 ✕）。

- [ ] **Step 3: 接到配网模式**

`Quote_NavSetPairing(bool on)` 里加 `Ble_Enable(on);`，并同步 `AppState.pairing`。3 分钟超时那条路（`Quote_Run` 里 `pair_timeout`）也要调 `Ble_Enable(false)`。

- [ ] **Step 4: 真机验证（无手机也能验）**

```powershell
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd D:\github\WESR-STOCK\firmware
idf.py build
idf.py -p COM5 flash
```
长按 KEY 3 秒 → 串口出现 `ble: advertising as WESR-STOCK`，状态条反白；再长按 → `ble: stopped`。
（本机串口**只能读**，长按要人按；自动化验证放在 Task 20。）

- [ ] **Step 5: 提交**

```bash
git commit -am "feat(wesr): 配网模式起 NimBLE NUS 外设（NUS UUID 照 spec §10）"
```

---

### Task 19: 命令分发与执行

**Files:**
- Modify: `firmware/components/wesr/app/ble_task.cpp`
- Modify: `firmware/components/wesr/app/net_task.cpp` + `include/net_task.h`（加 `int Net_WifiScan(wesr_ap_t *out, uint8_t max)`）

**Interfaces:**
- Consumes: Task 17 的解析/构造，Task 18 的 `Ble_SendLine`。
- Produces: `void Ble_OnLine(const char *line);`

| cmd | 做什么 | 回什么 |
| --- | --- | --- |
| `hello` | 什么都不做 | `status` |
| `scan` | `Net_WifiScan()`（最多 12 个，按 RSSI 降序，跳过空 SSID） | `scanResult` |
| `setWifi` | 校验 `ssid` 空/>32 → `E_SSID`，`pass` 空/>64 → `E_PASS`；存 NVS；调 `Net_WifiConnect(ssid,pass,15000)` | 成功 `ack`，失败 `err E_WIFI`，存不下 `err E_NVS` |
| `getCfg` | 读当前 `wesr_app_cfg_t` | `cfg` |
| `setStocks` | `wesr_ble_parse_items` → `wesr_make_marks` → 存 NVS → 通知行情服务重载 | `ack` / `err` |
| `setInterval` | 只接受 5/15/30/60 | `ack` / `err E_ARG` |
| `timeSync` | `setenv("TZ","CST-8",1); tzset();` → `localtime_r` → `Rtc_SetTime` | `ack` |
| `exit` | `Quote_NavSetPairing(false)` | `ack` |
| 其它 | — | `err E_ARG`（带 `cmd` 回显，方便小程序报"设备不认识这条命令"） |

- [ ] **Step 1: 实现分发**（`Ble_OnLine`：先 `wesr_ble_cmd_is` 逐个比对，顺序按上表）
- [ ] **Step 2: setStocks 生效**：写 NVS 后调 `Quote_SetPage(当前页)` 强制重画（已有函数），并把 `AppState.stock_count` 更新 —— 否则第 1 页右栏还是旧名。
- [ ] **Step 3: 真机自测**：用 `firmware/tools/ble_probe.py`（Task 20）跑一遍 8 条命令。
- [ ] **Step 4: 提交**（中文 message，写清"改了什么 + 为什么 + 代价"）。

---

### Task 20: PC 端自检客户端（验收工具，不是产品）

**Files:**
- Create: `firmware/tools/ble_probe.py`

- [ ] **Step 1: 装 bleak**（本机已验证有 Intel 蓝牙适配器）

```powershell
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' -m pip install bleak
```

- [ ] **Step 2: 写脚本**：按 NUS UUID 找设备 → 连 → 订阅 TX → `hello`（收 status）→ `scan`（收 scanResult）→ `getCfg` → `setStocks`（2 只夹具）→ `setInterval 30` → `timeSync` → `exit`；每条命令打印收发的原文，超时 5 秒算失败，最后打印 `PASS/FAIL` 汇总。
- [ ] **Step 3: 真机跑通**（长按 KEY 进配网 → 跑脚本 → 看板子状态条和串口）。
- [ ] **Step 4: 提交**，并把用法写进 `firmware/README.md` 新增的 §11「BLE 配网」。

---

### Task 21: 微信小程序三屏

**Files:** `miniprogram/`（`app.json` / `app.js` / `app.wxss` / `pages/device|wifi|stocks/` / `utils/ble.js` / `utils/store.js`）

**Interfaces:**
- Consumes: Task 18/19 的协议。`utils/ble.js` 对外只暴露 `connect()` / `send(obj)`（返回 Promise，内部做 20 字节分包、按 `\n` 重组、5 秒超时）。

- [ ] **Step 1: `utils/ble.js`**：`openBluetoothAdapter` → `startBluetoothDevicesDiscovery({services:['6E400001-...']})` → `createBLEConnection` → `getBLEDeviceServices` → `notifyBLECharacteristicValueChange`(TX) → 收到 notify 累积到 `\n` 再 JSON.parse 分发。
- [ ] **Step 2: 设备屏**：扫描列表 → 连接 → 显示 status（固件/电量/WiFi/IP/RSSI），顶部固定提示"板子需先长按 KEY 3 秒进入配网模式"。
- [ ] **Step 3: 配网屏**：`scan` → 列表选 SSID → 输密码 → `setWifi` → 显示结果与板端 IP；密码错红字提示并留在本屏（不承诺保存密码明文，`wx.setStorageSync` 只存 SSID）。
- [ ] **Step 4: 股票屏**：8 只（增删排序、全名 + 首字，首字默认自动取名字第一个字、撞车才让改）→ `setStocks`；刷新间隔 5/15/30/60 → `setInterval`；"从设备读回" → `getCfg`。
- [ ] **Step 5: 用户用微信开发者工具打开 `miniprogram/` 验证**（需要 AppID，用测试号也行）。我没法代跑这一步，脚本和协议都冻结了，出错按 `err.code` 查表。
- [ ] **Step 6: 提交**

---

### Task 22: 双端联调与文档

- [ ] **Step 1:** 板端改完股票 → 第 1 页右栏 8 行立刻变；改 WiFi → 串口出现新 IP，状态条从"未联网"变正常。
- [ ] **Step 2:** `firmware/README.md` 加 §11「BLE 配网（M4）」：NUS UUID、命令表、`ble_probe.py` 用法、常见错误码。
- [ ] **Step 3:** `KNOWLEDGE.md` 加一节"S3 原生 USB-Serial-JTAG 只读不可写"（这次踩的坑）与 NimBLE 配置要点。
- [ ] **Step 4:** 提交 + 给用户的验收清单（哪几条已真机验证、哪几条要他用手机确认）。

---

## Self-Review

**Spec 覆盖**：§10 的 8 条命令（Task 19）、NUS UUID 与 20 字节分片/4KB 上限（Task 17）、长按进配网与 3 分钟超时（Task 18，复用已有逻辑）、§11 三屏（Task 21）、§12 `timeSync` 兜底对时（Task 19）。

**有意简化（ponytail，写在这里免得后人以为是漏了）**：
1. 设备名固定 `WESR-STOCK`，不带 MAC 后缀 —— 一台板子够用；要同时配两台再加后缀（3 行）。
2. 不做 `\uXXXX` 转义解码 —— 报文里的中文一律按 UTF-8 原样传（小程序侧 `JSON.stringify` 就是这么出的）。
3. 不做通用的 JSON 解析器（spec 也说了手写）—— 报文集是我们自己冻结的，字段固定。
4. 蓝牙只在配网模式开，不做"常连"——省电，且避免和 WiFi 抢天线（S3 单天线共存）。

**类型一致性**：`wesr_ble_status_t` / `wesr_ap_t` 在 Task 17 定义，Task 18/19/20 只用不改名；`Ble_SendLine`（Task 18）与 `Ble_OnLine`（Task 19）成对。
