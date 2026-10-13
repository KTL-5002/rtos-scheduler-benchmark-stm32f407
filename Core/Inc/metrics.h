#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include "task_config.h"
#include "FreeRTOS.h"

typedef struct {
    uint32_t jobs;
    uint32_t misses;
    uint32_t last_resp_us;
    uint32_t max_resp_us;
    uint32_t min_resp_us;
} TaskMetrics_t;

extern volatile TaskMetrics_t metrics[NUM_TASKS];

void     Metrics_Init(void);
void     Metrics_RecordCompletion(uint32_t id, uint32_t release_cyc, uint32_t done_cyc);
void     Metrics_Print(void);

/* Call Metrics_TimebaseInit() from task context (scheduler running).
   Safe to call from every task — the first caller sets the anchor. */
void     Metrics_TimebaseInit(void);
uint32_t Metrics_TickToCyc(TickType_t tick);

#endif /* METRICS_H */
