#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "lvgl_bsp.h"

static lv_disp_draw_buf_t disp_buf; 		// contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;      		// contains callback functions
static SemaphoreHandle_t lvgl_mux = NULL;

static const char *TAG = "LvglPort";

/* 性能基线打点：lv_timer_handler() 单轮耗时，含 flush（flush 在 handler 内同步跑完） */
static volatile uint32_t s_lv_n, s_lv_sum_us, s_lv_max_us;

uint32_t Lvgl_stats_mean_us(void) { uint32_t n = s_lv_n; return n ? s_lv_sum_us / n : 0; }
uint32_t Lvgl_stats_max_us(void)  { return s_lv_max_us; }
void     Lvgl_stats_reset(void)   { s_lv_n = 0; s_lv_sum_us = 0; s_lv_max_us = 0; }

static void Increase_lvgl_tick(void *arg)
{
  	lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool Lvgl_lock(int timeout_ms)
{
  	const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  	return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

void Lvgl_unlock(void)
{
  	assert(lvgl_mux && "bsp_display_start must be called first");
  	xSemaphoreGive(lvgl_mux);
}

static void Lvgl_port_task(void *arg)
{
  	uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	for(;;)
  	{
  	  	if (Lvgl_lock(-1)) 
  	  	{
  	  	  	uint32_t t0 = (uint32_t)esp_timer_get_time();
  	  	  	task_delay_ms = lv_timer_handler();
  	  	  	uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
  	  	  	/* C++26 对 volatile 的 ++/+= 已弃用且这里开了 -Werror，写展开形式 */
  	  	  	s_lv_sum_us = s_lv_sum_us + dt;
  	  	  	s_lv_n = s_lv_n + 1;
  	  	  	if (dt > s_lv_max_us) s_lv_max_us = dt;
  	  	  	//Release the mutex
  	  	  	Lvgl_unlock();
  	  	}
  	  	if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	  	} else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
  	  	}
  	  	vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  	}
}


void Lvgl_PortInit(int width, int height,DispFlushCb flush_cb) {
    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();
    lv_color_t *buffer1 = (lv_color_t *)heap_caps_malloc(width * height * sizeof(lv_color_t) , MALLOC_CAP_SPIRAM);
  	assert(buffer1);
	lv_color_t *buffer2 = (lv_color_t *)heap_caps_malloc(width * height * sizeof(lv_color_t) , MALLOC_CAP_SPIRAM);
  	assert(buffer2);

    lv_disp_draw_buf_init(&disp_buf, buffer1, buffer2, width * height);
    ESP_LOGI(TAG, "Register display driver to LVGL");

    lv_disp_drv_init(&disp_drv);
  	disp_drv.hor_res = width;
  	disp_drv.ver_res = height;
  	disp_drv.flush_cb = flush_cb;
	disp_drv.full_refresh = 1;
  	disp_drv.draw_buf = &disp_buf;
  	lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
  	esp_timer_create_args_t lvgl_tick_timer_args = {};
  	lvgl_tick_timer_args.callback = &Increase_lvgl_tick;
  	lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
  	ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  	ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreatePinnedToCore(Lvgl_port_task, "LVGL", 8 * 1024, NULL, 5, NULL, 0);
}
