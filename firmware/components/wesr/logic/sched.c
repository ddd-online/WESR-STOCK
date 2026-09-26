#include "wesr_logic.h"

#define RETRY_MIN_MS    5000u
#define IDLE_RECHECK_MS 60000u

static const uint32_t k_backoff[] = { 5000u, 10000u, 20000u, 30000u };

void wesr_sched_init(wesr_sched_t *s, uint16_t interval_s)
{
    s->page = 1;
    s->interval_s = interval_s ? interval_s : 15;
    s->next_ms = 0;
    s->fail_streak = 0;
    s->offline = false;
}

/* 切页：立刻到期，先用缓存画，不等网络 */
void wesr_sched_page(wesr_sched_t *s, uint8_t page)
{
    s->page = page;
    s->next_ms = 0;
}

static uint32_t interval_ms(const wesr_sched_t *s)
{
    return (uint32_t)s->interval_s * 1000u;
}

wesr_fetch_t wesr_sched_tick(wesr_sched_t *s, uint32_t now_ms, bool trading)
{
    if (!trading) {
        s->next_ms = now_ms + IDLE_RECHECK_MS;   /* 休市：一分钟后再看，不空转 */
        return WESR_FETCH_NONE;
    }
    if ((int32_t)(now_ms - s->next_ms) < 0) return WESR_FETCH_NONE;
    if (s->page == 4) {
        s->next_ms = now_ms + interval_ms(s);
        return WESR_FETCH_NONE;
    }
    return (s->page == 1) ? WESR_FETCH_QUOTES : WESR_FETCH_MINUTES;
}

void wesr_sched_result(wesr_sched_t *s, bool ok, uint32_t now_ms)
{
    if (ok) {
        s->fail_streak = 0;
        s->offline = false;
        s->next_ms = now_ms + interval_ms(s);
        return;
    }
    if (s->fail_streak < 255) s->fail_streak++;
    uint8_t idx = (uint8_t)(s->fail_streak - 1);
    if (idx > 3) idx = 3;
    uint32_t wait = k_backoff[idx];
    if (wait < RETRY_MIN_MS) wait = RETRY_MIN_MS;
    s->next_ms = now_ms + wait;
    s->offline = (s->fail_streak >= 5);
}
