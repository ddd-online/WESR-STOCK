#pragma once
/* NVS 读写：这个头文件依赖 ESP-IDF，只在板端编译（logic/ 下面的文件不许 include 它） */
#include "esp_err.h"
#include "wesr_logic.h"

esp_err_t wesr_cfg_load(wesr_app_cfg_t *cfg);
esp_err_t wesr_cfg_save(const wesr_app_cfg_t *cfg);
