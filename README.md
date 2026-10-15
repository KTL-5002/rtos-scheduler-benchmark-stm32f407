# DMS vs EDF: A Hardware-Validated Scheduler Benchmark on STM32F407

A comparative study of **Deadline Monotonic Scheduling (DMS)** and **Earliest Deadline First (EDF)** implemented on real silicon — an STM32F407 Discovery board running FreeRTOS — under identical, controllable workloads.

**Status:** Measurement complete (firmware v0.5.1, 829 runs, 15 October 2026) · Report in progress

---

## Key results

| Finding | Evidence |
|---|---|
| With few tasks, EDF matches or beats DMS — most clearly on constrained deadlines | 5 tasks, set A (D < T): first miss DMS 87.5 %, EDF 95 % |
| As tasks grow, EDF's run-time overhead outweighs its advantage | 50 tasks: first miss DMS 90 %, EDF 70 % |
| EDF's overhead grows faster than linearly with task count | At U = 50 %: 2.0 % CPU at 5 tasks → 18.4 % at 50 tasks |
| DMS fails predictably under overload and protects the critical task; EDF fails everywhere | U = 110 %: motor misses DMS 0 %, EDF ~97 % |
| EDF's collapse is deterministic, not random | 10 / 10 identical repeats, adherence 3.58 % (50 tasks, U = 70 %), SD 0 |
| Printing during measurement destroys the timing | Motor worst-case response 2.05 ms → ~20 ms with logging on |

First deadline miss vs task count (lowest tested U with any miss):

| Tasks | 5 | 10 | 20 | 35 | 50 |
|---|---:|---:|---:|---:|---:|
| DMS | 95 % | 90 % | 90 % | 90 % | 90 % |
| EDF | 95 % | 95 % | 90 % | 80 % | 70 % |

The conclusion is about **this EDF implementation on FreeRTOS**, not the EDF policy: FreeRTOS only schedules by fixed priority, so EDF must be emulated by re-sorting and rewriting priorities at run time, and that work competes with the tasks for the same CPU.

---

## Motivation

Real-time systems are defined by temporal guarantees, and the scheduling algorithm is what makes or breaks those guarantees. The theory is well established: EDF is optimal for uniprocessor preemptive scheduling and can in principle sustain utilization up to 100 %, while fixed-priority schemes like DMS have a lower schedulable bound but fail in a predictable, analysable order.

What the literature largely lacks is **measured behaviour on resource-constrained hardware**. Existing comparisons are simulation-based, or benchmark Rate Monotonic (RMS) rather than DMS, and rarely report the runtime cost of maintaining dynamic priorities on a device where every cycle is accounted for. This project measures that cost directly.

### Research question

> On a 168 MHz Cortex-M4 running FreeRTOS, how do DMS and EDF actually compare on response time, deadline adherence, achievable utilization, scheduling overhead, and failure behaviour under overload — and how does that change with the number of tasks?

---

## Approach

Both schedulers are built into one firmware image and run the same task set on the same board. Only the priority assignment differs; the scheduler is selected per run.

**DMS** sorts tasks by relative deadline once at startup and assigns static FreeRTOS priorities, shortest deadline first. At run time FreeRTOS's own scheduler does everything — no extra work.

**EDF** keeps priorities ordered by absolute deadline at run time:

- `vApplicationTickHook()` runs every tick (100 µs) and detects job releases.
- A manager task at priority 52 (above every measured task) re-sorts the active tasks by absolute deadline and calls `vTaskPrioritySet()` where the order changed.
- Each task gets its own priority level (2 … N+1, up to 50 tasks within `configMAX_PRIORITIES = 56`), so the ordering is exact, not bucketed.

Load is set by a synthetic workload: each synthetic job burns a calibrated number of CPU cycles (a 3-cycle assembly loop), scaled so that `U = Σ(Cᵢ/Tᵢ)` hits a target without changing the real peripheral tasks.

### Metrics

| Metric | Method |
|---|---|
| Response time | release → completion, DWT cycle counter (6 ns resolution); release time from the tick count anchored with `SysTick->VAL` |
| Start latency | release → first instruction of the job |
| Deadline adherence | response > D in cycles = miss; jobs still unfinished at the end of the window also count as misses |
| Response distribution | per-task histogram of R/D |
| CPU utilization | FreeRTOS run-time stats on the DWT clock: 1 − idle time / window |
| EDF overhead | tick-hook time (DWT) + manager task CPU time (run-time stats), and the delay from release detection to priorities written |

Each run is a fresh boot with one **10 s measurement window**. All tasks release first at tick 1000 (plus an optional per-task offset). Nothing is printed during the window: at its end the top-priority reporter suspends the scheduler, freezes the counters, prints the results, and resets the board into the next run.

### Task sets

Two real tasks are in every set:

| Task | Period | Deadline | WCET | Notes |
|---|---|---|---|---|
| Motor control | 10 ms | 8 ms | 2 ms | **Critical** — D < T; drives PWM + direction pins |
| Accelerometer read | 20 ms | 20 ms | 1.5 ms | LIS3DSH over SPI1 |

Task 0's deadline is deliberately shorter than its period. That is what makes this DMS rather than RMS, and it is the case where the two diverge.

Fixed sets A–D add three synthetic tasks (T / D in ms):

| Set | Structure | synth1 | synth2 | synth3 |
|---|---|---|---|---|
| A | harmonic, D < T | 50 / 40 | 100 / 100 | 200 / 150 |
| B | non-harmonic, D = T | 30 / 30 | 70 / 70 | 130 / 130 |
| C | non-harmonic, tight D | 30 / 18 | 70 / 40 | 130 / 70 |
| D | harmonic, D = T | 50 / 50 | 100 / 100 | 200 / 200 |

Generated sets (5–50 tasks) draw periods from {10, 20, 25, 40, 50, 80, 100, 125, 200} ms (2 s hyperperiod) and split the utilization with UUniFast.

---

## Experiments

All four are run automatically by the sweep driver (`sweep.c`): one flash runs every combination of an experiment. Trial 0 releases every task together (the critical instant); trials 1–2 use random per-task release offsets with the same seed for DMS and EDF, so each pair is directly comparable.

| # | Experiment | What varies | Runs | Why |
|---|---|---|---:|---|
| 1 | Fixed task sets | sets A–D × 21 U levels (50–130 %) × DMS/EDF × 3 trials | 504 | Effect of period and deadline structure; overload behaviour |
| 2 | Task count | N = 5, 10, 20, 35, 50 × U = 50–110 % (8 levels) × DMS/EDF × 3 trials | 240 | Does EDF's advantage survive as N grows? (after Zhang & Li, AICCONF 2025) |
| 3 | Observer effect | set A × U = 60/80/90/100 % × DMS/EDF × logging off/on × 3 trials | 48 | Cost of printing during measurement; justifies deferred reporting |
| 4 | Repeatability | (50 tasks, 70 %) and (35 tasks, 80 %) × DMS/EDF × 10 identical repeats | 40 | Is EDF's collapse random or reproducible? |

### Hypotheses vs outcome

| Hypothesis | Outcome |
|---|---|
| EDF sustains higher utilization before the first miss | True for ≤ 10 tasks and constrained deadlines; false at 35–50 tasks and on harmonic D = T sets |
| DMS fails predictably, lowest priority first; EDF fails scattered | Confirmed in all four fixed sets |
| DMS protects the critical motor task; EDF does not | Confirmed: 0 % vs ~97 % motor misses at U = 110 % |
| EDF has measurable overhead; DMS has none at run time | Confirmed; EDF overhead grows from 2 % to 18 % (U = 50 %) and up to 86 % under load |
| Recovery time after an overload burst | Not measured (Phase 3 dropped; see Scope changes) |

---

## Running a sweep

1. In `Core/Inc/sweep.h` set `SWEEP_ENABLE 1` and `SWEEP_EXPERIMENT` to 1, 2, 3 or 4.
2. Commit, then build and flash (Run in CubeIDE).
3. Log the serial port to a file (PuTTY, 115200 baud) **before** the board starts printing.
4. The board resets itself between runs. `##### SWEEP COMPLETE` and the blue LED (PD15) mark the end. Hold the USER button during reset to restart a sweep from run 1.

Approximate durations: experiment 1 ≈ 95 min, 2 ≈ 50 min, 3 ≈ 10 min, 4 ≈ 9 min. The run index lives in CCM RAM, which survives the reset, so no host software is needed. For a single manual run set `SWEEP_ENABLE 0` and the `MANUAL_` values.

Each run prints machine-readable lines, each preceded by its header line:

| Line | Content |
|---|---|
| `META` | provenance: firmware version, git commit, build ID, clocks, window, trial/repeat |
| `CSV` | per task: jobs, misses, unfinished, response min/mean/SD/max, start latency, CPU |
| `HIST` | per task: response-time histogram (R/D bins) |
| `OVH` | EDF hook and manager cost, priority-update delay |
| `UHIST` | histogram of EDF priority-update delay |

### Analysis

```
python analysis/analyze.py sw_4.log sw_2.log sw_1.log sw_3.log -o results
```

Requires Python 3 with pandas and matplotlib. The script splits the logs by experiment and writes, per experiment, tables (CSV and Markdown) and figures, plus `results/coverage.md` with integrity checks: missing runs, clock consistency, the EDF counter-freeze check and source traceability.

### Git stamping (optional)

`tools/stamp_git.py` writes the current commit hash and a dirty flag into `Core/Inc/git_version.h` (gitignored), which the firmware prints in every `META` line. Add it as a CubeIDE pre-build step: Project → Properties → C/C++ Build → Settings → Build Steps → Pre-build: `python "${ProjDirPath}/tools/stamp_git.py"`. Without it, `META` reports `git_commit = unknown`.

---

## Repository layout

```
.
├── Core/
│   ├── Inc/   FreeRTOSConfig.h  task_config.h  scheduler.h  workload.h
│   │          metrics.h  sweep.h  task_bodies.h  uart.h  motor.h  lis3dsh.h
│   └── Src/   main.c  freertos.c
│              scheduler.c       dispatcher: selects DMS or EDF per run
│              scheduler_dms.c   static deadline-monotonic priorities
│              scheduler_edf.c   tick hook + manager task, exact EDF ordering
│              task_config.c     fixed sets A–D, UUniFast generator, release offsets
│              task_bodies.c     motor, accel, synthetic and reporter tasks
│              workload.c        calibrated cycle burn
│              metrics.c         DWT timebase, response/miss/histogram, CPU
│              sweep.c           automatic sweep driver and result output
│              uart.c  motor.c  lis3dsh.c   peripheral drivers
├── Drivers/      STM32 HAL + CMSIS
├── Middlewares/  FreeRTOS kernel
├── analysis/
│   └── analyze.py    log parser, statistics, tables, figures
├── tools/
│   └── stamp_git.py  pre-build git stamp
└── RTOS_Implementation.ioc   CubeMX configuration — the authoritative peripheral config
```

Raw UART logs are not committed (several MB each).

---

## Hardware

**STM32F407VG Discovery** — Cortex-M4F @ 168 MHz, 192 KB RAM

| Peripheral | Interface | Pins |
|---|---|---|
| LIS3DSH accelerometer (onboard) | SPI1, Mode 1, 2.625 MHz | PA5/PA6/PA7, CS on PE3, INT1 on PE0 |
| L298N H-bridge + DC motor | TIM4_CH3 PWM + GPIO direction | PD14 (ENA), PD12 (IN1), PD13 (IN2) |
| USART2 → CP2102 USB-TTL | Async, 115200 baud | PA2 (TX), PA3 (RX) |
| USER button | EXTI0, rising edge, 200 ms debounce | PA0 |
| Status / sweep-complete LED | GPIO | PD15 (LD6) |

The motor runs from a separate 9 V supply, with common ground across MCU, L298N and CP2102. It is not needed for the sweeps: the motor task still drives the PWM and direction pins, so its timing load is the same with or without the motor powered.

### Clock and RTOS configuration

```
HSE 8 MHz crystal → PLL (M=8, N=336, P=2) → SYSCLK 168 MHz
APB1 42 MHz (timers 84 MHz)  |  APB2 84 MHz (timers 168 MHz)
FreeRTOS 10.3.1, CMSIS-OS v2
  configTICK_RATE_HZ                      10000   (100 µs resolution)
  configTOTAL_HEAP_SIZE                   80 KB
  configMAX_PRIORITIES                    56
  configUSE_TICK_HOOK                     1       (EDF release detection)
  configUSE_IDLE_HOOK                     1
  configGENERATE_RUN_TIME_STATS           1       (CPU utilization, DWT clock)
  configCHECK_FOR_STACK_OVERFLOW          2
  configUSE_NEWLIB_REENTRANT              1
  configUSE_PORT_OPTIMISED_TASK_SELECTION 0       (forced off by CMSIS-OS v2)
  HAL timebase                            TIM6    (SysTick belongs to FreeRTOS)
```

Two configuration notes matter for reproducibility. The 10 kHz tick exceeds CubeMX's GUI limit of 1000, so it is overridden (with the heap size and run-time stats) in the `USER CODE BEGIN Defines` block at the foot of `FreeRTOSConfig.h`. And `configUSE_PORT_OPTIMISED_TASK_SELECTION` cannot be enabled alongside CMSIS-OS v2, since the CLZ-based selector caps priorities at 32 while CMSIS-OS v2 requires 56. Task selection therefore uses the generic C path, which adds a small constant cost to every context switch, identical for both schedulers.

## Build

Requires STM32CubeIDE 2.1.1 or later.

1. Import the project, or open the `.ioc` and generate code.
2. Confirm `-u _printf_float` is set under **C/C++ Build → Settings → MCU GCC Linker → Miscellaneous** (needed for `%f` in UART output).
3. Build and flash (Run).
4. Serial terminal at 115200 baud on the CP2102's COM port.

Peripheral changes go through the `.ioc` and regeneration — never by hand-editing generated init code. Commit before regenerating; CubeMX only preserves code inside `USER CODE` guards.

---

## Scope changes

**DHT22 dropped.** The temperature/humidity sensor was to be a third real task (100 ms period, 3 ms WCET). Its one-wire protocol needs about 5 ms of `__disable_irq()` per read, which on a 10 kHz tick blocks 50 scheduler ticks and would inject artefacts into exactly the timing the project measures. A calibrated busy-wait reaches the same utilization points without it.

**Binary packet export dropped.** The planned `0xAA`-framed binary protocol and `serial_receiver.py` were replaced by self-describing text lines (`META`/`CSV`/`HIST`/`OVH`/`UHIST`) printed after each window, parsed directly by `analyze.py`.

**Phase 3 (recovery after an overload burst) not run.** The sweeps cover failure onset and steady overload up to U = 130 %; recovery time is future work.

**Task-count experiment added** (5–50 tasks), following Zhang & Li (AICCONF 2025), along with the observer-effect and repeatability experiments.

## Limitations

- Single core, single board; one generated task set per task count (seeds vary only release offsets).
- EDF is emulated on a fixed-priority kernel; its overhead reflects this implementation (O(N) tick scan, re-sort per release), not EDF in general. A native or incremental EDF would cost less.
- 10 s window per run; long-run effects beyond that are not measured.
- U is the nominal target from the calibrated burn; measured CPU runs 1–5 points higher because of kernel and interrupt time.
- Timing is measured on-chip with the DWT counter; no oscilloscope cross-check yet.
- The DWT counter wraps every ~25.6 s at 168 MHz; all differences use wrap-safe unsigned arithmetic, and one run is ~11 s.

## Version history

| Version | Change |
|---|---|
| v0.4 | Both schedulers on hardware, manual runs |
| v0.5 | Automatic sweeps, task-count experiment, trials, histograms, EDF update-delay and manager cost, observer effect |
| v0.5.1 | Counters frozen at window end, cycle-exact miss test, git stamping, repeatability experiment — all reported results use this version |

## Environment

STM32CubeIDE 2.1.1 · FreeRTOS 10.3.1 (CMSIS-OS v2) · Python 3 with pandas and matplotlib · PuTTY at 115200 baud
