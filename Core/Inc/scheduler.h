#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "task_config.h"

/* ---- generic interface (scheduler.c dispatches to DMS or EDF) ---- */
void        Scheduler_Select(uint32_t sched);   /* SCHED_DMS / SCHED_EDF, before Init */
uint32_t    Scheduler_Active(void);
void        Scheduler_Init(void);
const char *Scheduler_Name(void);
TickType_t  Scheduler_GetBaseTick(void);
void        Scheduler_PrintOverhead(void);
void        Scheduler_JobComplete(uint32_t id);
void        Scheduler_WindowBegin(void);        /* reset overhead counters at window start */

/* ---- implementations ---- */
void DMS_Init(void);
void DMS_PrintOverhead(void);

void EDF_Init(void);
void EDF_PrintOverhead(void);
void EDF_JobComplete(uint32_t id);
void EDF_TickHook(void);
void EDF_WindowBegin(void);
uint32_t EDF_ManagerCpuCycles(void);            /* manager CPU time in the window */

/* EDF overhead counters (defined in scheduler_edf.c) */
extern volatile uint32_t edf_hook_calls, edf_hook_acc, edf_hook_max;
extern volatile uint32_t edf_release_events;
extern volatile uint32_t edf_recompute_count, edf_recompute_acc, edf_recompute_max;
extern volatile uint32_t edf_prio_writes;

/* Priority-update delay (detection -> priorities written), in the window */
#define UPD_BINS 10
extern const uint16_t     UPD_BIN_EDGES_US[UPD_BINS - 1];
extern volatile uint32_t  edf_upd_n, edf_upd_max;
extern volatile uint64_t  edf_upd_acc;
extern volatile uint32_t  edf_upd_hist[UPD_BINS];

#endif /* SCHEDULER_H */
