#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 连 WiFi（STA，2.4G），成功返回 true；会顺带把 IP/RSSI 写进 AppState */
bool Net_WifiConnect(const char *ssid, const char *pass, int timeout_ms);
/* SNTP 校时，成功后把时间写进板载 RTC（PCF85063） */
void Net_TimeSync(int timeout_ms);
/* 扫描附近 2.4G 网络并把结果打到串口（配网时看该填哪个 SSID） */
void Net_WifiScanLog(void);
/* 起网络任务：加载配置 → 连 WiFi → 校时 → 交给行情服务按页拉取 */
void Net_TaskStart(void);

#ifdef __cplusplus
}
#endif
