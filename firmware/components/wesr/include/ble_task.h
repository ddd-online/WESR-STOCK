#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 配网模式开关（spec §8：长按 KEY 进入/退出）：
   true  = 把 NimBLE 拉起来并开始广播 NUS，false = 停广播并断开当前连接。
   幂等，重复调同一个值不做事。 */
void Ble_Enable(bool on);

/* 开机早期调一次：建命令队列 + 命令执行任务。
   必须赶在 NimBLE / WiFi 之前 —— 它们初始化完内部堆就碎得拿不出这么大的任务栈了
   （实测在 Ble_Enable 里建 8KB 任务直接 pdFAIL，命令全都没人执行）。 */
void Ble_Start(void);

#ifdef __cplusplus
}
#endif
