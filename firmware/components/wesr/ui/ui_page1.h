#pragma once
#include "lvgl.h"
#include "wesr_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *Ui_Page1Create(void);
void Ui_Page1SetClock(const char *hhmm, const char *date);
void Ui_Page1SetEnv(float temp_c, float humi_pct);
void Ui_Page1Update(const wesr_quote_t *q, const wesr_stock_cfg_t *cfg, uint8_t count);

#ifdef __cplusplus
}
#endif
