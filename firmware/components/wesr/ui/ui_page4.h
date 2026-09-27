#pragma once
#include "lvgl.h"
#include "app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *Ui_Page4Create(void);
void      Ui_Page4Update(const wesr_status_t *st);

#ifdef __cplusplus
}
#endif
