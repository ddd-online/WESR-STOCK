#pragma once
#include "wesr_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 行情主循环（不返回）：按当前页决定拉什么，处理退避与状态条。
   设计：第 1 页只拉 8 只批量快照；第 2 页拉当前组 4 只分时；第 3 页拉选中那 1 只分时。 */
void Quote_Run(wesr_app_cfg_t *cfg);

/* 把缓存里的行情刷到 UI（内部会取 LVGL 锁） */
void Quote_RefreshUi(void);

/* 切页：同步内部页状态并立刻用缓存重画（Task 16 的按键切页也走这里） */
void Quote_SetPage(uint8_t page);
/* 把 net_task 里那份"活的配置"登记进来（开机就要调，否则 Quote_Cfg 是空的） */
void Quote_SetCfg(wesr_app_cfg_t *cfg);
/* 当前生效的配置：配网下发后直接改它（本地默认值、按键、行情都读同一份） */
wesr_app_cfg_t *Quote_Cfg(void);
/* 配置改完了叫一声：重算导航边界、作废行情缓存、立刻重画 */
void Quote_CfgChanged(void);

/* 按键动作（KEY 键）：单击切页、双击换组/换股、长按进配网模式 */
void Quote_NavClick(void);
void Quote_NavDouble(void);
void Quote_NavSetPairing(bool on);

#ifdef __cplusplus
}
#endif
