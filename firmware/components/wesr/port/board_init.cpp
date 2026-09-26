#include "board_init.h"
#include "display_bsp.h"
#include "i2c_bsp.h"
#include "i2c_equipment.h"
#include "lvgl_bsp.h"
#include "adc_bsp.h"
#include "user_config.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <unistd.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>

#define FB_W 400
#define FB_H 300
#define FB_STRIDE (FB_W / 8)          /* 50 字节/行 */

static DisplayPort *s_lcd = nullptr;
static I2cMasterBus *s_i2c = nullptr;
/* 显存镜像：官方 DisplayPort 的内部缓冲是私有的，这里在 flush 时同步记一份，供 dump 用 */
static uint8_t *s_fb = nullptr;

static inline void fb_set(int x, int y, bool black)
{
    if (!s_fb || x < 0 || y < 0 || x >= FB_W || y >= FB_H) return;
    uint8_t *p = &s_fb[y * FB_STRIDE + (x >> 3)];
    if (black) *p |= (uint8_t)(0x80u >> (x & 7));
    else       *p &= (uint8_t)~(0x80u >> (x & 7));
}

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint16_t *buffer = (uint16_t *)color_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            bool black = (*buffer < 0x7fff);
            s_lcd->RLCD_SetPixel(x, y, black ? ColorBlack : ColorWhite);
            fb_set(x, y, black);
            buffer++;
        }
    }
    s_lcd->RLCD_Display();
    lv_disp_flush_ready(drv);
}

extern "C" void Board_Init(void)
{
    static I2cMasterBus i2c(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, 0);
    s_i2c = &i2c;
    static DisplayPort lcd(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN, RLCD_CS_PIN,
                           RLCD_RST_PIN, FB_W, FB_H);
    s_lcd = &lcd;
    lcd.RLCD_Init();
    Lvgl_PortInit(FB_W, FB_H, Lvgl_FlushCallback);
    Adc_PortInit();
    Rtc_Setup(&i2c, 0x51);
    s_fb = (uint8_t *)heap_caps_malloc(FB_STRIDE * FB_H, MALLOC_CAP_SPIRAM);
    if (s_fb) memset(s_fb, 0, FB_STRIDE * FB_H);
}

extern "C" I2cMasterBus *Board_I2c(void) { return s_i2c; }

/* 半分辨率 ASCII dump：'#' = 黑（该 2×2 块里至少 2 个黑点），'.' = 白。
   120 行 × 200 列，够看版式；要看字体细节用 Board_DumpFbFull()。 */
extern "C" void Board_DumpFb(void)
{
    if (!s_fb) { printf("FB-BEGIN\n(no buffer)\nFB-END\n"); return; }
    printf("FB-BEGIN %dx%d\n", FB_W, FB_H);
    for (int y = 0; y < FB_H; y += 2) {
        char line[FB_W / 2 + 1];
        for (int x = 0; x < FB_W; x += 2) {
            int cnt = 0;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int px = x + dx, py = y + dy;
                    if (s_fb[py * FB_STRIDE + (px >> 3)] & (0x80u >> (px & 7))) cnt++;
                }
            }
            line[x / 2] = (cnt >= 2) ? '#' : '.';
        }
        line[FB_W / 2] = 0;
        printf("%s\n", line);
        vTaskDelay(1);          /* 让出 CPU：串口写是阻塞的，不让 IDLE 会触发任务看门狗 */
    }
    printf("FB-END\n");
}

/* 全分辨率 dump（1 字符 = 1 像素，200 行×400 列 ≈ 120KB，串口约 10 秒）：
   按 'd' 键触发；用来核对文字/斜纹这类细节。 */
extern "C" void Board_DumpFbFull(void)
{
    if (!s_fb) { printf("FB-BEGIN\n(no buffer)\nFB-END\n"); return; }
    printf("FB-BEGIN-FULL %dx%d\n", FB_W, FB_H);
    for (int y = 0; y < FB_H; y++) {
        char line[FB_W + 1];
        for (int x = 0; x < FB_W; x++) {
            line[x] = (s_fb[y * FB_STRIDE + (x >> 3)] & (0x80u >> (x & 7))) ? '#' : '.';
        }
        line[FB_W] = 0;
        printf("%s\n", line);
        vTaskDelay(1);          /* 同上：全分辨率 300 行，每行让出一次 */
    }
    printf("FB-END\n");
}

/* 串口有字节进来时返回它（不阻塞），没有返回 -1 */
extern "C" int Board_PollKey(void)
{
    fd_set r;
    struct timeval tv = { 0, 0 };
    FD_ZERO(&r);
    FD_SET(STDIN_FILENO, &r);
    if (select(STDIN_FILENO + 1, &r, NULL, NULL, &tv) <= 0) return -1;
    int c = getchar();
    return (c == EOF) ? -1 : c;
}
