#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

void        Scheduler_Init(void);
const char *Scheduler_Name(void);
TickType_t  Scheduler_GetBaseTick(void);
void        Scheduler_PrintOverhead(void);
void Scheduler_JobComplete(uint32_t id);

#endif /* SCHEDULER_H */
