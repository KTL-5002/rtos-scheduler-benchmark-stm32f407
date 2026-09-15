#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H

#include <stdint.h>

/* Scheduler selection */
#define SCHED_DMS          1
#define SCHED_EDF          2
#define ACTIVE_SCHEDULER   SCHED_DMS

#define NUM_TASKS          5
#define TASK_STACK_WORDS   512     /* words, for xTaskCreate */

/* Task identifiers — index into task_set[] */
#define TASK_MOTOR         0
#define TASK_ACCEL         1
#define TASK_SYNTH1        2
#define TASK_SYNTH2        3
#define TASK_SYNTH3        4

typedef struct {
    const char *name;
    uint32_t    period_ms;
    uint32_t    deadline_ms;
    uint32_t    wcet_us;        /* nominal; synthetics scaled at runtime */
    uint8_t     is_critical;
    uint8_t     is_synthetic;
} TaskSpec_t;

extern const TaskSpec_t task_set[NUM_TASKS];

#endif /* TASK_CONFIG_H */
