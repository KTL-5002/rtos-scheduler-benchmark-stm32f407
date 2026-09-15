#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include "task_config.h"

typedef struct {
    uint32_t jobs;            /* completed jobs */
    uint32_t misses;          /* deadline misses */
    uint32_t last_resp_us;    /* most recent response time */
    uint32_t max_resp_us;     /* worst observed */
    uint32_t min_resp_us;     /* best observed */
} TaskMetrics_t;

extern volatile TaskMetrics_t metrics[NUM_TASKS];

void Metrics_Init(void);
void Metrics_RecordCompletion(uint32_t id, uint32_t release_cyc, uint32_t done_cyc);
void Metrics_Print(void);

#endif /* METRICS_H */
