# 需要手动下载的文件（自动下载被 WAF 拦截）

微雪的 `www.waveshare.net/w/upload/*` 走了阿里云 WAF 滑块验证：脚本/curl 请求只能拿到
12 KB 的验证页（不是 PDF）。**用浏览器打开下面的链接另存为即可**，文件名按“保存为”一列命名。

保存目录：`references/datasheets/`

| 保存为 | 链接 |
| --- | --- |
| `ESP32-S3-RLCD-4.2-schematic.pdf` | https://www.waveshare.net/w/upload/e/e6/ESP32-S3-RLCD-4.2-schematic.pdf |
| `ESP32-S3-RLCD-4.2-3dFile.rar` | https://www.waveshare.net/w/upload/f/f2/ESP32-S3-RLCD-4.2-3dFile.rar |
| `ST7305_datasheet.pdf` | https://www.waveshare.net/w/upload/5/5d/ST_7305_V0_2.pdf |
| `ES8311_datasheet.pdf` | https://www.waveshare.net/w/upload/6/65/ES8311.DS.pdf |
| `PCF85063_datasheet.pdf` | https://www.waveshare.net/w/upload/c/c0/Pcf85063atl1118-NdPQpTGE-loeW7GbZ7.pdf |
| `SHTC3_datasheet.pdf` | https://www.waveshare.net/w/upload/3/33/SHTC3_Datasheet.pdf |

可选（乐鑫站点，重要信息已在 `README.md` 里摘过，不下载也能开工）：

| 保存为 | 链接 |
| --- | --- |
| `ESP32-S3_datasheet_cn.pdf` | https://documentation.espressif.com/esp32-s3_datasheet_cn.pdf |
| `ESP32-S3_TRM_cn.pdf` | https://documentation.espressif.com/esp32-s3_technical_reference_manual_cn.pdf |

## 已经下载好的（不用管）

`references/official/` 已经是完整官方示例包（177 MB 的 `ESP32-S3-RLCD-4.2-Demo.zip` 解压结果）：

| 目录 | 内容 |
| --- | --- |
| `01_Arduino/examples` | 10 个 Arduino 例程（WIFI/ADC/RTC/SHTC3/SD/Audio/LVGL V8+V9/U8G2） |
| `01_Arduino/libraries` | LVGL8 / LVGL9 / SensorLib 离线库（353 MB） |
| `02_ESP-IDF` | 11 个 ESP-IDF 工程（含 `10_FactoryProgram` 出厂程序） |
| `03_XiaoZhi/XiaoZhiCode_V2.1.0` | 小智 AI 完整工程（含 `boards/waveshare-s3-rlcd-4.2`） |
| `04_Firmware` | 出厂固件 + 小智固件 bin，可恢复原厂状态 |

原始 zip 保留在 `references/_archives/ESP32-S3-RLCD-4.2-Demo.zip`。

> 这些大体积文件已写进 `.gitignore`，只留在本地。要在别的机器上重建，直接按
> `official/` 的来源重新下载：https://files.waveshare.net/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2-Demo.zip
> （这个域名没有 WAF，脚本可以直接拉）
