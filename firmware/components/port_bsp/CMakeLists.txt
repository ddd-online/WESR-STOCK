idf_component_register(
    SRCS 
    "i2c_bsp.cpp" 
    "display_bsp.cpp" 
    "sdcard_bsp.cpp" 
    "./src/multi_button/multi_button.c" 
    "button_bsp.c"
    "./i2c_equipment.cpp"
    "./adc_bsp.cpp"
    PRIV_REQUIRES 
    SensorLib
    REQUIRES
    # 这些只被 .cpp 用到；但端口层的公有头（display_bsp.h / i2c_bsp.h / adc_bsp.h…）都 include 它们，
    # 所以必须是公有依赖（PRIV_REQUIRES 不会传给消费者，原本会让上层编译不过）。
    # IDF v6 起头文件由 esp_driver_* 组件提供，必须显式声明（v5 时代靠 driver 传递）。
    driver
    esp_driver_i2c
    esp_driver_spi
    esp_driver_gpio
    esp_lcd
    sdmmc
    esp_timer
    esp_adc
    esp_driver_sdmmc
    fatfs
    INCLUDE_DIRS 
    "./" 
    "./src/multi_button")
