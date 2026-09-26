#include "wesr_logic.h"

#define DAY_OPEN   930
#define AM_CLOSE   1130
#define PM_OPEN    1300
#define DAY_CLOSE  1500
#define AM_SPAN    (11 * 60 + 30 - (9 * 60 + 30))       /* 120 */
#define TOTAL_SPAN (AM_SPAN + 1 + (15 * 60 - 13 * 60))  /* 241 */

static int to_min(uint16_t hhmm) { return (hhmm / 100) * 60 + (hhmm % 100); }

/* 交易分钟序号：09:30→0 … 11:30→120、13:00→121 … 15:00→241；午休压掉，越界 clamp */
int wesr_minute_index(uint16_t hhmm)
{
    if (hhmm <= DAY_OPEN) return 0;
    if (hhmm <= AM_CLOSE) return to_min(hhmm) - to_min(DAY_OPEN);
    if (hhmm < PM_OPEN)   return AM_SPAN;                 /* 午休压成一个点 */
    if (hhmm <= DAY_CLOSE) return AM_SPAN + 1 + (to_min(hhmm) - to_min(PM_OPEN));
    return TOTAL_SPAN;
}

int wesr_index_to_x(int idx, int x0, int x1)
{
    if (idx < 0) idx = 0;
    if (idx > TOTAL_SPAN) idx = TOTAL_SPAN;
    return x0 + (int)((long)(x1 - x0) * idx / TOTAL_SPAN);
}
