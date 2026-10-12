#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>


#define AlgorithmOptimization  3     //1:原始算法 2:采用移位算法 3:查表法   来优化CPU

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

class DisplayPort {
private:
    esp_lcd_panel_io_handle_t io_handle = NULL;
    uint32_t            i2c_data_pdMS_TICKS = 0;
    uint32_t            i2c_done_pdMS_TICKS = 0;
    const char         *TAG                 = "Display";
    int                 mosi_;
    int                 scl_;
    int                 dc_;
    int                 cs_;
    int                 rst_;
    int                 width_;
    int                 height_;
    uint8_t            *DispBuffer = NULL;
    int                 DisplayLen;
#if (AlgorithmOptimization == 3)
	uint16_t (*PixelIndexLUT)[300];
	uint8_t  (*PixelBitLUT  )[300];
	void InitPortraitLUT();
	void InitLandscapeLUT();
#endif

    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset(void);

public:
    DisplayPort(int mosi, int scl, int dc, int cs, int rst, int width, int height, spi_host_device_t spihost = SPI3_HOST);
    ~DisplayPort();
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
    /* 把 LVGL 的 RGB565 画布整帧转成 ST7305 要的 1bpp 面板格式。
       lv_buf 的步长（像素）由 src_stride 给出。整帧 15000 字节按顺序写，
       逐像素查表的老路径（RLCD_SetPixel）保留作对照。
       面板缓冲指针与长度供帧差比对用。 */
    void RLCD_BlitLvglFrame(const uint16_t *lv_buf, int src_stride);
    uint8_t *RLCD_FrameBuffer(void) { return DispBuffer; }
    int      RLCD_FrameLength(void) const { return DisplayLen; }
	#if (AlgorithmOptimization != 3)
    void RLCD_SetPortraitPixel(uint16_t x, uint16_t y, uint8_t color);      //竖屏显示
    void RLCD_SetLandscapePixel(uint16_t x, uint16_t y, uint8_t color);     //横屏显示
	#endif
	#if (AlgorithmOptimization == 3)
	void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
	#endif
};