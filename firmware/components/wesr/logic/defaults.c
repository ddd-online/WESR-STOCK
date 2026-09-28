#include "wesr_logic.h"
#include <ctype.h>
#include <string.h>

/* 编译期默认股票池：买来就能上电看到东西，不用等小程序配网（M4） */
static const struct { const char *code; const char *name; } k_defaults[WESR_MAX_STOCKS] = {
    { "sh603936", "博敏电子" }, { "sz002436", "兴森科技" },
    { "sh603920", "世运电路" }, { "sh605058", "澳弘电子" },
    { "sz000636", "风华高科" }, { "sh605258", "协和电子" },
    { "sh603867", "新化股份" }, { "sz002815", "崇达技术" },
};

void wesr_cfg_defaults(wesr_app_cfg_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->refresh_sec = 15;
    cfg->count = WESR_MAX_STOCKS;
    for (int i = 0; i < WESR_MAX_STOCKS; i++) {
        strncpy(cfg->stocks[i].code, k_defaults[i].code, WESR_CODE_LEN - 1);
        strncpy(cfg->stocks[i].name, k_defaults[i].name, WESR_NAME_LEN - 1);
    }
    wesr_make_marks(cfg->stocks, cfg->count);
    /* WiFi 默认值由调用方从 menuconfig 填（见 Task 10/15） */
}

/* 代码格式：sh|sz|bj + 6 位数字 */
bool wesr_code_valid(const char *code)
{
    if (!code || strlen(code) != 8) return false;
    if (!(strncmp(code, "sh", 2) == 0 || strncmp(code, "sz", 2) == 0 ||
          strncmp(code, "bj", 2) == 0)) return false;
    for (int i = 2; i < 8; i++) {
        if (!isdigit((unsigned char)code[i])) return false;
    }
    return true;
}
