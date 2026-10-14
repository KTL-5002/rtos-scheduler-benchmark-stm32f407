#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include "task_config.h"
#include "FreeRTOS.h"

/* Response-time histogram: bins of R/D in percent. Bin k counts jobs with
   RESP_BIN_EDGES[k-1] <= R/D*100 < RESP_BIN_EDGES[k] (bin 0 from 0, the last
   bin open-ended). Everything at or above 100 is a deadline miss. */
#define RESP_BINS 16
extern const uint16_t RESP_BIN_EDGES[RESP_BINS - 1];

typedef struct {
    uint32_t jobs;            /* completed jobs */
    uint32_t misses;          /* deadline misses */
    uint32_t last_resp_us;    /* most recent response time */
    uint32_t max_resp_us;     /* worst observed */
    uint32_t min_resp_us;     /* best observed */
    uint64_t sum_resp_us;     /* for the mean response time */
    uint64_t sumsq_resp_us;   /* for the standard deviation (us^2) */

    /* Start latency: release -> first instruction of the job. This is what
       Zhang & Li (AICCONF 2025) call "response time"; recorded separately
       so both definitions can be reported. */
    uint32_t starts;
    uint32_t max_start_us;
    uint64_t sum_start_us;

    uint32_t hist[RESP_BINS]; /* response time / deadline, see RESP_BIN_EDGES */
} TaskMetrics_t;

extern volatile TaskMetrics_t metrics[MAX_TASKS];

void     Metrics_Init(void);
void     Metrics_RecordStart(uint32_t id, uint32_t release_cyc, uint32_t start_cyc);
void     Metrics_RecordCompletion(uint32_t id, uint32_t release_cyc, uint32_t done_cyc);

/* Mean / standard deviation helpers (us), for printing */
uint32_t Metrics_MeanResp(uint32_t id);
uint32_t Metrics_SdResp(uint32_t id);
uint32_t Metrics_MeanStart(uint32_t id);

/* CPU utilization over a window, from FreeRTOS run-time stats (idle task
   run time, counted in CPU cycles). Call Begin at the window start and End
   at the end; End returns utilization in hundredths of a percent. */
void     Metrics_CpuBegin(void);
uint32_t Metrics_CpuEnd(void);

/* Run-time stats clock for FreeRTOS (portGET_RUN_TIME_COUNTER_VALUE). */
uint32_t Metrics_RunTimeCounter(void);
void     Metrics_Print(void);

/* Tick <-> cycle timebase. Call Metrics_TimebaseInit() from task context
   (after the scheduler has started, so SysTick is running); it is safe to
   call from every task — the first caller sets the anchor. */
void     Metrics_TimebaseInit(void);
uint32_t Metrics_TickToCyc(TickType_t tick);

#endif /* METRICS_H */
