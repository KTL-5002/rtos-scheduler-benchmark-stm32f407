#include "task_config.h"
#include <stdio.h>
#include <math.h>

/* ==================================================================
 * Experiment 1 — four fixed 5-task sets
 *
 * Motor and accel are the real tasks and are the same everywhere. The
 * three synthetic tasks define each set; their nominal WCETs set how the
 * synthetic load is split, and Workload_SetTargetUtilization() scales
 * them to reach the target U.
 *
 * Predicted first-miss utilization (response-time analysis for DMS,
 * processor-demand analysis for EDF incl. ~1.9% EDF overhead):
 *
 *   A  harmonic, D<T          DMS ~87.5%   EDF ~97%   large EDF advantage
 *   B  non-harmonic, D=T      DMS ~94.5%   EDF ~97%   classic textbook case
 *   C  non-harmonic, tight D  DMS ~83%     EDF ~88%   both fail early
 *   D  harmonic, D=T          DMS ~99%     EDF ~97%   control: FP optimal,
 *                                                     EDF loses to overhead
 * ================================================================== */

#define MOTOR_SPEC  { "motor",    10,   8,   2000,   1,    0 }
#define ACCEL_SPEC  { "accel",    20,  20,   1500,   0,    0 }

const TaskSpec_t task_sets[NUM_TASK_SETS][FIXED_SET_TASKS] = {
    /* A: harmonic periods, constrained deadlines (the original set) */
    {   MOTOR_SPEC, ACCEL_SPEC,
        /* name       T    D    C(us)  crit  synth */
        { "synth1",   50,  40,   5000,   0,    1 },
        { "synth2",  100, 100,   8000,   0,    1 },
        { "synth3",  200, 150,  10000,   0,    1 },
    },
    /* B: non-harmonic periods, implicit deadlines (D = T) */
    {   MOTOR_SPEC, ACCEL_SPEC,
        { "synth1",   30,  30,   5000,   0,    1 },
        { "synth2",   70,  70,   8000,   0,    1 },
        { "synth3",  130, 130,  10000,   0,    1 },
    },
    /* C: non-harmonic periods, tight deadlines (D ~ 0.55-0.6 T) */
    {   MOTOR_SPEC, ACCEL_SPEC,
        { "synth1",   30,  18,   5000,   0,    1 },
        { "synth2",   70,  40,   8000,   0,    1 },
        { "synth3",  130,  70,  10000,   0,    1 },
    },
    /* D: harmonic periods, implicit deadlines (control set) */
    {   MOTOR_SPEC, ACCEL_SPEC,
        { "synth1",   50,  50,   5000,   0,    1 },
        { "synth2",  100, 100,   8000,   0,    1 },
        { "synth3",  200, 200,  10000,   0,    1 },
    },
};

const char *const task_set_names[NUM_TASK_SETS] = {
    "A_harmonic_constrD",
    "B_nonharmonic_implD",
    "C_nonharmonic_tightD",
    "D_harmonic_implD",
};

/* ==================================================================
 * Active set — always copied into RAM so fixed and generated sets are
 * handled identically.
 * ================================================================== */
/* Starts out holding set A, so anything that reads the task set before
   Sweep_Begin() picks this run's set (e.g. boot prints) sees valid data. */
static TaskSpec_t task_buf[MAX_TASKS] = {
    MOTOR_SPEC, ACCEL_SPEC,
    { "synth1",   50,  40,   5000,   0,    1 },
    { "synth2",  100, 100,   8000,   0,    1 },
    { "synth3",  200, 150,  10000,   0,    1 },
};
static char       gen_names[MAX_TASKS][8];
static char       gen_set_name[16];

const TaskSpec_t *task_set      = task_buf;
const char       *task_set_name = "A_harmonic_constrD";
uint32_t          task_count    = FIXED_SET_TASKS;

void TaskSet_Select(uint32_t id)
{
    if (id >= NUM_TASK_SETS) id = 0;
    for (uint32_t i = 0; i < FIXED_SET_TASKS; i++) task_buf[i] = task_sets[id][i];
    task_count    = FIXED_SET_TASKS;
    task_set_name = task_set_names[id];
}

/* ==================================================================
 * Experiment 2 — generated task sets with n tasks
 *
 * Tasks 0 and 1 are always motor and accel. Tasks 2..n-1 are synthetic:
 *   period    drawn from {10,20,25,40,50,80,100,125,200} ms
 *             (hyperperiod 2 s, so a 10 s window covers it 5 times)
 *   deadline  uniform in [0.6 T, T]  (constrained, so DMS != RMS)
 *   load      split with UUniFast (Bini & Buttazzo, 2005), the standard
 *             unbiased way to divide a utilization among n tasks.
 *
 * The nominal synthetic utilizations sum to 1; Workload_SetTargetUtilization
 * then scales them so the WHOLE set (real + synthetic) hits the target U.
 *
 * The generator is seeded from n only, so a given n always produces the
 * same task set: every U level and both schedulers see identical tasks.
 * ================================================================== */
static uint32_t rng_state;

static uint32_t Rng(void)                     /* xorshift32 */
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static float RngUnit(void)                    /* uniform in (0, 1) */
{
    return ((float)(Rng() >> 8) + 0.5f) / 16777216.0f;
}

void TaskSet_Generate(uint32_t n)
{
    static const uint16_t PERIODS[] = { 10, 20, 25, 40, 50, 80, 100, 125, 200 };
    const uint32_t NP = sizeof(PERIODS) / sizeof(PERIODS[0]);

    if (n < 3)         n = 3;
    if (n > MAX_TASKS) n = MAX_TASKS;

    rng_state = 0x9E3779B9u ^ (n * 2654435761u);
    if (rng_state == 0) rng_state = 1;

    task_buf[0] = (TaskSpec_t)MOTOR_SPEC;
    task_buf[1] = (TaskSpec_t)ACCEL_SPEC;

    /* UUniFast over the m synthetic tasks, total 1.0 */
    uint32_t m   = n - 2;
    float    sum = 1.0f;
    for (uint32_t k = 0; k < m; k++) {
        float u;
        if (k < m - 1) {
            float next = sum * powf(RngUnit(), 1.0f / (float)(m - 1 - k));
            u   = sum - next;
            sum = next;
        } else {
            u = sum;
        }

        uint32_t i = k + 2;
        uint32_t T = PERIODS[Rng() % NP];
        uint32_t D = (uint32_t)((float)T * (0.6f + 0.4f * RngUnit()));
        if (D < 1) D = 1;
        if (D > T) D = T;

        /* nominal WCET in us; synthetic utilizations sum to ~1.0 */
        uint32_t C = (uint32_t)(u * (float)T * 1000.0f + 0.5f);

        snprintf(gen_names[i], sizeof(gen_names[i]), "t%02lu", (unsigned long)i);
        task_buf[i].name         = gen_names[i];
        task_buf[i].period_ms    = T;
        task_buf[i].deadline_ms  = D;
        task_buf[i].wcet_us      = C;
        task_buf[i].is_critical  = 0;
        task_buf[i].is_synthetic = 1;
    }

    task_count = n;
    snprintf(gen_set_name, sizeof(gen_set_name), "GEN_N%02lu", (unsigned long)n);
    task_set_name = gen_set_name;
}

/* ==================================================================
 * Release offsets
 * ================================================================== */
uint16_t task_offset_ms[MAX_TASKS];

void TaskSet_SetOffsets(uint32_t seed)
{
    for (uint32_t i = 0; i < MAX_TASKS; i++) task_offset_ms[i] = 0;
    if (seed == 0) return;                       /* synchronous release */

    rng_state = 0x85EBCA6Bu ^ (seed * 2246822519u);
    if (rng_state == 0) rng_state = 1;
    for (uint32_t i = 0; i < NUM_TASKS; i++)
        task_offset_ms[i] = (uint16_t)(Rng() % task_set[i].period_ms);
}

uint32_t TaskSet_Due(uint32_t i, uint32_t window_ms)
{
    uint32_t T = task_set[i].period_ms, D = task_set[i].deadline_ms;
    uint32_t first = task_offset_ms[i] + D;      /* deadline of job 0, ms after base */
    return (window_ms >= first) ? (window_ms - first) / T + 1u : 0u;
}
