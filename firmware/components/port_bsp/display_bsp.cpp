#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include "display_bsp.h"

DisplayPort::DisplayPort(int mosi, int scl, int dc, int cs, int rst, int width, int height, spi_host_device_t spihost) : 
mosi_(mosi), 
scl_(scl), 
dc_(dc), 
cs_(cs), 
rst_(rst), 
width_(width), 
height_(height) 
{
    esp_err_t        ret;
    spi_bus_config_t buscfg   = {};
    int              transfer = width_ * height_;
    /* IDF v6 把这些字段的类型收成了 gpio_num_t，C++ 不再隐式转换 int → 显式 cast */
    buscfg.miso_io_num                   = (gpio_num_t)-1;
    buscfg.mosi_io_num                   = (gpio_num_t)mosi;
    buscfg.sclk_io_num                   = (gpio_num_t)scl;
    buscfg.quadwp_io_num                 = (gpio_num_t)-1;
    buscfg.quadhd_io_num                 = (gpio_num_t)-1;
    buscfg.max_transfer_sz               = transfer;
    ret                                  = spi_bus_initialize(spihost, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = (gpio_num_t)dc_;
    io_config.cs_gpio_num = (gpio_num_t)cs_;
    /* 保持示例验证过的 10MHz。手册 §12.1.2 写口 tSCYC 最小 30ns（≈33MHz）、
       微雪自家 codec_board 甚至默认 60MHz，但**上板实测** 30MHz 只让 lcd 段
       从 684us 降到 592us（−92us/帧，约占整帧 1%），而整帧瓶颈早已不在 SPI。
       收益这么小就不冒信号完整性的险了（`tools/spi_bench.py` 可复现这个对比）。 */
    io_config.pclk_hz = 10 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spihost, &io_config, &io_handle));

    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst_);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    Set_ResetIOLevel(1);

    DisplayLen                = transfer >> 3; //(1byte 8ipex)
    DispBuffer                = (uint8_t *) heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    assert(DispBuffer);

#if (AlgorithmOptimization == 3)
	PixelIndexLUT = (uint16_t (*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	PixelBitLUT   = (uint8_t (*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(PixelIndexLUT);
    assert(PixelBitLUT);
    if(width_ == 400) {
        InitLandscapeLUT();
    } else {
        InitPortraitLUT();
    }
#endif
}

DisplayPort::~DisplayPort() {
}

void DisplayPort::RLCD_Init() {
    RLCD_Reset();

    RLCD_SendCommand(0xD6);  // NVM Load Control
	RLCD_SendData(0x17);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xD1); //Booster Enable
	RLCD_SendData(0x01);

	RLCD_SendCommand(0xC0); //Gate Voltage Control
	RLCD_SendData(0x11);   
	RLCD_SendData(0x04);   

	RLCD_SendCommand(0xC1); //VSHP Setting
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);

	RLCD_SendCommand(0xC2);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xC4);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);

	RLCD_SendCommand(0xC5);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xD8);
	RLCD_SendData(0xA6);
	RLCD_SendData(0xE9);

	RLCD_SendCommand(0xB2);
	RLCD_SendData(0x05);

	RLCD_SendCommand(0xB3);
	RLCD_SendData(0xE5);
	RLCD_SendData(0xF6);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0xB4);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0x62);
	RLCD_SendData(0x32);
	RLCD_SendData(0x03);
	RLCD_SendData(0x1F);

	RLCD_SendCommand(0xB7);
	RLCD_SendData(0x13);

	RLCD_SendCommand(0xB0);
	RLCD_SendData(0x64);

	RLCD_SendCommand(0x11); 
	vTaskDelay(pdMS_TO_TICKS(200));     
	RLCD_SendCommand(0xC9);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0x36);
	RLCD_SendData(0x48); 

	RLCD_SendCommand(0x3A);
	RLCD_SendData(0x11); 

	RLCD_SendCommand(0xB9);
	RLCD_SendData(0x20);

	RLCD_SendCommand(0xB8);
	RLCD_SendData(0x29);

	RLCD_SendCommand(0x21);

	RLCD_SendCommand(0x2A); 
	RLCD_SendData(0x12);
	RLCD_SendData(0x2A);

	RLCD_SendCommand(0x2B); 
	RLCD_SendData(0x00);
	RLCD_SendData(0xC7);

	RLCD_SendCommand(0x35);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0xD0);
	RLCD_SendData(0xFF);

	RLCD_SendCommand(0x38);
	RLCD_SendCommand(0x29);

    RLCD_ColorClear(ColorWhite);
}

void DisplayPort::RLCD_ColorClear(uint8_t color) {
    memset(DispBuffer, color, DisplayLen);
}

void DisplayPort::RLCD_Display() {
    RLCD_SendCommand(0x2A);     // Column Address Set
  	RLCD_SendData(0x12);
  	RLCD_SendData(0x2A);

  	RLCD_SendCommand(0x2B);     // Page Address Set
  	RLCD_SendData(0x00);
  	RLCD_SendData(0xC7);

  	RLCD_SendCommand(0x2c);     // Page Address Set

	RLCD_Sendbuffera(DispBuffer,DisplayLen);
}

void DisplayPort::RLCD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void DisplayPort::RLCD_SendCommand(uint8_t Reg) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, Reg, NULL, 0));
}

void DisplayPort::RLCD_SendData(uint8_t Data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, -1, &Data, 1));
}

void DisplayPort::RLCD_Sendbuffera(uint8_t *Data, int len) {
    /* 这里原来用 ESP_ERROR_CHECK：整屏 15000 字节要一块连续的内部 DMA 缓冲，
       配网模式（BLE 常驻）下内存紧张时 spicommon_dma_setup_priv_buffer 会失败，
       然后整块板子 abort 重启。刷不进最多是画面停一帧，不值得重启。 */
    esp_err_t e = esp_lcd_panel_io_tx_color(io_handle, -1, Data, len);
    if (e == ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(20));                 /* 让出 CPU，DMA 缓冲可能刚好被释放 */
    e = esp_lcd_panel_io_tx_color(io_handle, -1, Data, len);
    if (e != ESP_OK) ESP_LOGW("lcd", "整屏刷新失败(%d)，本帧不改画面", (int)e);
}

void DisplayPort::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) rst_, level ? 1 : 0);
}
#if (AlgorithmOptimization != 3)

void DisplayPort::RLCD_SetPortraitPixel(uint16_t x, uint16_t y, uint8_t color) {
    if((x >= width_) || (y >= height_)) {
  	  	ESP_LOGE("Pixel","Beyond the limit : (%d,%d)",x ,y);
        return;
  	}
#if (AlgorithmOptimization == 2)
	const uint16_t W4 = width_ >> 2;  

    uint16_t byte_x = x >> 2;        
    uint16_t byte_y = y >> 1;        

    uint32_t index = byte_y * W4 + byte_x;

    uint8_t local_x = x & 0x03; 
    uint8_t local_y = y & 0x01; 

    uint8_t bit = 7 - ((local_x << 1) | local_y);

    uint8_t mask = 1 << bit;

    if (color)
        DispBuffer[index] |= mask;
    else
        DispBuffer[index] &= ~mask;
#else
    uint16_t byte_x = x / 4;
    uint16_t byte_y = y / 2;

    uint32_t index = byte_y * (width_ / 4) + byte_x;

    uint8_t local_x = x % 4;  
    uint8_t local_y = y % 2;  
    uint8_t bit = 7 - (local_x * 2 + local_y);
    if (color)
        DispBuffer[index] |=  (1 << bit);
    else
        DispBuffer[index] &= ~(1 << bit);
#endif
}

void DisplayPort::RLCD_SetLandscapePixel(uint16_t x, uint16_t y, uint8_t color) {
    if (x >= width_ || y >= height_)
        return;
#if (AlgorithmOptimization == 2)

	uint16_t inv_y = (height_ - 1 - y);
    const uint16_t H4 = height_ >> 2;  
    uint16_t byte_x = x >> 1;          
    uint16_t block_y = inv_y >> 2;     
    uint32_t index = byte_x * H4 + block_y;
    uint8_t local_x = x & 0x01;        
    uint8_t local_y = inv_y & 0x03;    
    uint8_t bit = 7 - ((local_y << 1) | local_x);
    uint8_t mask = 1 << bit;
    if (color)
        DispBuffer[index] |= mask;
    else
        DispBuffer[index] &= ~mask;
#else
    uint16_t inv_y = height_ - 1 - y;

    uint16_t byte_x  = x / 2;           // 0..199
    uint16_t block_y = inv_y / 4;       // 0..74

    uint32_t index = byte_x * (height_ / 4) + block_y; 

    uint8_t local_x = x % 2;            // 0 or 1
    uint8_t local_y = inv_y % 4;        // 0..3

    uint8_t bit = 7 - (local_y * 2 + local_x);

    if (color)
        DispBuffer[index] |= (1 << bit);
    else
        DispBuffer[index] &= ~(1 << bit);
#endif
}

#endif


#if (AlgorithmOptimization == 3)

void DisplayPort::InitPortraitLUT() {
    uint16_t W4 = width_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t byte_y = y >> 1;
        uint8_t  local_y = y & 1;

        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 2;
            uint8_t  local_x = x & 3;

            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void DisplayPort::InitLandscapeLUT() {
    uint16_t H4 = height_ >> 2;

    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t inv_y = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y  = inv_y & 3;

        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 1;
            uint8_t  local_x = x & 1;

            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void DisplayPort::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    uint32_t idx = PixelIndexLUT[x][y];
    uint8_t  mask = PixelBitLUT[x][y];

    uint8_t *p = &DispBuffer[idx];

    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

#endif

/* 整帧批量转换：RGB565 画布 → ST7305 的 1bpp 面板格式。
   面板字节布局（横屏 400x300，与 AlgorithmOptimization==3 的 LUT 等价，已逐位反解+对拍验证）：
       idx = byte_x * 75 + block_y,  byte_x = x >> 1,  inv_y = 299 - y,  block_y = inv_y >> 2
       bit = 7 - ((local_y << 1) | local_x),  local_x = x & 1,  local_y = inv_y & 3
   ⇒ 一个目标字节锁定 (byte_x, block_y)，覆盖 4 个连续源行 ybot..ybot+3：
       bit0/1 ← ybot        bit2/3 ← ybot+1      bit4/5 ← ybot+2      bit6/7 ← ybot+3
       （bit 对内：左列 x=2*byte_x → 高位，右列 x+1 → 低位）
     其中 ybot = height - 4 - 4*block_y（bit0/1 是本组最上面一行，往下的行进更高的 bit）。
   这一版位序由 tools/bitorder_search.py 穷举"行序 × 左右序"组合、
   对逐像素参照实现逐字节比对唯一匹配得到；并有 test/test_blit.c 做常驻回归。
   遍历方式：外层按"每 4 行"分组（block_y 固定、byte_x 从 0 到 199 连续写 200 字节），
   内层 4 行 × 2 像素一次产出一个整字节 —— 无逐像素查表、无读改写。

   黑/白判定与原来逐像素路径完全一致（color_map 里 < 0x7fff 判黑 → 清位；白 → 置位），
   保证画面逐字节不回退。 */
void DisplayPort::RLCD_BlitLvglFrame(const uint16_t *lv_buf, int src_stride)
{
    /* 索引 = (左像素?2:0) + (右像素?1:0)：白=1（置位）、黑=0（清位），与老路径同向 */
    static const uint8_t kNib[4] = { 0x0, 0x1, 0x2, 0x3 };
    const int cols = width_ >> 1;                    /* 200 个目标字节 */
    const int nblk = height_ >> 2;                   /* 75 */

    for (int block_y = 0; block_y < nblk; block_y++) {
        const int ybot = height_ - 4 - 4 * block_y;  /* bit0/1 所在行（本组最上面一行） */
        const uint16_t *p0 = lv_buf + (size_t)ybot * src_stride;        /* bit0/1 */
        const uint16_t *p1 = p0 + src_stride;                           /* bit2/3 */
        const uint16_t *p2 = p1 + src_stride;                           /* bit4/5 */
        const uint16_t *p3 = p2 + src_stride;                           /* bit6/7 */
        uint8_t *out = DispBuffer + block_y;         /* 步进 75，逐 byte_x 递增 */

        for (int byte_x = 0; byte_x < cols; byte_x++, out += nblk) {
            const int x = byte_x << 1;
            int n0 = ((p0[x] >= 0x7fff) ? 2 : 0) + ((p0[x + 1] >= 0x7fff) ? 1 : 0);
            int n1 = ((p1[x] >= 0x7fff) ? 2 : 0) + ((p1[x + 1] >= 0x7fff) ? 1 : 0);
            int n2 = ((p2[x] >= 0x7fff) ? 2 : 0) + ((p2[x + 1] >= 0x7fff) ? 1 : 0);
            int n3 = ((p3[x] >= 0x7fff) ? 2 : 0) + ((p3[x + 1] >= 0x7fff) ? 1 : 0);
            *out = (uint8_t)(kNib[n0] | (kNib[n1] << 2) | (kNib[n2] << 4) | (kNib[n3] << 6));
        }
    }
}


#if 0 
    RLCD_SendCommand(0xD6);  // NVM Load Control
	RLCD_SendData(0x17);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xD1); //Booster Enable
	RLCD_SendData(0x01);

	RLCD_SendCommand(0xC0); //Gate Voltage Control
	RLCD_SendData(0x11);   
	RLCD_SendData(0x04);   

	RLCD_SendCommand(0xC1); //VSHP Setting
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);

	RLCD_SendCommand(0xC2);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xC4);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);
	RLCD_SendData(0x41);

	RLCD_SendCommand(0xC5);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xD8);
	RLCD_SendData(0xA6);
	RLCD_SendData(0xE9);

	RLCD_SendCommand(0xB2);
	RLCD_SendData(0x05);

	RLCD_SendCommand(0xB3);
	RLCD_SendData(0xE5);
	RLCD_SendData(0xF6);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0xB4);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0x62);
	RLCD_SendData(0x32);
	RLCD_SendData(0x03);
	RLCD_SendData(0x1F);

	RLCD_SendCommand(0xB7);
	RLCD_SendData(0x13);

	RLCD_SendCommand(0xB0);
	RLCD_SendData(0x64);

	RLCD_SendCommand(0x11); 
	vTaskDelay(pdMS_TO_TICKS(200));     
	RLCD_SendCommand(0xC9);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0x36);
	RLCD_SendData(0x48); 

	RLCD_SendCommand(0x3A);
	RLCD_SendData(0x11); 

	RLCD_SendCommand(0xB9);
	RLCD_SendData(0x20);

	RLCD_SendCommand(0xB8);
	RLCD_SendData(0x29);

	RLCD_SendCommand(0x21);

	RLCD_SendCommand(0x2A); 
	RLCD_SendData(0x12);
	RLCD_SendData(0x2A);

	RLCD_SendCommand(0x2B); 
	RLCD_SendData(0x00);
	RLCD_SendData(0xC7);

	RLCD_SendCommand(0x35);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0xD0);
	RLCD_SendData(0xFF);

	RLCD_SendCommand(0x38);
	RLCD_SendCommand(0x29);
#endif
