#include "task_config.h"

const TaskSpec_t task_set[NUM_TASKS] = {
    /* name       T    D    C(us)  crit  synth */
    { "motor",    10,   8,   2000,   1,    0 },
    { "accel",    20,  20,   1500,   0,    0 },
    { "synth1",   50,  40,   5000,   0,    1 },
    { "synth2",  100, 100,   8000,   0,    1 },
    { "synth3",  200, 150,  10000,   0,    1 },
};
