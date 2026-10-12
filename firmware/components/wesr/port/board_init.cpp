#include "board_init.h"
#include "display_bsp.h"
#include "app_state.h"     /* 行情缓存状态打点见 Board_FlushStats */
#include "i2c_bsp.h"
#include "i2c_equipment.h"
#include "lvgl_bsp.h"
#include "adc_bsp.h"
#include "user_config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <unistd.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <new>

#define FB_W 400
#define FB_H 300
#define FB_STRIDE (FB_W / 8)          /* 50 字节/行 */

static DisplayPort *s_lcd = nullptr;
static I2cMasterBus *s_i2c = nullptr;
/* 不再维护显存镜像 —— dump 直接从面板缓冲反解（见 fb_is_black），
   省掉每帧 12 万次位操作，也不会出现"镜像与面板不一致"。 */

/* 直接从面板缓冲反解 (x,y) 的像素。
   面板约定：**置位 = 白**（白像素 → ColorWhite(0xff) → *p |= mask），所以这里返回"是否黑"。
   注意老代码的 s_fb 镜像把这件事写反了（fb_set 在 black 为真时置位），
   dump 出来的画面一直是反的；这里按面板真实语义给。 */
static bool fb_is_black(int x, int y)
{
    uint8_t *fb = s_lcd ? s_lcd->RLCD_FrameBuffer() : nullptr;
    if (!fb || x < 0 || y < 0 || x >= FB_W || y >= FB_H) return false;
    int inv_y  = FB_H - 1 - y;
    int index  = (x >> 1) * (FB_H >> 2) + (inv_y >> 2);
    uint8_t bit = (uint8_t)(7 - (((inv_y & 3) << 1) | (x & 1)));
    return ((fb[index] >> bit) & 1u) == 0;      /* 位清 = 黑 */
}

/* ---------- 性能基线打点（第 0 步：只测量，不改行为） ----------
   由 flush 回调累加，主循环调用 Board_FlushStats() 打印。
   现场实测用的 .bss 数据，几乎不占 CPU，不加锁（主循环与 LVGL 任务并发读写，
   偶发读到半更新的数字不影响量级判断）。 */
static uint32_t s_st_flush_n;
static uint32_t s_st_conv_us, s_st_conv_max;
static uint32_t s_st_lcd_us,  s_st_lcd_max;
static uint32_t s_st_flush_us, s_st_flush_max;
static uint32_t s_st_skip_n, s_st_skip_us;      /* 帧差短路掉的帧数与省下的时间 */
static uint32_t s_fb_hash;                       /* 上一帧面板缓冲的哈希 */
static bool     s_fb_hash_valid;
static uint32_t s_st_since_ms;
static uint32_t s_st_heap_min_kb = 0xFFFFFFFFu;
static uint32_t s_st_largest_min_b;
static uint32_t s_st_psram_min_kb = 0xFFFFFFFFu;

static inline uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

extern "C" void Board_FlushStats(void)
{
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_st_since_ms == 0) { s_st_since_ms = t; return; }
    if (t - s_st_since_ms < 30000 && s_st_flush_n < 100) return;
    s_st_since_ms = t;

    uint32_t n = s_st_flush_n;
    uint32_t hk = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    uint32_t lg = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t pk = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    if (hk < s_st_heap_min_kb) s_st_heap_min_kb = hk;
    if (pk < s_st_psram_min_kb) s_st_psram_min_kb = pk;
    if (s_st_largest_min_b == 0 || lg < s_st_largest_min_b) s_st_largest_min_b = lg;

    printf("[perf] flush n=%u mean=%u/%u/%u us (total/conv/lcd) max=%u/%u/%u"
           " skip=%u(%u us) heap=%u/%uKB largest=%uB psram=%u/%uKB lvgl_mean=%u us max=%u\n",
           (unsigned)n,
           n ? (unsigned)(s_st_flush_us / n) : 0u,
           n ? (unsigned)(s_st_conv_us / n) : 0u,
           n ? (unsigned)(s_st_lcd_us / n) : 0u,
           (unsigned)s_st_flush_max, (unsigned)s_st_conv_max, (unsigned)s_st_lcd_max,
           (unsigned)s_st_skip_n, (unsigned)s_st_skip_us,
           (unsigned)hk, (unsigned)s_st_heap_min_kb,
           (unsigned)lg, (unsigned)pk, (unsigned)s_st_psram_min_kb,
           (unsigned)Lvgl_stats_mean_us(), (unsigned)Lvgl_stats_max_us());

    /* 行情缓存状态：排查"某页不刷新"这类问题时，一眼能看出是没拉到、还是拉到了画不出来。
       顺便给状态条提供依据。 */
    {
        AppState_Lock();
        wesr_status_t *st = AppState_Status();
        bool m0 = st->minutes[0].valid, m1 = st->minutes[1].valid;
        bool m2 = st->minutes[2].valid, m3 = st->minutes[3].valid;
        printf("[state] page=%u group=%u idx=%u closed=%d offline=%d day=%lu"
               " quotes0=%d minutes=%d%d%d%d key=%u n0=%u ok=%d fail=%lu\n",
               (unsigned)st->page, (unsigned)st->group, (unsigned)st->idx,
               (int)st->closed, (int)st->offline, (unsigned long)st->data_day,
               (int)st->quotes[0].valid, (int)m0, (int)m1, (int)m2, (int)m3,
               (unsigned)st->minutes_key, (unsigned)st->minutes[0].n,
               (int)st->minutes_ok, (unsigned long)st->today_fail);
        AppState_Unlock();
    }

    s_st_flush_n = 0;
    s_st_conv_us = s_st_conv_max = 0;
    s_st_lcd_us = s_st_lcd_max = 0;
    s_st_flush_us = s_st_flush_max = 0;
    s_st_skip_n = s_st_skip_us = 0;
    Lvgl_stats_reset();
}

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint32_t t0 = now_us();

#if CONFIG_WESR_FLUSH_FAST
    /* 快路径：整帧批量转换（每 4 行 1 个整字节，无查表/读改写），再做帧差短路。
       full_refresh=1 时 area 恒为整屏，src_stride = 屏宽。 */
    s_lcd->RLCD_BlitLvglFrame((const uint16_t *)color_map, drv->hor_res);
    uint32_t t1 = now_us();

    /* 帧差短路：屏幕内容没变就不发 SPI。1-bit 屏没有中间态，整帧哈希是最可靠的判据：
       实测传感器 10s 周期里有大量"值没变"的空刷帧，每帧白付一次转换 + 15KB SPI。 */
    {
        uint8_t *fb = s_lcd->RLCD_FrameBuffer();
        const uint32_t fb_len = (uint32_t)s_lcd->RLCD_FrameLength();
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < fb_len; i++) h = (h ^ fb[i]) * 16777619u;
        bool changed = (!s_fb_hash_valid) || (h != s_fb_hash);
        s_fb_hash = h;
        s_fb_hash_valid = true;
        if (!changed) {
            uint32_t t3 = now_us();
            s_st_skip_n++;
            s_st_skip_us += t3 - t0;
            lv_disp_flush_ready(drv);
            s_st_flush_n++;
            return;
        }
    }
    s_lcd->RLCD_Display();
    uint32_t t3 = now_us();
    lv_disp_flush_ready(drv);
#else
    /* 老路径（对照用）：逐像素 LUT 查表 + 读改写。 */
    uint16_t *buffer = (uint16_t *)color_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            bool black = (*buffer < 0x7fff);
            s_lcd->RLCD_SetPixel(x, y, black ? ColorBlack : ColorWhite);
            buffer++;
        }
    }
    uint32_t t1 = now_us();
    s_lcd->RLCD_Display();
    uint32_t t3 = now_us();
    lv_disp_flush_ready(drv);
#endif

    s_st_conv_us += t1 - t0;
    s_st_lcd_us  += t3 - t1;          /* 含 RLCD_Display 的 SPI 排队 */
    s_st_flush_us += t3 - t0;
    if (t1 - t0 > s_st_conv_max) s_st_conv_max = t1 - t0;
    if (t3 - t1 > s_st_lcd_max)  s_st_lcd_max  = t3 - t1;
    if (t3 - t0 > s_st_flush_max) s_st_flush_max = t3 - t0;
    s_st_flush_n++;
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
}

extern "C" I2cMasterBus *Board_I2c(void) { return s_i2c; }

/* 半分辨率 ASCII dump：'#' = 黑（该 2×2 块里至少 2 个黑点），'.' = 白。
   120 行 × 200 列，够看版式；要看字体细节用 Board_DumpFbFull()。 */
extern "C" void Board_DumpFb(void)
{
    if (!s_lcd || !s_lcd->RLCD_FrameBuffer()) { printf("FB-BEGIN\n(no buffer)\nFB-END\n"); return; }
    printf("FB-BEGIN %dx%d\n", FB_W, FB_H);
    for (int y = 0; y < FB_H; y += 2) {
        char line[FB_W / 2 + 1];
        for (int x = 0; x < FB_W; x += 2) {
            int cnt = 0;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    if (fb_is_black(x + dx, y + dy)) cnt++;
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
    if (!s_lcd || !s_lcd->RLCD_FrameBuffer()) { printf("FB-BEGIN\n(no buffer)\nFB-END\n"); return; }
    printf("FB-BEGIN-FULL %dx%d\n", FB_W, FB_H);
    for (int y = 0; y < FB_H; y++) {
        char line[FB_W + 1];
        for (int x = 0; x < FB_W; x++) {
            line[x] = fb_is_black(x, y) ? '#' : '.';
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
