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
#include "sweep.h"
#include <stdio.h>

extern volatile uint32_t idle_counter;

/* ------------------------------------------------------------------
 * Every measured task blocks here until its first release:
 * RELEASE_START_TICK + task_offset_ms[id]. With all offsets zero (trial 0)
 * every task is released together (the critical instant). Releases line up
 * exactly with the ones the EDF tick hook computes, which uses the same
 * base tick and offsets.
 *
 * Returns the release tick of the first job; vTaskDelayUntil() then
 * advances it by exactly one period per job, even when a job overruns,
 * so last_wake is always the release tick of the job about to run.
 * ------------------------------------------------------------------ */
static TickType_t WaitForFirstRelease(uint32_t id)
{
    Metrics_TimebaseInit();

    TickType_t start = Scheduler_GetBaseTick() + pdMS_TO_TICKS(task_offset_ms[id]);
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

    TickType_t last_wake = WaitForFirstRelease(id);

    for (;;) {
        uint32_t release_cyc = Metrics_TickToCyc(last_wake);
        Metrics_RecordStart(id, release_cyc, DWT->CYCCNT);

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

    TickType_t last_wake = WaitForFirstRelease(id);

    for (;;) {
        uint32_t release_cyc = Metrics_TickToCyc(last_wake);
        Metrics_RecordStart(id, release_cyc, DWT->CYCCNT);

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

    TickType_t last_wake = WaitForFirstRelease(id);

    for (;;) {
        uint32_t release_cyc = Metrics_TickToCyc(last_wake);
        Metrics_RecordStart(id, release_cyc, DWT->CYCCNT);

        Workload_BurnCycles(scaled_wcet_cycles[id]);

        Metrics_RecordCompletion(id, release_cyc, DWT->CYCCNT);
        Scheduler_JobComplete(id);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(task_set[id].period_ms));
    }
}

/* ------------------------------------------------------------------
 * Reporter: one fixed measurement window per run.
 *
 * It runs at the HIGHEST priority, but only wakes twice: at the first
 * release (to zero idle_counter) and at the end of the window. At the
 * end it freezes the scheduler, so no measured task runs again, and
 * prints everything at leisure. Printing therefore never disturbs the
 * measurement, and it cannot be starved by overload, which is what
 * happened to the old priority-1 reporter at U=110%.
 *
 * After printing it hands over to the sweep driver, which prints the
 * CSV lines and resets into the next run (or halts at the end).
 *
 * Observer-effect variant (experiment 3, pt->obs = 1): the reporter ALSO
 * logs intrusively during the window - every OBS_LOG_PERIOD_MS it prints a
 * status line per task over the blocking UART, at its top priority. This
 * is the classic "printf status dump" and reproduces, under control, the
 * early reporter-priority bug. Bytes and records are counted and reported.
 * ------------------------------------------------------------------ */
#define MEASURE_WINDOW_MS  10000u
#define OBS_LOG_PERIOD_MS   1000u

/* One intrusive status dump: a line per task, like Metrics_Print. */
static uint32_t IntrusiveDump(uint32_t k, uint32_t *records)
{
    char     buf[96];
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < NUM_TASKS; i++) {
        int n = snprintf(buf, sizeof(buf), "LOG,%lu,%s,jobs=%lu,miss=%lu,max=%lu,last=%lu\r\n",
                         (unsigned long)k, task_set[i].name,
                         (unsigned long)metrics[i].jobs, (unsigned long)metrics[i].misses,
                         (unsigned long)metrics[i].max_resp_us,
                         (unsigned long)metrics[i].last_resp_us);
        if (n > 0) { UART_Print(buf); bytes += (uint32_t)n; (*records)++; }
    }
    return bytes;
}

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
    Metrics_CpuBegin();
    Scheduler_WindowBegin();

    const TickType_t end = t + pdMS_TO_TICKS(MEASURE_WINDOW_MS);
    uint32_t log_bytes = 0, log_records = 0;

    if (Sweep_Current()->obs) {
        /* intrusive logging during the window */
        for (uint32_t k = 1; k < MEASURE_WINDOW_MS / OBS_LOG_PERIOD_MS; k++) {
            vTaskDelayUntil(&t, pdMS_TO_TICKS(OBS_LOG_PERIOD_MS));
            log_bytes += IntrusiveDump(k, &log_records);
        }
    }

    /* Sleep until the end of the window (exactly base + MEASURE_WINDOW_MS). */
    vTaskDelayUntil(&t, end - t);

    /* Freeze. Nothing else runs from here on; UART_Printf is plain
       blocking HAL, so it is safe with the scheduler suspended. */
    vTaskSuspendAll();
    uint32_t idle = idle_counter;
    uint32_t cpu  = Metrics_CpuEnd();          /* hundredths of a percent */

    const SweepPoint_t *pt = Sweep_Current();
    UART_Printf("\r\n===== %s, set %s (%lu tasks), U=%lu.%lu%%, %lu ms window =====",
                Scheduler_Name(), task_set_name, (unsigned long)NUM_TASKS,
                (unsigned long)(pt->u_permille / 10), (unsigned long)(pt->u_permille % 10),
                (unsigned long)MEASURE_WINDOW_MS);
    Metrics_Print();

    /* Jobs whose deadline fell inside the window but which never finished
       are misses too; the completion-based counter cannot see them. */
    UART_Print("\r\n task      due   done  unfinished  total_miss\r\n");
    for (uint32_t i = 0; i < NUM_TASKS; i++) {
        uint32_t due  = TaskSet_Due(i, MEASURE_WINDOW_MS);
        uint32_t done = metrics[i].jobs;
        uint32_t unf  = (due > done) ? due - done : 0u;
        UART_Printf(" %-8s %5lu %6lu %11lu %11lu\r\n", task_set[i].name,
                    (unsigned long)due, (unsigned long)done,
                    (unsigned long)unf, (unsigned long)(metrics[i].misses + unf));
    }

    UART_Printf("\r\nCPU utilization = %lu.%02lu %%   (idle_counter = %lu)\r\n",
                (unsigned long)(cpu / 100), (unsigned long)(cpu % 100), (unsigned long)idle);
    Scheduler_PrintOverhead();

    if (pt->obs)
        UART_Printf("intrusive logging: %lu records, %lu bytes, every %lu ms at priority %lu\r\n",
                    (unsigned long)log_records, (unsigned long)log_bytes,
                    (unsigned long)OBS_LOG_PERIOD_MS, (unsigned long)uxTaskPriorityGet(NULL));
    Sweep_NoteLogging(log_bytes, log_records);

    Sweep_RunDone(MEASURE_WINDOW_MS, idle, cpu);   /* CSV lines, then next run; never returns */
}
