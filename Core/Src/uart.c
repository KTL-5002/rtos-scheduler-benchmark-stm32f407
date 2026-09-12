#include "uart.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

extern UART_HandleTypeDef huart2;

void UART_Print(const char *str)
{
    HAL_UART_Transmit(&huart2, (uint8_t*)str,
                      strlen(str), HAL_MAX_DELAY);
}

void UART_Printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    UART_Print(buf);
}
