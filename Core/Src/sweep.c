/* =========================================================================
 * sweep.c — automatic experiment sweep
 *
 * See sweep.h for how to use it. Each run is a fresh boot, so every run
 * starts from an identical state (fresh tasks, phases, metrics and EDF
 * tables) and runs on the same binary for both schedulers.
 *
 * Machine-readable output, one block per run (header lines first, so any
 * log file is self-describing):
 *   META,...   provenance: firmware, build, clocks, point, trial, seed
 *   CSV,...    one line per task: counts, response/start statistics, CPU
 *   HIST,...   one line per task: response-time histogram (R/D bins)
 *   OVH,...    scheduler overhead, manager CPU time, update delay
 *   UHIST,...  priority-update delay histogram (EDF)
 * analyze.py extracts these from the PuTTY log.
 * ========================================================================= */

#include "task_config.h"
#include "sweep.h"
#include "scheduler.h"
#include "metrics.h"
#include "uart.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "stm32f4xx_hal.h"

/* ============================================================ the matrices */

#if SWEEP_EXPERIMENT == 1
/* ---- Experiment 1: fixed task sets x utilization ---- */
static const uint8_t  SWEEP_OUTER[]  = { 0, 1, 2, 3 };          /* task set ids */
static const uint16_t SWEEP_U[] = {
     500,  550,  600,  650,  700,  750,
     800,  825,  850,  875,  900,  925,  950,  975, 1000,
    1050, 1100, 1150, 1200, 1250, 1300,
};
static const uint8_t  SWEEP_OBS[]    = { 0 };
#elif SWEEP_EXPERIMENT == 2
/* ---- Experiment 2: task count x utilization ----
   Analysis predicts first misses between ~88% and ~97% for these sets,
   so the grid is finer there. */
static const uint8_t  SWEEP_OUTER[]  = { 5, 10, 20, 35, 50 };   /* task counts N */
static const uint16_t SWEEP_U[]      = { 500, 700, 800, 850, 900, 950, 1000, 1100 };
static const uint8_t  SWEEP_OBS[]    = { 0 };
#elif SWEEP_EXPERIMENT == 3
/* ---- Experiment 3: observer effect, deferred vs intrusive logging ---- */
static const uint8_t  SWEEP_OUTER[]  = { 0 };                   /* set A */
static const uint16_t SWEEP_U[]      = { 600, 800, 900, 1000 };
static const uint8_t  SWEEP_OBS[]    = { 0, 1 };
#else
#error "SWEEP_EXPERIMENT must be 1, 2 or 3"
#endif

/* Schedulers, in the order they run for each point. */
static const uint8_t SWEEP_SCHEDS[] = { SCHED_DMS, SCHED_EDF };

#define COUNT(a)  (sizeof(a) / sizeof((a)[0]))
#define N_OUTER   COUNT(SWEEP_OUTER)
#define N_U       COUNT(SWEEP_U)
#define N_SCHEDS  COUNT(SWEEP_SCHEDS)
#define N_OBS     COUNT(SWEEP_OBS)
#define N_RUNS    (N_OUTER * N_U * N_SCHEDS * N_OBS * SWEEP_TRIALS)

/* ================================================== state surviving reset */

#define SWEEP_MAGIC  0x5357454Bu   /* "SWEK" */

typedef struct {
    uint32_t magic;
    uint32_t build;     /* hash of this file's build time + configuration */
    uint32_t index;     /* next run to execute */
} SweepState_t;

/* .ccmram is not touched by the startup code (no copy, no zeroing), so
   this survives NVIC_SystemReset(). After power-up it holds garbage,
   which the magic/build check rejects. */
static SweepState_t sweep_state __attribute__((section(".ccmram")));

static SweepPoint_t current;
static uint32_t     log_bytes_noted, log_records_noted;

static uint32_t BuildId(void)
{
    const char *s = __DATE__ " " __TIME__ " " FW_VERSION;
    uint32_t h = 2166136261u ^ (uint32_t)SWEEP_EXPERIMENT ^ ((uint32_t)SWEEP_TRIALS << 8);
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }                       /* FNV-1a */
    return h;
}

static void PrintU(uint32_t u_permille)
{
    UART_Printf("%lu.%lu%%", (unsigned long)(u_permille / 10),
                             (unsigned long)(u_permille % 10));
}

/* ================================================================= public */

SweepPoint_t Sweep_Begin(void)
{
#if SWEEP_ENABLE
    uint32_t build  = BuildId();
    int      button = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET);

    if (sweep_state.magic != SWEEP_MAGIC || sweep_state.build != build ||
        sweep_state.index >= N_RUNS || button)
    {
        sweep_state.magic = SWEEP_MAGIC;
        sweep_state.build = build;
        sweep_state.index = 0;
        UART_Printf("\r\n##### NEW SWEEP, experiment %lu: %lu runs "
                    "(%lu x %lu U x %lu sched x %lu logging x %lu trials) #####\r\n",
                    (unsigned long)SWEEP_EXPERIMENT, (unsigned long)N_RUNS,
                    (unsigned long)N_OUTER, (unsigned long)N_U, (unsigned long)N_SCHEDS,
                    (unsigned long)N_OBS, (unsigned long)SWEEP_TRIALS);
    }

    /* index -> (outer, U, sched, obs, trial); trial varies fastest, so the
       DMS and EDF trials of one point sit next to each other in the log */
    uint32_t k = sweep_state.index;
    current.run   = k;
    current.total = N_RUNS;
    current.trial = k % SWEEP_TRIALS;           k /= SWEEP_TRIALS;
    current.obs   = SWEEP_OBS[k % N_OBS];       k /= N_OBS;
    current.sched = SWEEP_SCHEDS[k % N_SCHEDS]; k /= N_SCHEDS;
    current.u_permille = SWEEP_U[k % N_U];      k /= N_U;
    uint32_t outer = SWEEP_OUTER[k];
    current.experiment = SWEEP_EXPERIMENT;

  #if SWEEP_EXPERIMENT == 2
    current.set = 0;
    TaskSet_Generate(outer);
  #else
    current.set = outer;
    TaskSet_Select(outer);
  #endif
#else
    current.experiment = 0;
    current.run        = 0;
    current.total      = 1;
    current.sched      = MANUAL_SCHED;
    current.u_permille = MANUAL_U_PERMILLE;
    current.set        = MANUAL_SET;
    current.trial      = MANUAL_TRIAL;
    current.obs        = MANUAL_OBS;
    if (MANUAL_N == 0) TaskSet_Select(MANUAL_SET);
    else               TaskSet_Generate(MANUAL_N);
#endif
    current.n_tasks = NUM_TASKS;
    current.u       = (float)current.u_permille / 1000.0f;

    TaskSet_SetOffsets(current.trial);          /* trial 0: synchronous release */
    Scheduler_Select(current.sched);

    UART_Printf("\r\n##### RUN %lu/%lu: %s, set %s, %lu tasks, U=",
                (unsigned long)(current.run + 1), (unsigned long)current.total,
                Scheduler_Name(), task_set_name, (unsigned long)current.n_tasks);
    PrintU(current.u_permille);
    UART_Printf(", trial %lu%s%s #####\r\n", (unsigned long)current.trial,
                current.trial ? " (random offsets)" : " (synchronous)",
                current.obs ? ", INTRUSIVE LOGGING" : "");

    return current;
}

const SweepPoint_t *Sweep_Current(void)
{
    return &current;
}

void Sweep_NoteLogging(uint32_t bytes, uint32_t records)
{
    log_bytes_noted   = bytes;
    log_records_noted = records;
}

void Sweep_RunDone(uint32_t window_ms, uint32_t idle_count, uint32_t cpu_x100)
{
    const char *sch = Scheduler_Name();
    const char *set = task_set_name;
    const unsigned long run = current.run, upm = current.u_permille;
    const unsigned long tr = current.trial, obs = current.obs;

    /* ---- provenance ---- */
    UART_Print("METAH,run,sched,set,u_permille,trial,obs,experiment,n_tasks,fw_version,git_commit,"
               "build_id,build_date,build_time,sysclk_hz,tick_hz,cyc_per_tick,window_ms,"
               "trials,offset_seed,log_bytes,log_records\r\n");
    UART_Printf("META,%lu,%s,%s,%lu,%lu,%lu,%lu,%lu,%s,%s,%08lx,%s,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
                run, sch, set, upm, tr, obs,
                (unsigned long)current.experiment, (unsigned long)NUM_TASKS,
                FW_VERSION, FW_GIT_COMMIT, (unsigned long)BuildId(), __DATE__, __TIME__,
                (unsigned long)SystemCoreClock, (unsigned long)configTICK_RATE_HZ,
                (unsigned long)(SysTick->LOAD + 1u), (unsigned long)window_ms,
                (unsigned long)SWEEP_TRIALS, tr,
                (unsigned long)log_bytes_noted, (unsigned long)log_records_noted);

    /* ---- per-task results ---- */
    UART_Print("CSVH,run,sched,set,u_permille,task,T_ms,D_ms,jobs,misses,"
               "max_us,min_us,due,done,unfinished,total_miss,idle,window_ms,"
               "experiment,n_tasks,mean_us,sd_us,start_mean_us,start_max_us,starts,cpu_x100,"
               "trial,obs,offset_ms\r\n");
    for (uint32_t i = 0; i < NUM_TASKS; i++) {
        uint32_t due  = TaskSet_Due(i, window_ms);
        uint32_t done = metrics[i].jobs;
        uint32_t unf  = (due > done) ? due - done : 0u;
        uint32_t mn   = (metrics[i].min_resp_us == 0xFFFFFFFFu) ? 0u : metrics[i].min_resp_us;

        UART_Printf("CSV,%lu,%s,%s,%lu,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                    "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
                    run, sch, set, upm, task_set[i].name,
                    (unsigned long)task_set[i].period_ms, (unsigned long)task_set[i].deadline_ms,
                    (unsigned long)metrics[i].jobs, (unsigned long)metrics[i].misses,
                    (unsigned long)metrics[i].max_resp_us, (unsigned long)mn,
                    (unsigned long)due, (unsigned long)done, (unsigned long)unf,
                    (unsigned long)(metrics[i].misses + unf),
                    (unsigned long)idle_count, (unsigned long)window_ms,
                    (unsigned long)current.experiment, (unsigned long)NUM_TASKS,
                    (unsigned long)Metrics_MeanResp(i), (unsigned long)Metrics_SdResp(i),
                    (unsigned long)Metrics_MeanStart(i), (unsigned long)metrics[i].max_start_us,
                    (unsigned long)metrics[i].starts, (unsigned long)cpu_x100,
                    tr, obs, (unsigned long)task_offset_ms[i]);
    }

    /* ---- per-task response-time histograms (R/D, percent bin edges) ---- */
    UART_Print("HISTH,run,sched,set,u_permille,trial,obs,task,D_ms");
    for (uint32_t b = 0; b < RESP_BINS; b++) {
        if (b == RESP_BINS - 1) UART_Printf(",ge%u", (unsigned)RESP_BIN_EDGES[b - 1]);
        else                    UART_Printf(",lt%u", (unsigned)RESP_BIN_EDGES[b]);
    }
    UART_Print("\r\n");
    for (uint32_t i = 0; i < NUM_TASKS; i++) {
        UART_Printf("HIST,%lu,%s,%s,%lu,%lu,%lu,%s,%lu", run, sch, set, upm, tr, obs,
                    task_set[i].name, (unsigned long)task_set[i].deadline_ms);
        for (uint32_t b = 0; b < RESP_BINS; b++)
            UART_Printf(",%lu", (unsigned long)metrics[i].hist[b]);
        UART_Print("\r\n");
    }

    /* ---- scheduler overhead, manager CPU time, priority-update delay ---- */
    int edf = (Scheduler_Active() == SCHED_EDF);
    uint32_t hc = edf ? edf_hook_calls : 0, rc = edf ? edf_recompute_count : 0;
    uint32_t un = edf ? edf_upd_n : 0;
    UART_Print("OVHH,run,sched,set,u_permille,hook_n,hook_avg_cyc,hook_max_cyc,"
               "recomp_n,recomp_avg_cyc,recomp_max_cyc,releases,prio_writes,n_tasks,"
               "trial,obs,mgr_cpu_cyc,upd_n,upd_avg_cyc,upd_max_cyc\r\n");
    UART_Printf("OVH,%lu,%s,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
                run, sch, set, upm,
                (unsigned long)hc,
                (unsigned long)(hc ? edf_hook_acc / hc : 0),
                (unsigned long)(edf ? edf_hook_max : 0),
                (unsigned long)rc,
                (unsigned long)(rc ? edf_recompute_acc / rc : 0),
                (unsigned long)(edf ? edf_recompute_max : 0),
                (unsigned long)(edf ? edf_release_events : 0),
                (unsigned long)(edf ? edf_prio_writes : 0),
                (unsigned long)NUM_TASKS, tr, obs,
                (unsigned long)(edf ? EDF_ManagerCpuCycles() : 0),
                (unsigned long)un,
                (unsigned long)(un ? (uint32_t)(edf_upd_acc / un) : 0),
                (unsigned long)(edf ? edf_upd_max : 0));

    UART_Print("UHISTH,run,sched,set,u_permille,trial,obs");
    for (uint32_t b = 0; b < UPD_BINS; b++) {
        if (b == UPD_BINS - 1) UART_Printf(",ge%uus", (unsigned)UPD_BIN_EDGES_US[b - 1]);
        else                   UART_Printf(",lt%uus", (unsigned)UPD_BIN_EDGES_US[b]);
    }
    UART_Print("\r\n");
    UART_Printf("UHIST,%lu,%s,%s,%lu,%lu,%lu", run, sch, set, upm, tr, obs);
    for (uint32_t b = 0; b < UPD_BINS; b++)
        UART_Printf(",%lu", (unsigned long)(edf ? edf_upd_hist[b] : 0));
    UART_Print("\r\n");

#if SWEEP_ENABLE
    sweep_state.index = current.run + 1;

    if (sweep_state.index < N_RUNS) {
        UART_Print("##### next run #####\r\n");
        /* let the last byte leave the UART before resetting */
        while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC) == RESET) { }
        NVIC_SystemReset();
    }

    sweep_state.magic = 0;      /* finished: the next reset starts a new sweep */
    UART_Printf("\r\n##### SWEEP COMPLETE: %lu runs. Stop the PuTTY log now. #####\r\n",
                (unsigned long)N_RUNS);
#else
    UART_Print("===== done, press reset to run again =====\r\n");
#endif

    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15, GPIO_PIN_SET);   /* blue LED = finished */
    for (;;) { }
}
