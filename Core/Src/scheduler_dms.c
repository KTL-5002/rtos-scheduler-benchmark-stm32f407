#include "task_config.h"

#if ACTIVE_SCHEDULER == SCHED_DMS

#include "scheduler.h"
#include "task_bodies.h"
#include "uart.h"
#include "FreeRTOS.h"
#include "task.h"

static TaskHandle_t task_handles[NUM_TASKS];

/* Lowest priority given to a real task. The reporter sits below this. */
#define DMS_BASE_PRIORITY   2

void Scheduler_Init(void)
{
	UART_Printf("\r\nscheduler=%s  built %s %s\r\n", Scheduler_Name(), __DATE__, __TIME__);
    uint8_t order[NUM_TASKS];

    for (uint8_t i = 0; i < NUM_TASKS; i++) order[i] = i;

    /* Insertion sort by relative deadline, ascending.
       Five elements — no need for qsort. */
    for (uint8_t i = 1; i < NUM_TASKS; i++) {
        uint8_t key = order[i];
        int8_t  j   = i - 1;
        while (j >= 0 && task_set[order[j]].deadline_ms > task_set[key].deadline_ms) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    UART_Print("\r\n--- DMS priority assignment ---\r\n");

    /* Shortest deadline gets the highest priority */
    UBaseType_t prio = DMS_BASE_PRIORITY + NUM_TASKS - 1;

    for (uint8_t i = 0; i < NUM_TASKS; i++) {
        uint8_t id = order[i];
        TaskFunction_t body;

        if      (id == TASK_MOTOR) body = MotorTaskBody;
        else if (id == TASK_ACCEL) body = AccelTaskBody;
        else                       body = SyntheticTaskBody;

        BaseType_t ok = xTaskCreate(
            body,
            task_set[id].name,
            TASK_STACK_WORDS,
            (void *)(uint32_t)id,
            prio,
            &task_handles[id]
        );

        UART_Printf("  %-8s T=%3lu D=%3lu prio=%2lu %s\r\n",
                    task_set[id].name,
                    (unsigned long)task_set[id].period_ms,
                    (unsigned long)task_set[id].deadline_ms,
                    (unsigned long)prio,
                    ok == pdPASS ? "OK" : "CREATE FAILED");

        prio--;
    }
    UART_Print("-------------------------------\r\n\r\n");
}
void        vApplicationTickHook(void)  { }
TickType_t  Scheduler_GetBaseTick(void) { return 0; }
const char *Scheduler_Name(void)        { return "DMS"; }
void        Scheduler_PrintOverhead(void) { UART_Printf("--- DMS: no scheduler overhead ---\r\n"); }
void Scheduler_JobComplete(uint32_t id) { (void)id; }

#endif /* ACTIVE_SCHEDULER == SCHED_DMS */
