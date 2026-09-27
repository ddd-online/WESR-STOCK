#pragma once
#include "lvgl.h"
#include "wesr_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *Ui_Page3Create(void);
void Ui_Page3Update(const wesr_minute_t *m, const wesr_quote_t *q,
                    const wesr_stock_cfg_t *cfg, uint8_t idx, uint8_t count);

#ifdef __cplusplus
}
#endif
