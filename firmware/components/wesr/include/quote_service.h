#pragma once
#include "wesr_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 行情主循环（不返回）：按当前页决定拉什么，处理退避与状态条。
   设计：第 1 页只拉 8 只批量快照；第 2 页拉当前组 4 只分时；第 3 页拉选中那 1 只分时。 */
void Quote_Run(const wesr_app_cfg_t *cfg);

/* 把缓存里的行情刷到 UI（内部会取 LVGL 锁） */
void Quote_RefreshUi(void);

#ifdef __cplusplus
}
#endif
