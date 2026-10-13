#include "workload.h"
#include "stm32f4xx_hal.h"

uint32_t scaled_wcet_cycles[NUM_TASKS];

/* ------------------------------------------------------------------
 * CPU-time burn
 *
 * The burn must consume CPU TIME, not WALL-CLOCK TIME. The old version
 * spun until DWT->CYCCNT advanced by N cycles; DWT keeps counting while
 * the task is preempted, so any preemption was silently absorbed into
 * the burn and the task never paid it back. Under load the real demand
 * was far below the nominal U, which is why 110% produced zero misses.
 *
 * This version spins a fixed number of loop iterations. Iterations only
 * advance while this task is actually running, so a preempted job is
 * stretched by exactly the time it lost. The cycles->iterations ratio
 * is measured at boot by Workload_Calibrate().
 * ------------------------------------------------------------------ */

/* Calibrated cost of one spin iteration, in CPU cycles, Q16 fixed point. */
static uint32_t cyc_per_iter_q16 = (4u << 16);   /* safe default until calibrated */

/* Fixed-instruction loop, immune to the compiler's optimisation choices. */
static void __attribute__((noinline)) Workload_Spin(uint32_t n)
{
    if (n == 0) return;
    __asm volatile(
        "1: subs %0, %0, #1 \n"
        "   bne  1b         \n"
        : "+r"(n)
        :
        : "cc");
}

/* Call ONCE, after DWT is enabled and BEFORE osKernelStart(). */
void Workload_Calibrate(void)
{
    const uint32_t N = 1000000u;

    Workload_Spin(1000u);                /* warm the ART cache / prefetch */

    __disable_irq();
    uint32_t t0  = DWT->CYCCNT;
    Workload_Spin(N);
    uint32_t cyc = DWT->CYCCNT - t0;
    __enable_irq();

    cyc_per_iter_q16 = (uint32_t)(((uint64_t)cyc << 16) / N);
}

uint32_t Workload_GetCyclesPerIterQ16(void)
{
    return cyc_per_iter_q16;
}

/* Burn a given number of CPU cycles of THIS task's execution. */
void Workload_BurnCycles(uint32_t cycles)
{
    uint32_t iters = (uint32_t)(((uint64_t)cycles << 16) / cyc_per_iter_q16);
    Workload_Spin(iters);
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

/* Scale synthetic WCETs so total utilization hits target_u. */
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
    if (scale < 0.0f) scale = 0.0f;

    for (int i = 0; i < NUM_TASKS; i++) {
        uint32_t c = task_set[i].wcet_us;
        if (task_set[i].is_synthetic) {
            c = (uint32_t)(c * scale);
        }
        scaled_wcet_cycles[i] = c * (SystemCoreClock / 1000000U);
    }
}

/* Calibration check (call before the scheduler starts). Should match ~1%. */
uint32_t Workload_MeasureBurn(uint32_t requested_us)
{
    uint32_t t0 = DWT->CYCCNT;
    Workload_BurnUs(requested_us);
    uint32_t elapsed = DWT->CYCCNT - t0;
    return elapsed / (SystemCoreClock / 1000000U);
}
