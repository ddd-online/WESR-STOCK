#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void    Ui_Init(void);
void    Ui_ShowPage(uint8_t page);
uint8_t Ui_CurrentPage(void);
void    Ui_StatusBar(const char *text, bool alert);

#ifdef __cplusplus
}
#endif
