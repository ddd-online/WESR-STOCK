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
- 产出：`font_cn16.c`（全 CJK 区段 + ASCII，约 672KB）与 `font_cn12.c`（小字用），
  以及 8 档数字字体 `font_num{78,25,19,16,14,12,11,9}.c`。
- 每档都带 `▲ ▼ — · ° … → ！？：；、。“”` 等标点 —— 缺哪个字形就会在屏上显示成方框。
- 中文脚本必须存成 **UTF-8 with BOM**，否则 Windows PowerShell 5.1 按 GBK 解析会报语法错。

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

没有相机、也不方便一直盯着屏幕时，可以开：

```powershell
# menuconfig → WESR-STOCK → "开发期：每 15 秒把显存 dump 成 ASCII 到串口"
# 或者直接往 sdkconfig 追加：CONFIG_WESR_DEBUG_FB_DUMP=y
```

固件会每 15 秒把 1-bit 显存按 `#`/`.` 打成 ASCII（400×300，约 120KB）从串口输出，
主机侧读串口、把 `FB-BEGIN-FULL` … `FB-END` 之间的行渲染成 PNG 就能逐像素核对画面。
**量产固件请关掉**（默认关）：它每 15 秒占用串口约 10 秒。

这条通道**只能读不能写**：本机（S3 原生 USB-Serial-JTAG，COM5）往板子写字节会
`Semaphore timeout`，.NET `SerialPort.Write` 和 pyserial 都一样 —— 所以别指望从串口
"按" KEY，要验证某个页面状态就让固件开机直接进那个状态（`s_nav.page/group/idx` +
`publish_nav()` + `Ui_ShowPage()`），烧一次看一屏，量完再撤掉。

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
├── test/                         宿主机 assert 测试 + 真实响应 fixture
└── tools/gen_fonts.ps1           字体生成
```

## 9. 已知限制（v1）

- **无 OTA、SD 卡未使用**（在 spec 的"范围外"里）。BLE 配网见 §11。
- **配网模式下不拉行情**：BLE 常驻要 ~50KB 内部 RAM，TLS 握手就起不来了（串口会打
  `http open failed (28674)`）。退出配网会 `nimble_port_deinit()` 把内存还回去，约 10 秒内自动恢复。
- 充电状态是"电压在涨"的**启发式推断**（板子的 CHG 只是 LED，没接 GPIO），插着 USB 但电压平稳时会显示"未充电"。
- 串口日志里的中文可能显示成 `?`：IDF v6 控制台默认非 UTF-8（`chcp 65001` 或系统开 UTF-8 可解）。
- 第 2、3 页的分时接口在部分网络里会被拦：固件已内置双主机回退
  （`web.ifzq.gtimg.cn` → `proxy.finance.qq.com/ifzqgtimg/...`，同一份数据）。
- 标题栏的 WiFi/蓝牙/电池图标用的是 LVGL 内置符号字体，与设计稿里手绘的图标形状略有差异。

## 10. BLE 配网（M4）

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

## 11. 参考资料

- 板卡知识库：`../KNOWLEDGE.md`（引脚、I2C 地址、屏特性、环境版本）
- 官方资料归档：`../references/`（`official/` 官方示例包、`datasheets/` 原理图与手册、`upstream/` 文档离线副本）
- 腾讯接口字段与实测记录：`../docs/superpowers/specs/2026-09-27-wesr-stock-design.md` §4
