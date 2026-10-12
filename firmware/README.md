# WESR-STOCK 板端固件

微雪 **ESP32-S3-RLCD-4.2**（400×300 1-bit 黑白全反射屏）上的看盘器：4 个页面、KEY 键切页换股、
直连腾讯公开接口拉行情、按设计稿绘制分时图。设计与规格见 `../docs/superpowers/specs/`，
实施过程与踩坑记录见 `../.superpowers/sdd/2026-09-27-wesr-stock-firmware/progress.md`。

## 1. 环境

| 项 | 要求 | 说明 |
| --- | --- | --- |
| ESP-IDF | ≥ 5.5.0 | 本机实测 **v6.1**；官方 BSP 是 5.x 时代写的，v6 的适配改动见 §6 |
| 工具链 | `idf.py`（含 cmake/ninja/xtensa 工具链） | 用 ESP-IDF 的 PowerShell profile 激活后再执行 |
| 字体生成 | node / npx（会临时拉 `lv_font_conv`） | 只在需要重建字体时用到 |
| 宿主机测试 | gcc + mingw32-make（Windows） | 纯逻辑层的 149 条断言，不需要硬件 |

## 2. 构建与烧写

```powershell
# 激活 ESP-IDF（路径按本机实际安装位置）
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd firmware

idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor      # COM 口按设备管理器里的实际值
```

宿主机逻辑测试（改 `components/wesr/logic/` 下任何文件后都要跑）：

```powershell
cd firmware
mingw32-make -C test clean && mingw32-make -C test run
```

## 3. WiFi 配置（v1 用 menuconfig）

```powershell
idf.py menuconfig      # WESR-STOCK → WiFi SSID (2.4G) / WiFi password
```

- **只能连 2.4G**（ESP32-S3 不支持 5G）。
- 凭据写进 `firmware/sdkconfig`，该文件**已加入 .gitignore**，不会进仓库。
- 命令行构建时可以用一层本地覆盖，免得每次都要开 menuconfig
  （`firmware/sdkconfig.wifi` 也在 .gitignore 里）：

  ```powershell
  $env:SDKCONFIG_DEFAULTS = "$PWD\sdkconfig.defaults;$PWD\sdkconfig.wifi"
  idf.py build
  ```
- 留空则开机直接显示反白状态条「未配网」，并按 M4 的计划等小程序 BLE 配网。
- 没连上时固件会**每 20 秒重试一次**，每次重试前扫描一遍附近 2.4G 网络并打到串口
  （`net: scan: N ap(s)`），方便对照"是密码错还是信号/频段问题"。

## 4. 字体（构建产物，不入 git）

`components/wesr/ui/font_*.c` 由脚本生成，**不在版本控制里**（字体许可 + 体积）：

```powershell
powershell -File tools/gen_fonts.ps1
```

- 默认用系统里的 `C:\Windows\Fonts\NotoSansSC-VF.ttf`（OFL 许可、单体 TTF）；
  换字体：`-FontPath C:\Windows\Fonts\simhei.ttf`。
  **不能用 `.ttc` 字体集合**（lv_font_conv 会报 `Unsupported OpenType signature ttcf`）。
- 产出：`font_cn16.c` / `font_cn12.c`（全 CJK 区段 + ASCII），
  以及 **6 档数字字体** `font_num{96,25,16,14,12,9}.c`。
  这份清单必须与 `components/wesr/CMakeLists.txt` 的 `SRCS` 一路对齐 ——
  曾经 `font_num16`（第 2 页四宫格价格）只在 CMakeLists 里、脚本不生成，
  "全新克隆 + 跑脚本 + build" 会直接缺文件编不过。
  改字号时**两处一起改**，并跑一次 `idf.py build` 验证。
- 每档都带 `▲ ▼ — · ° … → ！？：；、。“”` 等标点 —— 缺哪个字形就会在屏上显示成方框。
- 中文脚本必须存成 **UTF-8 with BOM**，否则 Windows PowerShell 5.1 按 GBK 解析会报语法错。
- 目录里如果出现不在清单内的 `font_num*.c`（历史上留下过 104/78/19/11 四档），
  它们既不在 SRCS 也无任何引用，可以安全删掉（约 418KB）。

## 5. KEY 键

| 动作 | 行为 |
| --- | --- |
| 单击 | 切页 1→2→3→4→1 |
| 双击 | 第 2 页切组（1–4 / 5–8）；第 3 页换股（`1/8`→`2/8`…）；第 1、4 页无动作 |
| 长按 3 秒 | 进/出配网模式（状态条反白「配网模式 · 等待小程序」，同时开 BLE 广播；3 分钟无操作自动退出） |

去抖与单击/双击/长按判定交给官方 `port_bsp/button_bsp.c`（multi_button），
固件只做"事件 → 语义"的映射（`app/input_task.cpp`）。

## 6. IDF v6 适配记录（换回 5.5 需要回退这几处）

- `json` 组件在 v6 已移出 IDF：本项目解析是手写的，直接不依赖它。
- 头文件由 `esp_driver_*` 组件提供，必须显式声明：`port_bsp` 补了
  `esp_driver_i2c/spi/gpio`、`esp_lcd`、`sdmmc`；`SensorLib` 补了 `esp_driver_i2c/spi`。
- `esp_lcd_panel_io_spi_config_t` 的 `dc_gpio_num/cs_gpio_num`、`spi_bus_config_t` 的
  `*_io_num` 在 v6 是 `gpio_num_t` 枚举，C++ 不再隐式转换 → `display_bsp.cpp` 补显式 cast。
- `port_bsp` 的公有头 include 了这些驱动头，所以依赖必须是 **REQUIRES（公有）**，
  放 `PRIV_REQUIRES` 会让上层组件编不过。

## 7. 无屏验证（开发期自检）

### 7.1 抓串口 / 解码画面

本板是 S3 原生 USB-Serial-JTAG，**串口只能读不能写**（往板子写字节会 `Semaphore timeout`），
所以下面这些工具都是只读的：

```powershell
python tools\serial_cap.py --port COM5 --secs 90 --out build\log.txt   # 抓一段时间
powershell -File tools\idf.ps1 -p COM5 -b 460800 flash                 # 复用 IDF 环境
```

把显存 dump 成画面（需要 `CONFIG_WESR_DEBUG_FB_DUMP=y`）：

```powershell
python tools\fb_to_png.py build\log.txt build\fb.png    # 依赖 Pillow
```

`Board_DumpFb()` 每次触发都会打 120×200 的半分辨率 ASCII（不需要任何 flag），
`Board_DumpFbFull()` 打 400×300 全分辨率（约 120KB，串口约 10 秒，**量产固件请关掉**）。

**面板字节约定（改 dump/转换代码前必读）**：1bpp 面板缓冲里 **置位 = 白**；
一个目标字节覆盖 4 个连续源行，bit0/1 是最上面那行、bit6/7 是最下面那行，
bit 对内左列是高位。完整推导与回归见 `test/test_blit.c`。

### 7.2 性能打点

`Board_FlushStats()` 每 30 秒（或 100 帧）打一行：

```
[perf] flush n=5 mean=5295/4890/404 us (total/conv/lcd) max=… skip=2(16882 us)
       heap=63/63KB largest=31744B psram=7330/7330KB lvgl_mean=4656 us max=99532
```

- `total/conv/lcd`：整帧 flush / RGB565→1bpp 转换 / SPI 排队
- `skip`：帧差短路掉的帧数与被省下的时间
- `lvgl_mean/max`：`lv_timer_handler()` 单轮耗时（含 flush，flush 在它里面同步跑完）

**实测（第 1 页常驻、未配网状态）**：

| 项 | 优化前 | 优化后 |
| --- | --- | --- |
| 单帧 RGB565→1bpp 转换 | **140.9 ms** | 4.9 ms（无内容变化时 0，被帧差短路） |
| 单帧 flush 合计 | **141.5 ms** | 5.3 ms |
| `lv_timer_handler` 峰值 | **209.8 ms** | 99.5 ms |
| 开机 bootstrap（1 快照 + 4 分时） | ~30 s，**分时全部失败** | ~1.2 s，全部成功 |

关键结论：**瓶颈从头到尾都不是 SPI**（实测 `lcd` 只有 0.4~0.7 ms/帧），
而是"逐像素查表 + 读改写"那 12 万次 PSRAM 随机访问。详见 §10。

## 8. 目录

```
firmware/
├── main/main.cpp                 启动：板级初始化 → UI → 传感器/时钟/按键/网络任务
├── components/wesr/
│   ├── include/                  对外头文件（wesr_logic.h 是纯 C 逻辑层的总入口）
│   ├── logic/                    纯 C 逻辑：解析/格式化/坐标/导航/调度/光栅化（宿主机可测）
│   ├── app/                      板端胶水：board_init / cfg_store / net / quote_service /
│   │                             sensor_task / input_task（含 C++：官方驱动是 C++ 类）
│   └── ui/                       四页 UI + 1-bit 分时控件 + 生成的字体
├── test/                         宿主机测试：test_logic.c（纯逻辑）+ test_blit.c（面板位序回归）
└── tools/                        字体生成、IDF 环境包装、串口抓取、画面解码、位序搜索
```

## 9. 已知限制（v1）

- **无 OTA、SD 卡未使用**（在 spec 的"范围外"里）。BLE 配网见 §11。
- **配网模式下不拉行情**：BLE 常驻要 ~50KB 内部 RAM，TLS 握手就起不来了（串口会打
  `http open failed (28674)`）。退出配网会 `nimble_port_deinit()` 把内存还回去，约 10 秒内自动恢复。
- 充电状态是"电压在涨"的**启发式推断**（板子的 CHG 只是 LED，没接 GPIO），插着 USB 但电压平稳时会显示"未充电"。
- 串口日志里的中文可能显示成 `?`：IDF v6 控制台默认非 UTF-8（`chcp 65001` 或系统开 UTF-8 可解）。
- 第 2、3 页的分时接口：优先走**明文 HTTP** 的 `proxy.finance.qq.com`
  （实测 200，和 HTTPS 同一份 JSON），失败再回退到 HTTPS 的 `web.ifzq.gtimg.cn`。
  注意 `web.ifzq.gtimg.cn` 的**明文**口会被腾讯网关拦成 302（`stgw`），所以两者不能简单对调。
- 标题栏的 WiFi/蓝牙/电池图标用的是 LVGL 内置符号字体，与设计稿里手绘的图标形状略有差异。

## 10. 性能：这次改了什么（2026-10）

三条主因，按收益排序：

1. **刷屏链路**（`port_bsp/display_bsp.cpp` + `wesr/port/board_init.cpp`）
   原来 flush 对每个像素做 `PixelIndexLUT[x][y]` + `PixelBitLUT[x][y]` + `DispBuffer[idx]` 读改写，
   两张表共 360KB 放 PSRAM、而索引沿一行是跳着走的（每次 +2 像素跳 75 字节），
   等于每帧 12 万次必然 cache miss 的 PSRAM 随机访问 —— 实测 **140.9ms/帧**。
   现在改成"每 4 个连续源行产出 1 个整字节"的顺序转换（`RLCD_BlitLvglFrame`），
   **两表整块删除（省 360KB PSRAM）**，并加了**帧差短路**（整帧哈希相同就不发 SPI）：
   实测 **4.9ms/帧，无变化时 0**。位序不许猜 —— `test/test_blit.c` 会拿逐像素老路径逐字节对拍。
2. **编译/时钟**：`CONFIG_COMPILER_OPTIMIZATION_DEBUG`（`-Og`，不内联）→ `_PERF`，
   CPU 160 → 240 MHz。已写进 `sdkconfig.defaults`，免得 menuconfig 又改回去。
3. **网络**：第 1 页快照与第 2/3 页分时都改走明文 HTTP（原来每轮 5 次全新 TCP+TLS 握手）。
   另外两个原本的坑：
   - `timeout_ms` 只是 **socket 级**超时，不是整体超时；
   - `skip_cert_common_name_check` 在 mbedTLS 4 下**必须同时给一个校验方式**，
     否则降级路径必然 `SSL_SETUP_FAILED`（原来每只股票白付一次 5 秒超时）。

   ⚠️ **踩坑记录：不要做跨请求的连接复用。** 曾经按 host 缓存 `esp_http_client` 句柄复用连接，
   结果是**第二次之后拿到的一直是第一次的响应** —— 四只股票全解析出 `17.42`、响应长度也都等于
   第一只的 10391 字节（真实值分别是 17.42/35.37/54.90/27.21，长度 10391/10708/10385/10380）。
   关掉复用立刻恢复。现象与"本次一个字节都没读到、缓冲区里留着上次内容"一致，
   在没查清 IDF 侧状态复位之前**不要再打开**。明文 HTTP 本来也没有握手可省。
   配套加了一条常驻校验：`fetch_minute` 会检查响应里是否出现本次请求的股票代码，
   不匹配就丢弃并报 `响应里没有 <code>（数据串号？）` —— 这类串号靠看曲线很难发现。

还做了：
- **UI 空刷帧**：`ui_util.h` 的 `Ui_SetTextIfChanged()` / `Ui_SetFontIfChanged()`
  （LVGL 的 `lv_label_set_text` 比较的是**指针**不是内容，`lv_obj_set_style_*` 更是无条件
  `refresh_style`，而 `full_refresh=1` 下任何一次 invalidate 都是整屏重绘）。
- **HTTP 响应缓冲**：实测服务端都是 chunked 无 Content-Length，`fetch_headers` 恒返回 -1，
  所以老的 `cap` 逻辑**每个请求固定 malloc 32KB**、实际只用 4~11KB。改成一块 16KB 常驻缓冲。
- **读循环完整性**：`r <= 0` 并不等于"读完了"（socket 超时且已读到数据时 IDF 会返回已读字节数），
  改用 `esp_http_client_is_complete_data_received()` 判定 + 一个有上限的续收窗口，
  不再把"半截响应 + HTTP 200"当成功。这也是先前"`quotes: 4/4 ok` 但只读了 2049 字节"的根因。
- **调度**：休市时"按页补一次"不再推后周期刷新（否则补拉后下一次要再等一个 interval，最长 75 秒）；
  分时的轮询间隔加 30s 下限（服务端是分钟级的，15s 去拉等于 4 次里 3 次白拉）；
  第 2 页改成**每只到手就刷**，不用等 4 只串行全完（原来画面要空白 2~4 秒）。
- **BLE**：连上后主动要 30~50ms 的连接间隔 + 4 个周期 slave latency
  （配网是小包低频，手机侧默认常要 7.5~15ms，会把配网窗口的平均电流抬很高）。
- **分页状态条**：切页/换股后能一眼看出"这页显示的是什么"。状态条优先级为
  `配网模式 > 未配网 > 未联网 > 本页无数据 > 休市(带日期) > 空`，文案：

  | 情况 | 文案 |
  | --- | --- |
  | 本页该有的数据还没到手（含分时缓存归属不对） | `第N页 · 数据拉取中`（反白） |
  | 休市，或手上这份是上一交易日的 | `第N页 · MM-DD 收盘` |
  | 未联网 / 未配网 / 配网模式 | 原有三句 |

  两个细节：页号取 `Ui_CurrentPage()` 而不是 `AppState_Status()->page`
  （后者只在按键路径 `publish_nav()` 时更新，绕过按键直接进页会显示错页号）；
  状态条高度 14 → **16px**，因为 `font_cn12` 的 `line_height` 是 15，14px 会把字顶端裁掉一行。
  字数上限约 15 个汉字（12px 等宽 × 400px），加字前先量。

**修正**：先前文档里"分时接口必须走 HTTPS"的结论不对 —— 是 `web.ifzq` 的明文口被网关拦成 302，
不是"HTTP 不行"；`proxy.finance` 的明文口一直是好的。

**SPI 已实测后关闭**：手册允许到约 33MHz，但上板对比 10MHz（684us/帧）与 30MHz（592us/帧）
只差 92us、约占整帧 1%，收益远小于风险，所以**保持 10MHz**。
`tools/spi_bench.py` 可以复现这个对比（把各档位日志喂给它）。

**还没做**（按收益排序，都没验证）：
- 中文字体覆盖 20902 个汉字、实际用不到 100 个：`font_cn16.c` 6.1MB + `font_cn12.c` 4.7MB
  占字体总量 94%，且 cmap 碎成 85 段（LVGL 8.4 的字形缓存只有"上一字符"1 项）。
  用 `gen_fonts.ps1` 精确列举用字即可大幅缩减，但要改生成脚本并重跑全量字体。
- `ui_trend.c` 已加内容指纹短路（内容没变不重画），但 `wesr_chart_render` 本身仍是全量光栅化。
- `parse_quote.c` 同一行被 9 个字段各扫一遍 `~`；`parse_minute.c` 逐点 `sscanf`。

## 11. BLE 配网（M4）

长按 KEY 3 秒进配网模式 → 板子广播 Nordic UART Service（名字 `WESR-STOCK`）→
手机小程序改 WiFi、改股票池、对时。协议（UUID、20 字节分片、8 条命令、错误码）
在 `../docs/superpowers/specs/2026-09-27-wesr-stock-design.md` §10。

| 位置 | 说明 |
| --- | --- |
| `components/wesr/logic/ble_proto.c` | 协议编解码（纯 C，宿主机 `test/` 里测） |
| `components/wesr/app/ble_task.cpp` | NimBLE NUS 外设 + 命令执行（独立任务） |
| `tools/ble_probe.py` | **PC 当假手机**的自检客户端（`pip install bleak`） |
| `../miniprogram/` | 微信小程序三屏 |

没有手机也能验收（板子先进配网模式）：

```powershell
python tools\ble_probe.py                 # 跑全部 8 条命令
python tools\ble_probe.py --only hello    # 只跑一条
python tools\ble_probe.py --addr 28:84:85:58:FA:CE   # 扫描偶尔抽风时直连
python tools\ble_probe.py --cmd '{"cmd":"setInterval","sec":15}'   # 临时改设置/恢复
```

**内存是这块板子的硬约束**（BLE 50KB vs TLS/整屏刷新 DMA 各要 15~40KB 连续内部内存），
踩过的坑与实测数字写在 `../docs/superpowers/plans/2026-09-28-wesr-stock-ble-provisioning.md`
的「实机约束」一节 —— 动蓝牙相关代码前先读那一节。

## 12. 参考资料

- 板卡知识库：`../KNOWLEDGE.md`（引脚、I2C 地址、屏特性、环境版本）
- 官方资料归档：`../references/`（`official/` 官方示例包、`datasheets/` 原理图与手册、`upstream/` 文档离线副本）
- 腾讯接口字段与实测记录：`../docs/superpowers/specs/2026-09-27-wesr-stock-design.md` §4
