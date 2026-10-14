#ifndef SWEEP_H
#define SWEEP_H

#include <stdint.h>

/* =========================================================================
 * Automatic experiment sweep
 *
 * SWEEP_ENABLE 1: one flash runs every combination of the chosen
 *   experiment (lists in sweep.c). Each run is a fresh boot: after the
 *   measurement window the reporter prints the results and resets the MCU
 *   into the next run. The run index lives in CCM RAM, which the startup
 *   code never clears, so it survives the reset.
 *     - A new sweep starts automatically after flashing a build in which
 *       sweep.c was recompiled, or when the previous sweep finished.
 *     - Hold the blue USER button while pressing reset to restart from 0.
 *     - Blue LED (PD15) on = sweep complete.
 *
 * SWEEP_EXPERIMENT selects what is swept (run counts for SWEEP_TRIALS = 3):
 *   1 = task sets:   4 fixed 5-task sets x 21 U x DMS/EDF          (504 runs, ~95 min)
 *   2 = task count:  N = 5,10,20,35,50 x 8 U x DMS/EDF             (240 runs, ~50 min)
 *   3 = observer:    set A x U 60/80/90/100 x DMS/EDF x logging off/on
 *                                                                  ( 48 runs, ~10 min)
 *
 * SWEEP_TRIALS: independent trials per point. Trial 0 releases every task
 * together (critical instant, worst case). Trial t >= 1 uses random per-task
 * release offsets with seed t; DMS and EDF use the same seed, so they are
 * paired. Set to 1 for a quick single-trial sweep.
 *
 * SWEEP_ENABLE 0: a single manual run with the MANUAL_ values below.
 * ========================================================================= */
#define SWEEP_ENABLE        1
#define SWEEP_EXPERIMENT    2
#define SWEEP_TRIALS        3

/* Firmware identification printed in every run's META line. Bump it when
   you change the firmware, and/or paste the git commit you built from. */
#define FW_VERSION          "v0.5-trials"
#define FW_GIT_COMMIT       "unknown"

#define MANUAL_SCHED        SCHED_DMS
#define MANUAL_N            0          /* 0 = fixed set MANUAL_SET; 5..50 = generated set */
#define MANUAL_SET          0          /* index into task_sets[] when MANUAL_N == 0 */
#define MANUAL_U_PERMILLE   1100       /* 1100 = U of 110% */
#define MANUAL_TRIAL        0          /* 0 = synchronous release */
#define MANUAL_OBS          0          /* 1 = intrusive logging during the window */

typedef struct {
    uint32_t experiment;    /* 1, 2, 3, or 0 for a manual run */
    uint32_t run;           /* 0-based run number */
    uint32_t total;         /* runs in this sweep */
    uint32_t sched;         /* SCHED_DMS / SCHED_EDF */
    uint32_t set;           /* fixed task set index (experiments 1, 3) */
    uint32_t n_tasks;       /* task count of this run */
    uint32_t u_permille;    /* target utilization x1000 */
    uint32_t trial;         /* 0 .. SWEEP_TRIALS-1; also the offset seed */
    uint32_t obs;           /* 1 = intrusive logging variant */
    float    u;             /* target utilization as a fraction */
} SweepPoint_t;

/* Call in main() after GPIO/UART init and BEFORE MX_FREERTOS_Init().
   Selects the task set, offsets and scheduler for this run and returns the
   point; pass .u to Workload_SetTargetUtilization(). */
SweepPoint_t Sweep_Begin(void);

/* The current point (valid after Sweep_Begin). */
const SweepPoint_t *Sweep_Current(void);

/* Reporter: how much intrusive logging happened in the window. */
void Sweep_NoteLogging(uint32_t bytes, uint32_t records);

/* Called by the reporter with the scheduler suspended. Prints the
   machine-readable result lines, then resets into the next run, or halts
   when the sweep (or the manual run) is complete. Does not return.
   cpu_x100 = measured CPU utilization in hundredths of a percent. */
void Sweep_RunDone(uint32_t window_ms, uint32_t idle_count, uint32_t cpu_x100);

#endif /* SWEEP_H */
