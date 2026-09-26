#include "wesr_logic.h"
#include <ctype.h>
#include <string.h>

/* 编译期默认股票池：买来就能上电看到东西，不用等小程序配网（M4） */
static const struct { const char *code; const char *name; } k_defaults[WESR_MAX_STOCKS] = {
    { "sh600519", "贵州茅台" }, { "sz300750", "宁德时代" },
    { "sz002594", "比亚迪"   }, { "sh600036", "招商银行" },
    { "sh601318", "中国平安" }, { "sh000300", "沪深300"  },
    { "sz000858", "五粮液"   }, { "sh600900", "长江电力" },
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
