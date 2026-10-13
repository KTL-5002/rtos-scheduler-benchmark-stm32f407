#ifndef WORKLOAD_H
#define WORKLOAD_H
#define CYCLES_PER_TICK   (168000000U / 10000U)   /* = 16800 */

#include <stdint.h>
#include "task_config.h"

extern uint32_t scaled_wcet_cycles[NUM_TASKS];

void     Workload_BurnCycles(uint32_t cycles);
void     Workload_BurnUs(uint32_t us);
void     Workload_SetTargetUtilization(float target_u);
float    Workload_GetRealUtilization(void);
uint32_t Workload_MeasureBurn(uint32_t requested_us);
void     Workload_Calibrate(void);
uint32_t Workload_GetCyclesPerIterQ16(void);

#endif /* WORKLOAD_H */
