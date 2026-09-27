/* KEY 键：单击切页、双击换组/换股、长按进配网模式。
   去抖与"单击/双击/长按"判定交给官方 multi_button（port_bsp/button_bsp.c）——
   它把三种事件分别置到 GP18ButtonGroups 的 bit0/1/2，这里只做事件到语义的映射。 */
#include "input_task.h"
#include "button_bsp.h"
#include "quote_service.h"
#include "app_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static bool pairing_on(void)
{
    AppState_Lock();
    bool p = AppState_Status()->pairing;
    AppState_Unlock();
    return p;
}

static void input_task(void *arg)
{
    const EventBits_t all = set_bit_button(0) | set_bit_button(1) | set_bit_button(2);
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(GP18ButtonGroups, all, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(200));
        if (!bits) continue;
        if (bits & set_bit_button(2)) {
            /* 长按 = 进/出配网模式（spec §8：长按 3 秒进入，再长按 3 秒退出） */
            bool on = pairing_on();
            Quote_NavSetPairing(!on);
        } else if (pairing_on()) {
            /* 配网模式中单击/双击不生效，免得画面乱跳干扰配网（spec §8） */
            ESP_LOGI("input", "ignored: pairing mode");
        } else if (bits & set_bit_button(1)) {
            Quote_NavDouble();                /* 双击：第 2 页切组 / 第 3 页换股 */
        } else if (bits & set_bit_button(0)) {
            Quote_NavClick();                 /* 单击：切页 */
        }
    }
}

extern "C" void Input_TaskStart(void)
{
    Custom_ButtonInit();
    xTaskCreate(input_task, "input", 4096, NULL, 4, NULL);
}
