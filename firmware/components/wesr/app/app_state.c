#include "app_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static wesr_status_t   s_status;
static SemaphoreHandle_t s_mux;

void AppState_Init(void)
{
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
}

void AppState_Lock(void)
{
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
}

void AppState_Unlock(void)
{
    if (s_mux) xSemaphoreGive(s_mux);
}

wesr_status_t *AppState_Status(void)
{
    return &s_status;
}
