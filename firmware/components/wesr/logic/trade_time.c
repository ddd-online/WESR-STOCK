#include "wesr_logic.h"

/* 本地兜底：设备时间在交易时段内才发请求（集合竞价 09:15 起算） */
bool wesr_in_trading(uint16_t hhmm)
{
    return (hhmm >= 915 && hhmm <= 1130) || (hhmm >= 1300 && hhmm <= 1500);
}
