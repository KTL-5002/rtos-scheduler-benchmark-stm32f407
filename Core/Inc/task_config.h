#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H

#include <stdint.h>

/* Scheduler identifiers. The scheduler is chosen at RUNTIME (see sweep.h
   and Scheduler_Select()), so both schedulers live in the same binary and
   the comparison runs on identical code. */
#define SCHED_DMS          1
#define SCHED_EDF          2

/* ------------------------------------------------------------------
 * Task count
 *
 * MAX_TASKS sizes every per-task array. The number of tasks actually
 * running is chosen at boot (task_count): 5 for the fixed task sets,
 * 5..50 for the task-count experiment. NUM_TASKS is the RUNTIME count,
 * so every "for (i = 0; i < NUM_TASKS; i++)" loop adapts automatically.
 * Never use NUM_TASKS as an array size; use MAX_TASKS.
 * ------------------------------------------------------------------ */
#define MAX_TASKS          50
#define FIXED_SET_TASKS    5
extern uint32_t task_count;
#define NUM_TASKS          (task_count)

/* Stack per task, in words. The real tasks (motor, accel) keep 512; the
   synthetic tasks only burn CPU and record metrics, so 256 is ample and
   lets 50 tasks fit in the FreeRTOS heap. */
#define TASK_STACK_WORDS        512
#define SYNTH_STACK_WORDS       256
#define TASK_STACK_FOR(id)      ((id) < 2u ? TASK_STACK_WORDS : SYNTH_STACK_WORDS)

/* Common first release for EVERY task (the critical instant), in ticks.
   1000 ticks = 100 ms at the 10 kHz tick: long enough for every task to
   be created and reach its first wait. Both schedulers report this via
   Scheduler_GetBaseTick(), and the EDF hook releases jobs from it, so the
   deadline table and the tasks' real releases stay in phase. */
#define RELEASE_START_TICK 1000u

/* Task identifiers — index into task_set[]. Same in every task set;
   indices 2 and up are synthetic. */
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

/* ---- Experiment 1: four fixed 5-task sets ---- */
#define NUM_TASK_SETS      4
extern const TaskSpec_t  task_sets[NUM_TASK_SETS][FIXED_SET_TASKS];
extern const char *const task_set_names[NUM_TASK_SETS];

/* ---- The ACTIVE task set (either a fixed set or a generated one) ----
   A pointer, so every existing task_set[i].field expression keeps working. */
extern const TaskSpec_t *task_set;
extern const char       *task_set_name;     /* for logs and CSV */

void TaskSet_Select(uint32_t id);           /* fixed set id; call before Scheduler_Init() */
void TaskSet_Generate(uint32_t n);          /* n tasks: motor, accel + (n-2) generated */

/* ------------------------------------------------------------------
 * Release offsets (for repeated, independent trials)
 *
 * Task i releases its first job at RELEASE_START_TICK + offset_ms[i] and
 * then every period. All zero = synchronous release (the critical instant,
 * the worst case for fixed priority). TaskSet_SetOffsets(seed) draws each
 * offset uniformly in [0, T_i) ms; the same seed gives the same offsets, so
 * DMS and EDF trials with equal seeds are exactly paired.
 * ------------------------------------------------------------------ */
extern uint16_t task_offset_ms[MAX_TASKS];
void     TaskSet_SetOffsets(uint32_t seed);      /* seed 0 = all offsets zero */

/* Jobs whose absolute deadline falls inside [base, base + window_ms]. */
uint32_t TaskSet_Due(uint32_t i, uint32_t window_ms);

#endif /* TASK_CONFIG_H */
