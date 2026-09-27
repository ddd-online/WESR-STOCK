#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void    Ui_Init(void);
void    Ui_ShowPage(uint8_t page);
uint8_t Ui_CurrentPage(void);
void    Ui_StatusBar(const char *text, bool alert);
/* 顶栏状态（电量/充电、WiFi 与蓝牙的连接状态）——从 AppState 驱动，不要写死 */
void    Ui_UpdateHeader(const wesr_status_t *st);

#ifdef __cplusplus
}
#endif
