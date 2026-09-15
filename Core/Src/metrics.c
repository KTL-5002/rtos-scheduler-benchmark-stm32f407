#include "metrics.h"
#include "workload.h"
#include "uart.h"
#include "stm32f4xx_hal.h"

volatile TaskMetrics_t metrics[NUM_TASKS];

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

/* Called from task context at job completion.
   Unsigned subtraction handles DWT wrap. */
void Metrics_RecordCompletion(uint32_t id, uint32_t release_cyc, uint32_t done_cyc)
{
    uint32_t cycles  = done_cyc - release_cyc;
    uint32_t resp_us = cycles / (SystemCoreClock / 1000000U);

    metrics[id].jobs++;
    metrics[id].last_resp_us = resp_us;

    if (resp_us > metrics[id].max_resp_us) metrics[id].max_resp_us = resp_us;
    if (resp_us < metrics[id].min_resp_us) metrics[id].min_resp_us = resp_us;

    /* Deadline miss: response time exceeded the relative deadline */
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
