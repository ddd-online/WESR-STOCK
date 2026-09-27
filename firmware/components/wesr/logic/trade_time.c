#include "wesr_logic.h"

/* 本地兜底：设备时间在交易时段内才发请求（集合竞价 09:15 起算） */
bool wesr_in_trading(uint16_t hhmm)
{
    return (hhmm >= 915 && hhmm <= 1130) || (hhmm >= 1300 && hhmm <= 1500);
}

uint8_t wesr_weekday(int year, int month, int day)
{
    /* Zeller 同余（把 1/2 月当作上一年的 13/14 月） */
    if (month < 3) { month += 12; year -= 1; }
    int k = year % 100, j = year / 100;
    int h = (day + (13 * (month + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;  /* 0=周六 */
    return (uint8_t)((h + 6) % 7);                                           /* 转成 0=周日 */
}
