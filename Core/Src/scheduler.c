/* =========================================================================
 * scheduler.c — runtime choice between DMS and EDF
 *
 * Both schedulers are compiled into one binary and the active one is
 * picked at boot (by the sweep driver). The comparison therefore runs on
 * byte-identical code, task bodies and memory layout.
 *
 * Cost of the dispatch for DMS: one compare-and-branch per tick and per
 * job completion, a few cycles per 16800-cycle tick (~0.03% CPU).
 * ========================================================================= */

#include "scheduler.h"
#include "uart.h"

static volatile uint32_t active_sched = SCHED_DMS;

void Scheduler_Select(uint32_t sched)
{
    active_sched = (sched == SCHED_EDF) ? SCHED_EDF : SCHED_DMS;
}

uint32_t Scheduler_Active(void)
{
    return active_sched;
}

void Scheduler_Init(void)
{
    if (active_sched == SCHED_EDF) EDF_Init();
    else                           DMS_Init();
}

const char *Scheduler_Name(void)
{
    return (active_sched == SCHED_EDF) ? "EDF" : "DMS";
}

/* Both schedulers release the first job of every task at the same tick. */
TickType_t Scheduler_GetBaseTick(void)
{
    return (TickType_t)RELEASE_START_TICK;
}

void Scheduler_PrintOverhead(void)
{
    if (active_sched == SCHED_EDF) EDF_PrintOverhead();
    else                           DMS_PrintOverhead();
}

void Scheduler_JobComplete(uint32_t id)
{
    if (active_sched == SCHED_EDF) EDF_JobComplete(id);
}

void Scheduler_WindowBegin(void)
{
    if (active_sched == SCHED_EDF) EDF_WindowBegin();
}

/* FreeRTOS tick hook (configUSE_TICK_HOOK = 1). Only EDF does work here. */
void vApplicationTickHook(void)
{
    if (active_sched == SCHED_EDF) EDF_TickHook();
}
