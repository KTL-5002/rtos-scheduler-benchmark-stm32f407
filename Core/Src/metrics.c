#include "metrics.h"
#include "workload.h"
#include "uart.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

volatile TaskMetrics_t metrics[NUM_TASKS];

/* Timebase: maps a tick number to the DWT cycle count at the instant that
 * tick occurred. SysTick->VAL gives cycles since the last tick boundary,
 * so the anchor is exact to a few cycles. One anchor shared by all tasks;
 * unsigned arithmetic stays valid across counter wraps. */
static volatile uint8_t tb_ready = 0;
static TickType_t       tb_tick0;
static uint32_t         tb_cyc0;
static uint32_t         tb_cyc_per_tick;

void Metrics_TimebaseInit(void)
{
    taskENTER_CRITICAL();               /* masks SysTick */
    if (!tb_ready) {
        uint32_t v1, v2, cyc, pend;

        /* SysTick counts down; retry if it reloaded mid-read. */
        do {
            v1   = SysTick->VAL;
            pend = SCB->ICSR & SCB_ICSR_PENDSTSET_Msk;
            cyc  = DWT->CYCCNT;
            v2   = SysTick->VAL;
        } while (v2 > v1);

        TickType_t t = xTaskGetTickCount();
        if (pend) t++;                  /* boundary happened, kernel hasn't counted it yet */

        tb_cyc_per_tick = SysTick->LOAD + 1u;          /* 16800 at 168 MHz / 10 kHz */
        tb_tick0 = t;
        tb_cyc0  = cyc - (SysTick->LOAD - v1);
        tb_ready = 1;
    }
    taskEXIT_CRITICAL();
}

uint32_t Metrics_TickToCyc(TickType_t tick)
{
    return tb_cyc0 + (uint32_t)(tick - tb_tick0) * tb_cyc_per_tick;
}

void Metrics_Init(void)
{
    for (int i = 0; i < NUM_TASKS; i++) {
        metrics[i].jobs         = 0;
        metrics[i].misses       = 0;
        metrics[i].last_resp_us = 0;
        metrics[i].max_resp_us  = 0;
        metrics[i].min_resp_us  = 0xFFFFFFFF;
    }
}

void Metrics_RecordCompletion(uint32_t id, uint32_t release_cyc, uint32_t done_cyc)
{
    uint32_t cycles  = done_cyc - release_cyc;
    uint32_t resp_us = cycles / (SystemCoreClock / 1000000U);

    metrics[id].jobs++;
    metrics[id].last_resp_us = resp_us;

    if (resp_us > metrics[id].max_resp_us) metrics[id].max_resp_us = resp_us;
    if (resp_us < metrics[id].min_resp_us) metrics[id].min_resp_us = resp_us;

    if (resp_us > (task_set[id].deadline_ms * 1000U)) {
        metrics[id].misses++;
    }
}

void Metrics_Print(void)
{
    UART_Print("\r\n task      jobs   miss   last    max    min  (us)\r\n");
    for (int i = 0; i < NUM_TASKS; i++) {
        UART_Printf(" %-8s %6lu %6lu %6lu %6lu %6lu\r\n",
                    task_set[i].name,
                    (unsigned long)metrics[i].jobs,
                    (unsigned long)metrics[i].misses,
                    (unsigned long)metrics[i].last_resp_us,
                    (unsigned long)metrics[i].max_resp_us,
                    (unsigned long)(metrics[i].min_resp_us == 0xFFFFFFFF ? 0 : metrics[i].min_resp_us));
    }
}
