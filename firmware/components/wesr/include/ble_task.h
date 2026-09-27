#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 配网模式开关（spec §8：长按 KEY 进入/退出）：
   true  = 把 NimBLE 拉起来并开始广播 NUS，false = 停广播并断开当前连接。
   幂等，重复调同一个值不做事。 */
void Ble_Enable(bool on);

#ifdef __cplusplus
}
#endif
