#include "task_bodies.h"
#include "task_config.h"
#include "workload.h"
#include "metrics.h"
#include "motor.h"
#include "lis3dsh.h"
#include "uart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "scheduler.h"
#define MEASURE_WINDOW_MS  10000u

extern volatile uint32_t idle_counter;

/* Every measured task blocks here until the common first release,
 * RELEASE_START_TICK, so all tasks are released together (critical
 * instant) and in step with the EDF hook. Returns the first release tick;
 * vTaskDelayUntil() then advances it by one period per job, even when a
 * job overruns, so last_wake is always the release of the job about to run. */
static TickType_t WaitForFirstRelease(void)
{
    Metrics_TimebaseInit();

    TickType_t start = Scheduler_GetBaseTick();
    TickType_t now   = xTaskGetTickCount();

    /* If this fires, boot took longer than RELEASE_START_TICK: raise it. */
    configASSERT((int32_t)(start - now) > 0);

    vTaskDelayUntil(&now, start - now);     /* wakes exactly at 'start' */
    return start;
}

/* Motor control — critical task, D < T */
void MotorTaskBody(void *argument)
{
    uint32_t id = (uint32_t)argument;
    uint8_t speed = 40;
    uint32_t job = 0;

    TickType_t last_wake = WaitForFirstRelease();

    for (;;) {
        uint32_t release_cyc = Metrics_TickToCyc(last_wake);

        Motor_Dir_t dir = ((job++ / 200) & 1) ? MOTOR_REVERSE : MOTOR_FORWARD;
        Motor_Set(dir, speed);
        Workload_BurnCycles(scaled_wcet_cycles[id]);

        Metrics_RecordCompletion(id, release_cyc, DWT->CYCCNT);
        Scheduler_JobComplete(id);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(task_set[id].period_ms));
    }
}

/* Accelerometer read — real SPI transaction, D = T */
void AccelTaskBody(void *argument)
{
    uint32_t id = (uint32_t)argument;
    LIS3DSH_Data_t data;

    TickType_t last_wake = WaitForFirstRelease();

    for (;;) {
        uint32_t release_cyc = Metrics_TickToCyc(last_wake);

        LIS3DSH_ReadXYZ(&data);
        Workload_BurnCycles(scaled_wcet_cycles[id]);

        Metrics_RecordCompletion(id, release_cyc, DWT->CYCCNT);
        Scheduler_JobComplete(id);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(task_set[id].period_ms));
    }
}

/* Synthetic load — calibrated burn only */
void SyntheticTaskBody(void *argument)
{
    uint32_t id = (uint32_t)argument;

    TickType_t last_wake = WaitForFirstRelease();

    for (;;) {
        uint32_t release_cyc = Metrics_TickToCyc(last_wake);

        Workload_BurnCycles(scaled_wcet_cycles[id]);

        Metrics_RecordCompletion(id, release_cyc, DWT->CYCCNT);
        Scheduler_JobComplete(id);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(task_set[id].period_ms));
    }
}

/* Lowest-priority reporter — not part of the measured task set. */

void ReporterTaskBody(void *argument)
{
    (void)argument;

    vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);
    UART_Printf("reporter prio = %lu, window = %lu ms\r\n",
                (unsigned long)uxTaskPriorityGet(NULL),
                (unsigned long)MEASURE_WINDOW_MS);

    /* Wake at the common first release, start the clean window. */
    TickType_t t   = xTaskGetTickCount();
    TickType_t rel = Scheduler_GetBaseTick();
    configASSERT((int32_t)(rel - t) > 0);
    vTaskDelayUntil(&t, rel - t);
    idle_counter = 0;

    /* Sleep through the whole measurement window. */
    vTaskDelayUntil(&t, pdMS_TO_TICKS(MEASURE_WINDOW_MS));

    /* Freeze. Nothing else runs from here on; UART_Printf is plain
       blocking HAL, so it is safe with the scheduler suspended. */
    vTaskSuspendAll();
    uint32_t idle = idle_counter;

    UART_Printf("\r\n===== %s, %lu ms window =====", Scheduler_Name(),
                (unsigned long)MEASURE_WINDOW_MS);
    Metrics_Print();

    /* Jobs whose deadline fell inside the window but never finished
       are misses too; the completion-based counter cannot see them. */
    UART_Print("\r\n task      due   done  unfinished  total_miss\r\n");
    for (uint32_t i = 0; i < NUM_TASKS; i++) {
        uint32_t T = task_set[i].period_ms, D = task_set[i].deadline_ms;
        uint32_t due  = (MEASURE_WINDOW_MS >= D) ? (MEASURE_WINDOW_MS - D) / T + 1u : 0u;
        uint32_t done = metrics[i].jobs;
        uint32_t unf  = (due > done) ? due - done : 0u;
        UART_Printf(" %-8s %5lu %6lu %11lu %11lu\r\n", task_set[i].name,
                    (unsigned long)due, (unsigned long)done,
                    (unsigned long)unf, (unsigned long)(metrics[i].misses + unf));
    }

    UART_Printf("\r\nidle_counter = %lu\r\n", (unsigned long)idle);
    Scheduler_PrintOverhead();
    UART_Print("===== done, press reset to run again =====\r\n");

    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15, GPIO_PIN_SET);   /* blue LED = finished */
    for (;;) { }
}
