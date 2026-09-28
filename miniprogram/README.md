# WESR-STOCK 配网小程序

用微信开发者工具打开**本目录**（`miniprogram/`），AppID 选「测试号」或直接用它自带的
`touristappid`（`project.config.json` 里可以改成你自己的）。

三屏：**设备**（扫描 → 连接 → 看固件/电量/IP/信号）、**配网**（让板子扫 2.4G →
选 SSID + 输密码 → 保存并连接）、**股票**（8 只的增删排序 + 刷新间隔，下发或读回）。

用法：先把开发板**长按 KEY 3 秒**进配网模式（屏幕底部状态条反白），再来扫。
3 分钟没操作板子会自动退出配网。

## 协议

BLE 走 Nordic UART Service，报文是 UTF-8 JSON + `\n`，双向固定 20 字节分片 ——
见 [设计文档 §10](../docs/superpowers/specs/2026-09-27-wesr-stock-design.md)。
`utils/ble.js` 是唯一的通信层（不依赖任何 npm 包）。

## 自检

编解码/分包/重组是纯函数，改完在这儿跑一下（不需要开发者工具）：

```bash
node miniprogram/tools/selftest.js
```

板子侧的同一套协议由 `firmware/tools/ble_probe.py`（PC 当假手机）端到端验收。
