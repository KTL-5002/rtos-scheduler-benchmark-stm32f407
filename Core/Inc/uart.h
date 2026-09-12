#ifndef UART_H
#define UART_H
#include "stm32f4xx_hal.h"
#include <stdarg.h>

void UART_Print(const char *str);
void UART_Printf(const char *fmt, ...);

#endif
