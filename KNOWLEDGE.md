# ESP32-S3-RLCD-4.2 知识库

微雪 4.2 英寸全反射屏（RLCD）ESP32-S3 AIoT 开发板。本文是从官方文档 + 示例源码里
提炼的可直接下手的结论；所有原始参考料都在 `references/` 下。

## 1. 硬件规格

| 项目 | 参数 |
| --- | --- |
| 模组 | ESP32-S3-WROOM-1-N16R8（Xtensa LX7 双核 240 MHz，16 MB Flash + 8 MB PSRAM） |
| 无线 | 2.4 GHz Wi-Fi + BLE 5，板载天线 |
| 屏幕 | 4.2" 全反射 RLCD，300×400，控制器 ST7305，**无背光**（靠环境光成像） |
| 音频 | ES8311（DAC 播放）+ ES7210（双麦 ADC，回声消除），MX1.25 2PIN 喇叭座 |
| 传感器 | SHTC3 温湿度 |
| 时钟 | PCF85063 RTC，独立备用电池座（**必须用可充电 ML1220，CR1220 会炸**） |
| 存储 | Micro SD 卡槽（FAT32，1-bit SDMMC） |
| 供电 | Type-C + 18650 电池座 + 充放电管理；CHG/WRN 指示灯 |
| 按键 | PWR（供电开关）、BOOT（GPIO0）、KEY（GPIO18） |
| 扩展 | 2×8 排母，2.54 mm |

功耗/续航（官方出厂程序实测）：待机约 24 h，5.32 V / 0.09 A。

## 2. 引脚表（唯一真源）

来源：`references/official/03_XiaoZhi/XiaoZhiCode_V2.1.0/main/boards/waveshare-s3-rlcd-4.2/config.h`，
与 `references/official/02_ESP-IDF/10_FactoryProgram/main/user_config.h` 一致。

| 功能 | GPIO |
| --- | --- |
| RLCD SPI | SCK 11 / MOSI 12 / CS 40 / DC 5 / RST 41 / TE 6 |
| I2C（RTC、SHTC3、Codec） | SCL 14 / SDA 13（`I2C_NUM_0`） |
| I2S 音频 | MCLK 16 / BCLK 9 / WS 45 / DIN 10 / DOUT 8；功放使能 PA 46 |
| SD 卡（1-bit SDMMC） | CLK 38 / CMD 21 / D0 39 |
| 按键 | BOOT 0、KEY 18（均低电平有效） |
| 电池电压 | ADC1_CH3（12 bit，ATTEN_DB_12，采样后 ×3 还原） |

LCD 走 `SPI3_HOST`，SPI 时钟与其他外设不冲突；RLCD 的 TE 引脚可用于撕裂同步（示例未用）。

## 3. 屏幕要点（最容易踩坑的地方）

- 单色（黑/白）黑白屏，**不是灰阶、也没有背光**；"看起来发暗"是正常物理特性，光越强越清晰。
- 分辨率：竖屏 300(W)×400(H)，横屏 400×300。官方例程按 400×300 横屏做 UI。
- 刷新：ST7305 是 SPI 屏，示例里用查表法（`AlgorithmOptimization = 3`）做像素→字节映射优化 CPU；U8G2 例程实测能跑到 74 FPS。
- 颜色只有 `ColorBlack = 0` / `ColorWhite = 0xff`，LVGL 用单色/`alpha 1bpp` 图像资源（如 `_ein_alpha_400x300`）。
- 屏幕是易碎件：装电池、插线时不要以屏幕为受力点，摔/压导致的破裂不在保修内。

## 4. I2C 设备地址

| 器件 | 地址 | 说明 |
| --- | --- | --- |
| PCF85063 | `0x51` | RTC，示例会先写一次时间再每秒读 |
| SHTC3 | `0x70` | 温湿度，需 CRC 校验，测量前先唤醒 |
| ES8311 | `0x18` | 播放 Codec |
| ES7210 | `0x40` | 录音 ADC |

## 5. 开发环境

| 方式 | 要求 |
| --- | --- |
| Arduino IDE | arduino-esp32 **≥ 3.3.0**；LVGL 用 **8.3.11 或 9.3.0**（版本强绑定驱动，混用会编译失败/跑飞）；SensorLib 0.3.1；U8G2 master |
| ESP-IDF | **≥ 5.5.0**，推荐 VS Code + ESP-IDF 扩展；装完自动识别环境 |

Arduino 板级设置（Tools 菜单）：芯片选 ESP32-S3，Flash/PSRAM 按 N16R8 配（16 MB Flash + OPI PSRAM）。
首次编译要下载依赖 + 建架构缓存，很慢是正常的。

## 6. 示例索引

Arduino（`references/official/01_Arduino/examples`）与 ESP-IDF（`references/official/02_ESP-IDF`）例程基本一一对应，
ESP-IDF 多一个 `10_FactoryProgram`（出厂综合示例，LVGL V8）。

| 例程 | 作用 | 关键 API |
| --- | --- | --- |
| 01/02 WIFI_AP / STA | 热点 / 连路由器，打印 IP | Arduino `WiFi.softAP()`；IDF `esp_wifi_bsp.c` |
| 03_ADC_Test | 读电池电压/百分比 | `Adc_GetBatteryVoltage()` / `Adc_GetBatteryLevel()` |
| 04_I2C_PCF85063 | RTC 读写 | `Rtc_Setup(&I2cbus, 0x51)` / `Rtc_SetTime()` |
| 05_I2C_SHTC3 | 温湿度 | `Shtc3Port::Shtc3_ReadTempHumi()`（依赖 SensorLib） |
| 06_SD_Card | SD 挂载 + 读写 | `CustomSDPort("/sdcard")` |
| 07_Audio_Test | 录音回放 / 播放音乐 | `CodecPort_SetInfo("es8311 & es7210",1,16000,2,16)`、`SetSpeakerVol(100)`、`SetMicGain(35)` |
| 08/09 LVGL_V8/V9_Test | LVGL 8.3.11 / 9.3.0 显示图片 | `lv_img_set_src` / `lv_image_set_src` |
| 10/11 U8G2_Test | U8G2 移植，快速刷新 | `u8g2_drawXxx` + `u8g2_sendBuffer()` |

按键交互（07_Audio_Test）：双击 BOOT 录音（3 s 自动停），单击 BOOT 回放，
双击 KEY 放音乐，单击 KEY 打断。

## 7. 小智 AI

`references/official/03_XiaoZhi/XiaoZhiCode_V2.1.0` 就是小智 AI 完整工程，板型选 `waveshare-s3-rlcd-4.2`。
不想编译的话，`references/official/04_Firmware/02_XiaoZhi_V2.1.0.bin` 可直接烧录。

烧录后连设备热点 `Xiaozhi-xxxxxx` → 浏览器开 `http://192.168.4.1` 配 2.4G Wi-Fi
（iPhone 热点要开"最大兼容性"）→ 设备播报 6 位验证码 → 到 xiaozhi.me 控制台绑定 → 唤醒词"你好，小智"。

注意：小智固件与出厂/自写程序是两份独立固件，**不能共存或一键切换**，要重新烧录；
小智不能离线使用，也读不了 SD 卡文件。官方最新固件版本是 v2.4.2（包里带的是 v2.1.0）。

## 8. FAQ / 避坑

- **电池供电开不了机**：装电池后第一次必须同时插 Type-C 激活，之后可拔线。
- **开机/关机**：短按 PWR 开机，长按 PWR 关机。
- **烧录失败/串口连不上**：按住 BOOT 再上电进下载模式，能解决大部分问题。
- **RTC 备用电池**：只能可充电（ML1220 等 3 V/3.3 V），用 CR1220 会损坏电池或电路。
- **SD 卡**：FAT32；上电前插好。
- **Arduino 编译报错**：先核对 Tools 里的芯片型号、Flash 模式、PSRAM 设置。
- **VSCode/ESP-IDF 装不上**：多为网络问题，换网络重试；安装路径不要有中文和空格。
- 首次编译慢是正常的，增量编译会快很多。

## 9. 目录结构

```
KNOWLEDGE.md        知识提炼（本文件）
references/
├── DOWNLOAD.md     参考资料清单与来源出处
├── upstream/       官方文档 7 个页面的离线纯文本副本
├── datasheets/     原理图 / 芯片手册（ST7305、ES8311、PCF85063、SHTC3）
├── official/       官方示例包（Arduino/ESP-IDF/小智/固件），体积大，未入 git
└── _archives/      官方 Demo.zip 原件，未入 git
```

上游来源：

- 文档首页 https://docs.waveshare.net/ESP32-S3-RLCD-4.2
- 示例代码包 https://files.waveshare.net/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2-Demo.zip
- GitHub 镜像 https://github.com/waveshareteam/ESP32-S3-RLCD-4.2
