#include "workload.h"
#include "stm32f4xx_hal.h"

uint32_t scaled_wcet_cycles[NUM_TASKS];

/* Burn CPU for a precise cycle count. Unsigned subtraction
   handles the DWT counter wrap at ~25.6s automatically. */
void Workload_BurnCycles(uint32_t cycles)
{
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
}

void Workload_BurnUs(uint32_t us)
{
    Workload_BurnCycles(us * (SystemCoreClock / 1000000U));
}

/* Fixed utilization contributed by the real (non-synthetic) tasks */
float Workload_GetRealUtilization(void)
{
    float u = 0.0f;
    for (int i = 0; i < NUM_TASKS; i++) {
        if (!task_set[i].is_synthetic) {
            u += (task_set[i].wcet_us / 1000.0f) / task_set[i].period_ms;
        }
    }
    return u;
}

/* Scale synthetic WCETs so total utilization hits target_u.
   Task set structure is unchanged — only load varies. */
void Workload_SetTargetUtilization(float target_u)
{
    float u_real  = 0.0f;
    float u_synth = 0.0f;

    for (int i = 0; i < NUM_TASKS; i++) {
        float u = (task_set[i].wcet_us / 1000.0f) / task_set[i].period_ms;
        if (task_set[i].is_synthetic) u_synth += u;
        else                          u_real  += u;
    }

    float scale = (target_u - u_real) / u_synth;
    if (scale < 0.0f) scale = 0.0f;   /* target below the fixed floor */

    for (int i = 0; i < NUM_TASKS; i++) {
        uint32_t c = task_set[i].wcet_us;
        if (task_set[i].is_synthetic) {
            c = (uint32_t)(c * scale);
        }
        scaled_wcet_cycles[i] = c * (SystemCoreClock / 1000000U);
    }
}

/* Calibration check: returns actual elapsed microseconds
   for a requested burn. Should match within ~1%. */
uint32_t Workload_MeasureBurn(uint32_t requested_us)
{
    uint32_t t0 = DWT->CYCCNT;
    Workload_BurnUs(requested_us);
    uint32_t elapsed = DWT->CYCCNT - t0;
    return elapsed / (SystemCoreClock / 1000000U);
}
