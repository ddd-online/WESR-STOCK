#include "wesr_logic.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

#define CFG_KEY     "cfg"
#define CFG_VER_KEY "ver"
#define CFG_VERSION 1

/* 整个配置当一个 blob 存；版本号对不上就回默认值（结构体改过时不会读到脏数据） */
esp_err_t wesr_cfg_load(wesr_app_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wesr", NVS_READONLY, &h);
    if (err != ESP_OK) { wesr_cfg_defaults(cfg); return err; }
    uint8_t ver = 0;
    size_t len = sizeof *cfg;
    err = nvs_get_u8(h, CFG_VER_KEY, &ver);
    if (err == ESP_OK && ver == CFG_VERSION) err = nvs_get_blob(h, CFG_KEY, cfg, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof *cfg) {
        wesr_cfg_defaults(cfg);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t wesr_cfg_save(const wesr_app_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wesr", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, CFG_KEY, cfg, sizeof *cfg);
    if (err == ESP_OK) err = nvs_set_u8(h, CFG_VER_KEY, CFG_VERSION);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
