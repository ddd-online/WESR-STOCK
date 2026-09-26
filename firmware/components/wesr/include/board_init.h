#pragma once
/* 板级初始化：复用官方 port_bsp/app_bsp 的 C++ 类，所以这一层是 C++ */
#ifdef __cplusplus
class I2cMasterBus;
extern "C" {
#else
typedef struct I2cMasterBus I2cMasterBus;
#endif

void Board_Init(void);
I2cMasterBus *Board_I2c(void);

/* 调试用：把 1-bit 显存 dump 成 ASCII 到串口（400×300 → 半分辨率 200×150），
   用于在没有相机/眼睛的情况下核对画面。见 README 的"无屏验证"一节。 */
void Board_DumpFb(void);
void Board_DumpFbFull(void);
int  Board_PollKey(void);      /* 串口有输入时返回该字节，否则 -1 */

#ifdef __cplusplus
}
#endif
