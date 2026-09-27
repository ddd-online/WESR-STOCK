#pragma once
#include "lvgl.h"
#include "wesr_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *Ui_Page2Create(void);
/* m[4] 是当前组的四只分时；start = 0 或 4（组起点） */
void Ui_Page2Update(const wesr_minute_t *m, const wesr_quote_t *q,
                    const wesr_stock_cfg_t *cfg, uint8_t start, uint8_t count);

#ifdef __cplusplus
}
#endif
