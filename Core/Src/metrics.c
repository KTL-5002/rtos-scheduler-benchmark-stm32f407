#include "metrics.h"
#include "workload.h"
#include "uart.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <math.h>

volatile TaskMetrics_t metrics[MAX_TASKS];

/* R/D in percent: fine below the deadline, coarse above it. */
const uint16_t RESP_BIN_EDGES[RESP_BINS - 1] = {
    10, 20, 30, 40, 50, 60, 70, 80, 90, 100,     /* met deadline   */
    125, 150, 200, 500, 1000,                     /* missed, by how much */
};

/* ------------------------------------------------------------------
 * Timebase: maps a FreeRTOS tick number to the DWT cycle count at the
 * instant that tick occurred.
 *
 * The anchor is exact to a few cycles: SysTick->VAL says how many
 * cycles have passed since the last tick boundary, so we do not need
 * to be running AT a tick boundary to find one. (The old per-task
 * anchor read CYCCNT whenever the task first ran, up to a full tick
 * = 100 us after the boundary, which made every response look up to
 * 100 us shorter than it was.)
 *
 * One global anchor shared by all tasks. Unsigned arithmetic keeps
 * the mapping valid across both counter wraps, as long as a single
 * response time stays under ~25.6 s.
 * ------------------------------------------------------------------ */
static volatile uint8_t tb_ready = 0;
static TickType_t       tb_tick0;
static uint32_t         tb_cyc0;
static uint32_t         tb_cyc_per_tick;

void Metrics_TimebaseInit(void)
{
    taskENTER_CRITICAL();               /* masks SysTick */
    if (!tb_ready) {
        uint32_t v1, v2, cyc, pend;

        /* SysTick counts down. Retry if it reloaded mid-read, so that
           v1, cyc and the pending flag all describe the same tick period. */
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
        tb_cyc0  = cyc - (SysTick->LOAD - v1);         /* cycles back to that boundary */
        tb_ready = 1;
    }
    taskEXIT_CRITICAL();
}

uint32_t Metrics_TickToCyc(TickType_t tick)
{
    return tb_cyc0 + (uint32_t)(tick - tb_tick0) * tb_cyc_per_tick;
}

/* Clears ALL slots (MAX_TASKS), so it is safe to call before the task
   count for this run is known. */
void Metrics_Init(void)
{
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        metrics[i].jobs          = 0;
        metrics[i].misses        = 0;
        metrics[i].last_resp_us  = 0;
        metrics[i].max_resp_us   = 0;
        metrics[i].min_resp_us   = 0xFFFFFFFF;
        metrics[i].sum_resp_us   = 0;
        metrics[i].sumsq_resp_us = 0;
        metrics[i].starts        = 0;
        metrics[i].max_start_us  = 0;
        metrics[i].sum_start_us  = 0;
        for (uint32_t b = 0; b < RESP_BINS; b++) metrics[i].hist[b] = 0;
    }
}

/* Called from task context at the top of every job. */
void Metrics_RecordStart(uint32_t id, uint32_t release_cyc, uint32_t start_cyc)
{
    uint32_t us = (start_cyc - release_cyc) / (SystemCoreClock / 1000000U);
    metrics[id].starts++;
    metrics[id].sum_start_us += us;
    if (us > metrics[id].max_start_us) metrics[id].max_start_us = us;
}

/* Called from task context at job completion.
   Unsigned subtraction handles DWT wrap. */
void Metrics_RecordCompletion(uint32_t id, uint32_t release_cyc, uint32_t done_cyc)
{
    uint32_t cycles  = done_cyc - release_cyc;
    uint32_t resp_us = cycles / (SystemCoreClock / 1000000U);

    metrics[id].jobs++;
    metrics[id].last_resp_us   = resp_us;
    metrics[id].sum_resp_us   += resp_us;
    metrics[id].sumsq_resp_us += (uint64_t)resp_us * resp_us;

    if (resp_us > metrics[id].max_resp_us) metrics[id].max_resp_us = resp_us;
    if (resp_us < metrics[id].min_resp_us) metrics[id].min_resp_us = resp_us;

    /* histogram of R/D (percent); 64-bit so long overload responses can't overflow */
    uint64_t pct = ((uint64_t)resp_us * 100u) / ((uint64_t)task_set[id].deadline_ms * 1000u);
    uint32_t b = 0;
    while (b < RESP_BINS - 1 && pct >= RESP_BIN_EDGES[b]) b++;
    /* R == D exactly is a met deadline (miss means R > D): keep it in the
       last "met" bin so the histogram agrees with the miss counter */
    if (resp_us <= task_set[id].deadline_ms * 1000U && b > 9) b = 9;
    metrics[id].hist[b]++;

    /* Deadline miss: response time exceeded the relative deadline */
    if (resp_us > (task_set[id].deadline_ms * 1000U)) {
        metrics[id].misses++;
    }
}

uint32_t Metrics_MeanResp(uint32_t id)
{
    uint32_t n = metrics[id].jobs;
    return n ? (uint32_t)(metrics[id].sum_resp_us / n) : 0u;
}

uint32_t Metrics_SdResp(uint32_t id)
{
    uint32_t n = metrics[id].jobs;
    if (n < 2) return 0u;
    double mean = (double)metrics[id].sum_resp_us / n;
    double var  = (double)metrics[id].sumsq_resp_us / n - mean * mean;
    return (var > 0.0) ? (uint32_t)(sqrt(var) + 0.5) : 0u;
}

uint32_t Metrics_MeanStart(uint32_t id)
{
    uint32_t n = metrics[id].starts;
    return n ? (uint32_t)(metrics[id].sum_start_us / n) : 0u;
}

void Metrics_Print(void)
{
    UART_Print("\r\n task      jobs   miss    max    min   mean     sd  start_mean start_max (us)\r\n");
    for (uint32_t i = 0; i < NUM_TASKS; i++) {
        UART_Printf(" %-8s %6lu %6lu %6lu %6lu %6lu %6lu %11lu %9lu\r\n",
                    task_set[i].name,
                    (unsigned long)metrics[i].jobs,
                    (unsigned long)metrics[i].misses,
                    (unsigned long)metrics[i].max_resp_us,
                    (unsigned long)(metrics[i].min_resp_us == 0xFFFFFFFF ? 0 : metrics[i].min_resp_us),
                    (unsigned long)Metrics_MeanResp(i),
                    (unsigned long)Metrics_SdResp(i),
                    (unsigned long)Metrics_MeanStart(i),
                    (unsigned long)metrics[i].max_start_us);
    }
}

/* ------------------------------------------------------------------
 * CPU utilization from FreeRTOS run-time stats.
 *
 * FreeRTOSConfig.h sets configGENERATE_RUN_TIME_STATS = 1 with this DWT
 * cycle counter as the clock, so the kernel adds up, at every context
 * switch, exactly how many cycles each task ran - including the idle
 * task. Utilization = 1 - idle cycles / window cycles. The idle task's
 * counter is current whenever another task (here the reporter) is
 * running, which is exactly when Begin and End are called.
 *
 * The 32-bit counters wrap after 25.6 s; one run is ~11 s from reset.
 * ------------------------------------------------------------------ */
uint32_t Metrics_RunTimeCounter(void)
{
    return DWT->CYCCNT;
}

static uint32_t cpu_idle0, cpu_cyc0;

void Metrics_CpuBegin(void)
{
    cpu_idle0 = ulTaskGetIdleRunTimeCounter();
    cpu_cyc0  = DWT->CYCCNT;
}

uint32_t Metrics_CpuEnd(void)
{
    uint32_t idle = ulTaskGetIdleRunTimeCounter() - cpu_idle0;
    uint32_t cyc  = DWT->CYCCNT - cpu_cyc0;
    if (cyc == 0) return 0;
    if (idle > cyc) idle = cyc;
    return 10000u - (uint32_t)(((uint64_t)idle * 10000u) / cyc);
}
