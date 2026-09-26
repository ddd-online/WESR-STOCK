# 显式列文件：IDF 在"组件依赖扫描"阶段不允许 file(GLOB ... CONFIGURE_DEPENDS)。
# 每加一个源文件就补一行（Task 10–13 会往 ui/ 里加）。
idf_component_register(
    SRCS
    "logic/parse_quote.c"
    "logic/parse_minute.c"
    "logic/parse_meta.c"
    "logic/fmt_stock.c"
    "logic/chart_map.c"
    "logic/trade_time.c"
    "logic/nav.c"
    "logic/sched.c"
    "logic/chart_draw.c"
    "logic/defaults.c"
    "app/cfg_store.c"
    "port/board_init.cpp"
    "ui/ui_root.c"
    INCLUDE_DIRS "include" "ui"
    REQUIRES port_bsp app_bsp lvgl__lvgl
    # 不拉 json/cJSON：解析是手写的（IDF v6 起 json 组件也不再内置）
    PRIV_REQUIRES esp_wifi nvs_flash esp_http_client mbedtls esp_timer)
