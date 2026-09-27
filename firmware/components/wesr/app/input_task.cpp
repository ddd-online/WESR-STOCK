/* KEY 键：单击切页、双击换组/换股、长按进配网模式。
   去抖与"单击/双击/长按"判定交给官方 multi_button（port_bsp/button_bsp.c）——
   它把三种事件分别置到 GP18ButtonGroups 的 bit0/1/2，这里只做事件到语义的映射。 */
#include "input_task.h"
#include "button_bsp.h"
#include "quote_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static void input_task(void *arg)
{
    const EventBits_t all = set_bit_button(0) | set_bit_button(1) | set_bit_button(2);
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(GP18ButtonGroups, all, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(200));
        if (!bits) continue;
        if (bits & set_bit_button(2)) {
            Quote_NavSetPairing(true);        /* 长按：配网模式（M4 真正开 BLE） */
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
