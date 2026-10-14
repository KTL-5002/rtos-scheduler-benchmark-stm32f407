/* =========================================================================
 * scheduler_edf.c — Earliest Deadline First for FreeRTOS on STM32F407
 *
 * Compiled in every build; selected at runtime via Scheduler_Select()
 * (see scheduler.c). Public entry points are EDF_*.
 *
 * Design rationale:
 *   Absolute deadlines change only at job release instants, so the 10 kHz
 *   tick hook does nothing but detect releases and update the deadline
 *   table. It never touches priorities: vTaskPrioritySet() has no FromISR
 *   variant and is not safe to call from a hook.
 *
 *   A dedicated EDF manager task, priority above every measured task, is
 *   notified from the hook, sorts the table, and writes new priorities.
 *   It runs once per release event (~every 10 ms here), not once per tick.
 *
 *   Priority mapping is EXACT: every task gets its own priority level
 *   (2 .. 1+N, up to N = 50 within configMAX_PRIORITIES = 56), earliest
 *   absolute deadline highest. No bucket approximation is involved.
 *
 *   Cost grows with N: the hook scans all N tasks every tick, and the
 *   manager's insertion sort is O(N^2) per release event. Measuring that
 *   growth is part of the task-count experiment.
 * ========================================================================= */

#include "task_config.h"

#include "scheduler.h"
#include "FreeRTOS.h"
#include "task.h"
#include "task_bodies.h"
#include "uart.h"
#include "stm32f4xx.h"

/* ---------------------------------------------------------------- config */

#define EDF_BASE_PRIORITY     2u
#define EDF_MANAGER_PRIORITY  (EDF_BASE_PRIORITY + MAX_TASKS)   /* = 52: above every task, below the reporter (55) */
#define MANAGER_STACK_WORDS   256u


/* ----------------------------------------------------------------- state */

static TaskHandle_t        edf_task[MAX_TASKS];
static TaskHandle_t        edf_manager = NULL;

static volatile TickType_t edf_next_release[MAX_TASKS];

/* Job accounting. A task has a pending job whenever released > completed.
 * The job it is working on is always the OLDEST pending one, whose
 * absolute deadline follows from how many jobs have completed:
 *     base + completed * T + D
 * This stays correct under overload, when a task is still finishing an
 * old job after its next release has already happened — the old
 * "overwrite the deadline at release, clear a flag at completion" scheme
 * gave a late job the NEXT job's deadline and marked a task with a
 * pending job as inactive. */
static volatile uint32_t   edf_released[MAX_TASKS];
static volatile uint32_t   edf_completed[MAX_TASKS];

static TickType_t          edf_period_ticks[MAX_TASKS];
static TickType_t          edf_deadline_ticks[MAX_TASKS];

#define EDF_BASE_TICK  ((TickType_t)RELEASE_START_TICK)

/* ------------------------------------------------------ overhead counters */

volatile uint32_t edf_hook_calls      = 0;
volatile uint32_t edf_hook_acc        = 0;
volatile uint32_t edf_hook_max        = 0;
volatile uint32_t edf_release_events  = 0;
volatile uint32_t edf_recompute_count = 0;
volatile uint32_t edf_recompute_acc   = 0;
volatile uint32_t edf_recompute_max   = 0;
volatile uint32_t edf_prio_writes     = 0;

/* Priority-update delay: from the moment an event is DETECTED (a release in
 * the tick hook, or a backlogged job found in EDF_JobComplete) to the moment
 * the manager has FINISHED writing the new priorities. Events that arrive
 * while one is already pending are folded into it, so each update is timed
 * from its oldest unserved event. This is elapsed time; the manager's own
 * CPU time comes from FreeRTOS run-time stats (EDF_ManagerCpuCycles). */
static volatile uint32_t edf_detect_cyc     = 0;
static volatile uint8_t  edf_detect_pending = 0;

volatile uint32_t edf_upd_n   = 0;
volatile uint64_t edf_upd_acc = 0;          /* cycles */
volatile uint32_t edf_upd_max = 0;          /* cycles */
volatile uint32_t edf_upd_hist[UPD_BINS];   /* see UPD_BIN_EDGES_US */
const uint16_t UPD_BIN_EDGES_US[UPD_BINS - 1] = { 2, 5, 10, 20, 50, 100, 200, 500, 1000 };

static uint32_t edf_mgr_rt0 = 0;            /* manager run time at window start */
static uint32_t edf_mgr_rt1 = 0;            /* manager run time at window end   */

/* Set when the measurement window closes. FreeRTOS keeps calling the tick
 * hook while the scheduler is suspended (the reporter prints with it
 * suspended), so without this the hook counters would keep growing during
 * printing. Once frozen the hook does nothing at all: no task can run any
 * more, so there is nothing left to schedule. */
static volatile uint8_t edf_frozen = 0;

/* Bring-up stages — see notes. Enable one at a time.
 *   1 = DWT bracket only (measure bare hook cost)
 *   2 = full hook + manager, but no priority writes
 *   3 = everything live                                                   */
#define EDF_STAGE  3

/* ===================================================================== */
/*  TICK HOOK — ISR context, 10 kHz. Keep short.                         */
/* ===================================================================== */

void EDF_TickHook(void)
{
    if (edf_frozen) return;                 /* window closed: counters are final */

    uint32_t t0 = DWT->CYCCNT;

#if EDF_STAGE >= 2
    TickType_t now      = xTaskGetTickCountFromISR();
    BaseType_t released = pdFALSE;

    for (uint32_t i = 0; i < NUM_TASKS; i++)
    {
        /* signed difference so TickType_t wraparound is handled */
        if ((int32_t)(now - edf_next_release[i]) >= 0)
        {
            edf_next_release[i] += edf_period_ticks[i];
            edf_released[i]++;
            released = pdTRUE;
        }
    }

    if (released && (edf_manager != NULL))
    {
        BaseType_t higher_woken = pdFALSE;
        if (!edf_detect_pending) { edf_detect_pending = 1; edf_detect_cyc = t0; }
        edf_release_events++;
        vTaskNotifyGiveFromISR(edf_manager, &higher_woken);
        portYIELD_FROM_ISR(higher_woken);
    }
#endif

    uint32_t d = DWT->CYCCNT - t0;
    edf_hook_calls++;
    edf_hook_acc += d;
    if (d > edf_hook_max) edf_hook_max = d;
}

/* ===================================================================== */
/*  EDF MANAGER TASK                                                     */
/* ===================================================================== */

#if EDF_STAGE >= 2
static void EDF_ManagerBody(void *argument)
{
    (void)argument;

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t   t0 = DWT->CYCCNT;
        TickType_t dl[MAX_TASKS];
        uint8_t    idx[MAX_TASKS];
        uint8_t    act[MAX_TASKS];

        /* snapshot only — the sort must not run with ticks masked */
        taskENTER_CRITICAL();
        uint8_t  had_event = edf_detect_pending;
        uint32_t det_cyc   = edf_detect_cyc;
        edf_detect_pending = 0;                 /* later events start a new update */
        for (uint32_t i = 0; i < NUM_TASKS; i++)
        {
            uint32_t rel = edf_released[i];
            uint32_t cmp = edf_completed[i];
            act[i] = (rel != cmp);
            dl[i]  = EDF_BASE_TICK + pdMS_TO_TICKS(task_offset_ms[i])
                                   + (TickType_t)cmp * edf_period_ticks[i]
                                   + edf_deadline_ticks[i];
            idx[i] = (uint8_t)i;
        }
        taskEXIT_CRITICAL();

        /* Insertion sort. Ordering rule, in priority order:
         *   1. active jobs before inactive ones
         *   2. among active jobs, earliest absolute deadline first
         *   3. inactive jobs keep index order (their deadline is stale)
         * Signed comparison so tick wraparound is handled. */
        for (uint32_t i = 1; i < NUM_TASKS; i++)
        {
            TickType_t key_dl  = dl[i];
            uint8_t    key_act = act[i];
            uint8_t    key_idx = idx[i];
            int32_t    j       = (int32_t)i - 1;

            while (j >= 0 &&
                   ( (act[j] < key_act) ||
                     (act[j] == key_act && key_act &&
                      (int32_t)(dl[j] - key_dl) > 0) ))
            {
                dl[j + 1]  = dl[j];
                act[j + 1] = act[j];
                idx[j + 1] = idx[j];
                j--;
            }
            dl[j + 1]  = key_dl;
            act[j + 1] = key_act;
            idx[j + 1] = key_idx;
        }

#if EDF_STAGE >= 3
        vTaskSuspendAll();
        for (uint32_t r = 0; r < NUM_TASKS; r++)
        {
            if (!act[r]) continue;          /* sleeper — leave its priority alone */
            UBaseType_t  want = EDF_BASE_PRIORITY + (NUM_TASKS - 1u - r);
            TaskHandle_t h    = edf_task[idx[r]];

            if (uxTaskPriorityGet(h) != want)
            {
                vTaskPrioritySet(h, want);
                edf_prio_writes++;
            }
        }
        xTaskResumeAll();
#endif

        uint32_t t1 = DWT->CYCCNT;
        uint32_t d  = t1 - t0;
        edf_recompute_count++;
        edf_recompute_acc += d;
        if (d > edf_recompute_max) edf_recompute_max = d;

        if (had_event)
        {
            uint32_t delay = t1 - det_cyc;
            uint32_t us    = delay / (SystemCoreClock / 1000000U);
            uint32_t b     = 0;
            while (b < UPD_BINS - 1 && us >= UPD_BIN_EDGES_US[b]) b++;
            edf_upd_hist[b]++;
            edf_upd_n++;
            edf_upd_acc += delay;
            if (delay > edf_upd_max) edf_upd_max = delay;
        }
    }
}
#endif

/* ===================================================================== */
/*  PUBLIC INTERFACE                                                     */
/* ===================================================================== */

/* Called by each task body when a job finishes (task context).
 * Normally the task then blocks until its next release, and nothing else
 * is needed. If its next job has ALREADY been released (overload), the
 * task runs straight on into it with a new, later deadline, so the
 * manager must re-sort now rather than wait for the next release event. */
void EDF_JobComplete(uint32_t id)
{
    if (id >= NUM_TASKS) return;

    edf_completed[id]++;

    if (edf_released[id] != edf_completed[id] && edf_manager != NULL)
    {
        taskENTER_CRITICAL();
        if (!edf_detect_pending) { edf_detect_pending = 1; edf_detect_cyc = DWT->CYCCNT; }
        taskEXIT_CRITICAL();
        xTaskNotifyGive(edf_manager);
    }
}

/* Manager task's total CPU cycles so far (FreeRTOS run-time stats, which
 * count only the time the manager itself was running, not interrupts or
 * other tasks that preempted it). */
static uint32_t EDF_ManagerRunTime(void)
{
    if (edf_manager == NULL) return 0;
    TaskStatus_t st;
    vTaskGetInfo(edf_manager, &st, pdFALSE, eRunning);
    return st.ulRunTimeCounter;
}

/* Called by the reporter at the start of the measurement window: every
 * overhead counter then covers exactly the window, nothing from boot. */
void EDF_WindowBegin(void)
{
    uint32_t rt = EDF_ManagerRunTime();
    taskENTER_CRITICAL();
    edf_hook_calls = edf_hook_acc = edf_hook_max = 0;
    edf_release_events = 0;
    edf_recompute_count = edf_recompute_acc = edf_recompute_max = 0;
    edf_prio_writes = 0;
    edf_upd_n = 0; edf_upd_acc = 0; edf_upd_max = 0;
    for (uint32_t b = 0; b < UPD_BINS; b++) edf_upd_hist[b] = 0;
    edf_mgr_rt0 = rt;
    taskEXIT_CRITICAL();
}

/* Called by the reporter right after it suspends the scheduler at the end
 * of the window: freezes every EDF counter before anything is printed. */
void EDF_WindowEnd(void)
{
    edf_frozen  = 1;
    edf_mgr_rt1 = EDF_ManagerRunTime();
}

uint32_t EDF_ManagerCpuCycles(void)
{
    return (edf_frozen ? edf_mgr_rt1 : EDF_ManagerRunTime()) - edf_mgr_rt0;
}
void EDF_Init(void)
{
    for (uint32_t i = 0; i < NUM_TASKS; i++)
    {
        edf_period_ticks[i]   = pdMS_TO_TICKS(task_set[i].period_ms);
        edf_deadline_ticks[i] = pdMS_TO_TICKS(task_set[i].deadline_ms);
        edf_next_release[i]   = EDF_BASE_TICK + pdMS_TO_TICKS(task_offset_ms[i]);
        edf_released[i]       = 0;
        edf_completed[i]      = 0;
    }

    /* Initial priorities = shortest relative deadline first. The manager
     * corrects this on the first tick; this only covers the gap before it
     * gets to run. */
    for (uint32_t i = 0; i < NUM_TASKS; i++)
    {
        UBaseType_t start_prio = EDF_BASE_PRIORITY + (NUM_TASKS - 1u - i);

        TaskFunction_t body;
        if      (i == TASK_MOTOR) body = MotorTaskBody;
        else if (i == TASK_ACCEL) body = AccelTaskBody;
        else                      body = SyntheticTaskBody;

        BaseType_t ok = xTaskCreate(body,
                                    task_set[i].name,
                                    TASK_STACK_FOR(i),
                                    (void *)(uint32_t)i,
                                    start_prio,
                                    &edf_task[i]);
        if (ok != pdPASS)
        {
            UART_Printf("EDF: create FAILED %s\r\n", task_set[i].name);
            configASSERT(0);
        }
    }

#if EDF_STAGE >= 2
    /* created last: the hook checks edf_manager for NULL, so the task
     * table is guaranteed populated before any notification can fire */
    BaseType_t ok = xTaskCreate(EDF_ManagerBody, "edfmgr",
                                MANAGER_STACK_WORDS, NULL,
                                EDF_MANAGER_PRIORITY, &edf_manager);
    configASSERT(ok == pdPASS);
    (void)ok;
#endif

    UART_Printf("--- EDF init (stage %d) ---\r\n", EDF_STAGE);
    for (uint32_t i = 0; i < NUM_TASKS; i++)
    {
        UART_Printf("  %-8s T=%4lu D=%4lu  Tt=%lu Dt=%lu\r\n",
                    task_set[i].name,
                    (unsigned long)task_set[i].period_ms,
                    (unsigned long)task_set[i].deadline_ms,
                    (unsigned long)edf_period_ticks[i],
                    (unsigned long)edf_deadline_ticks[i]);
    }
    UART_Printf("  mgr prio=%u  task prios=%u..%u\r\n",
                (unsigned)EDF_MANAGER_PRIORITY,
                (unsigned)EDF_BASE_PRIORITY,
                (unsigned)(EDF_BASE_PRIORITY + NUM_TASKS - 1u));
}

void EDF_PrintOverhead(void)
{
    uint32_t c = edf_hook_calls, r = edf_recompute_count;

    UART_Printf("--- EDF overhead (cycles, 168 = 1us) ---\r\n");
    UART_Printf("  hook     : n=%lu avg=%lu max=%lu\r\n",
                (unsigned long)c,
                (unsigned long)(c ? edf_hook_acc / c : 0),
                (unsigned long)edf_hook_max);
    UART_Printf("  recompute: n=%lu avg=%lu max=%lu\r\n",
                (unsigned long)r,
                (unsigned long)(r ? edf_recompute_acc / r : 0),
                (unsigned long)edf_recompute_max);
    UART_Printf("  releases=%lu prio_writes=%lu\r\n",
                (unsigned long)edf_release_events,
                (unsigned long)edf_prio_writes);
}

