#pragma once
/* 全局状态：网络/电源/行情/系统，多个任务读写，用互斥锁保护 */
#include <stdbool.h>
#include <stdint.h>
#include "wesr_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 网络 */
    bool     wifi_connected;
    char     ssid[33];
    char     ip[25];
    int8_t   rssi;
    bool     bt_connected;
    char     mac[18];         /* 蓝牙地址 AA:BB:..：配网时给小程序显示，用来认哪台板子 */
    /* 电源与环境 */
    float    battery_v;
    uint8_t  battery_pct;
    bool     charging;
    float    temp_c, humi_pct;
    bool     sd_mounted;
    /* 行情 */
    bool     offline;
    bool     closed;
    uint32_t data_day;        /* 休市时显示的那天 yyyymmdd */
    uint32_t last_ok_ms;
    uint32_t today_fail;
    uint16_t refresh_sec;
    uint8_t  stock_count, page, group, idx;
    /* 系统 */
    char     fw[16];
    uint32_t uptime_s;
    uint32_t heap_kb, psram_kb;
    /* 行情缓存（quote_service 写，UI 读） */
    wesr_quote_t  quotes[WESR_MAX_STOCKS];
    uint8_t       quotes_n;
    wesr_minute_t minutes[4];      /* 当前组的四只 */
    uint8_t       minutes_key;     /* minutes[0] 是哪只股票（第 2 页=组起始，第 3 页=idx）*/
    bool          minutes_ok;
    bool          pairing;         /* 长按 KEY 进入配网模式（M4 起真正开 BLE 广播） */
    uint32_t      pairing_ms;      /* 进配网模式的时刻（3 分钟无操作自动退出） */
} wesr_status_t;

void            AppState_Init(void);
void            AppState_Lock(void);
void            AppState_Unlock(void);
wesr_status_t  *AppState_Status(void);

#ifdef __cplusplus
}
#endif
