#include "wesr_logic.h"

uint8_t wesr_nav_pages(void) { return 4; }

void wesr_nav_set_count(wesr_nav_t *n, uint8_t count)
{
    if (count > WESR_MAX_STOCKS) count = WESR_MAX_STOCKS;
    n->count = count;
    if (count <= 4) n->group = 0;                    /* 组 2 不存在 */
    if (count == 0) n->idx = 0;
    else if (n->idx >= count) n->idx = (uint8_t)(count - 1);
}

void wesr_nav_init(wesr_nav_t *n, uint8_t count)
{
    n->page = 1;
    n->group = 0;
    n->idx = 0;
    wesr_nav_set_count(n, count);
}

void wesr_nav_click(wesr_nav_t *n)
{
    n->page = (uint8_t)(n->page >= wesr_nav_pages() ? 1 : n->page + 1);
}

/* 双击：第 2 页切组、第 3 页换股；第 1、4 页无动作 */
void wesr_nav_double(wesr_nav_t *n)
{
    if (n->page == 2) {
        if (n->count > 4) n->group = (uint8_t)(n->group ? 0 : 1);
    } else if (n->page == 3) {
        if (n->count > 0) n->idx = (uint8_t)((n->idx + 1) % n->count);
    }
}

uint8_t wesr_nav_group_start(const wesr_nav_t *n)
{
    return (n->group == 1 && n->count > 4) ? 4 : 0;
}
