# DMS vs EDF: A Hardware-Validated Scheduler Benchmark on STM32F407

A comparative study of **Deadline Monotonic Scheduling (DMS)** and **Earliest Deadline First (EDF)** implemented on real silicon — an STM32F407 Discovery board running FreeRTOS — under identical, controllable workloads.

---

## Motivation

Real-time systems are defined by temporal guarantees, and the scheduling algorithm is what makes or breaks those guarantees. The theory here is well established: EDF is optimal for uniprocessor preemptive scheduling and can in principle sustain utilization up to 100%, while fixed-priority schemes like DMS have a lower schedulable bound but fail in a predictable, analysable order.

What the literature largely lacks is **measured behaviour on resource-constrained hardware**. Existing comparisons are simulation-based, or they benchmark Rate Monotonic (RMS) rather than DMS, and they rarely report the runtime cost of maintaining dynamic priorities on a device where every cycle is accounted for. This project measures that cost directly.

### Research question

> On a 168 MHz Cortex-M4 running FreeRTOS, how do DMS and EDF actually compare on response time, deadline adherence, achievable utilization, scheduling overhead, and failure behaviour under overload?

---

## Approach

Both schedulers run the same task set on the same board with the same peripheral workload. Only the priority assignment mechanism differs:

**DMS** sorts tasks by relative deadline once at startup, assigns static FreeRTOS priorities shortest-deadline-first, and then does nothing at runtime. The native scheduler handles everything. Zero runtime overhead by construction.

**EDF** assigns priorities dynamically. `vApplicationTickHook()` runs on every tick, computes each task's remaining time to its absolute deadline, maps that onto a priority bucket, and calls `vTaskPrioritySet()` where the bucket changed. Better theoretical utilization, at a measurable per-tick cost.

Utilization is swept by a synthetic workload generator that busy-waits for calibrated durations, so the load can be dialled to a target `U = Σ(Cᵢ/Tᵢ)` without changing the real peripheral tasks.

### Metrics

| # | Metric | Method |
|---|--------|--------|
| 1 | Response time | `completion − release`, DWT cycle counter (~6 ns resolution) |
| 2 | Deadline adherence | completion time vs. absolute deadline, per job |
| 3 | CPU utilization | idle hook tick counting: `idle_ticks / total_ticks` |
| 4 | Scheduling overhead | context switch + tick hook duration via DWT in trace macros |
| 5 | Miss rate under overload | per-task miss counters plus a 2048-entry circular miss log |

### Task set

| ID | Task | Period | Deadline | WCET | Notes |
|----|------|--------|----------|------|-------|
| 0 | Motor control | 10 ms | 8 ms | 2 ms | **Critical** — deadline < period |
| 1 | Accelerometer read | 20 ms | 20 ms | 1.5 ms | SPI1, polled |
| 2–N | Synthetic | variable | variable | variable | Calibrated busy-wait |

```
HSE 8 MHz crystal → PLL (M=8, N=336, P=2) → SYSCLK 168 MHz
APB1 42 MHz (timers 84 MHz)  |  APB2 84 MHz (timers 168 MHz)
FreeRTOS 10.3.1, CMSIS-OS v2
  configTICK_RATE_HZ                      10000   (100 µs resolution)
  configTOTAL_HEAP_SIZE                   40960
  configUSE_TICK_HOOK                     1       (EDF priority recompute)
  configUSE_TRACE_FACILITY                1       (context switch timing)
  configCHECK_FOR_STACK_OVERFLOW          2
  configUSE_NEWLIB_REENTRANT              1
  configUSE_PORT_OPTIMISED_TASK_SELECTION 0       (forced off by CMSIS-OS v2)
```


---

## Experiment design

**Phase 1 — Baseline.** Utilization 50–85% in 5% steps, both schedulers. Expectation: zero misses throughout. Establishes clean response-time distributions and the overhead baseline.

**Phase 2 — Failure onset.** Utilization 90–130% in 5% steps. Records which task misses first, miss timestamps, and consecutive-miss runs. DMS should fail in strict priority order; EDF's empirical failure distribution is the open question.

**Phase 3 — Recovery.** Button-triggered 500 ms overload burst at roughly 20% above the schedulable limit. Measures time until all tasks meet deadlines again. Oscilloscope capture for hardware-resolution timing of the miss cluster.

Metrics leave the board as binary packets — `0xAA` sync, type byte, timestamp, task ID, payload, CRC16 — parsed by `serial_receiver.py` into CSV, then analysed with pandas and matplotlib.

### Hypotheses

- EDF sustains meaningfully higher utilization before first miss (~94% vs ~78%)
- DMS fails predictably, lowest priority first; EDF fails scattered
- DMS protects the critical motor task; EDF does not privilege it
- EDF shows measurable tick-hook overhead; DMS shows none at runtime
- Recovery time is repeatable for both, and differs between them

---

## Repository layout

```
.
├── Core/
│   ├── Inc/          uart.h  motor.h  lis3dsh.h
│   │                 task_config.h  scheduler.h  workload.h  metrics.h
│   │                 FreeRTOSConfig.h
│   └── Src/          uart.c  motor.c  lis3dsh.c
│                     scheduler_dms.c  scheduler_edf.c  task_bodies.c
│                     workload.c  metrics.c
│                     main.c  freertos.c
├── Drivers/          STM32 HAL + CMSIS
├── Middlewares/      FreeRTOS kernel
├── analysis/
│   ├── serial_receiver.py    binary packet → CSV
│   └── analyze.py            statistics + plots
├── data/
│   ├── raw/          UART captures (gitignored — large)
│   └── csv/          parsed, committed
├── docs/             report, plots, figures
└── *.ioc             CubeMX configuration — the authoritative peripheral config
```

RTOS code lives in `freertos.c` and the scheduler files, not `main.c`. `main.c` initialises peripherals and starts the kernel, nothing more.

## Build

Requires STM32CubeIDE 2.1.1 or later.

1. Import the project, or open the `.ioc` and generate code
2. Confirm `-u _printf_float` is set under **C/C++ Build → Settings → MCU GCC Linker → Miscellaneous** (needed for `%f` in UART output)
3. Build (`Ctrl+B`), flash (`F11`, then `F8`)
4. Serial terminal at 115200 baud on the CP2102's COM port

Peripheral changes go through the `.ioc` and regeneration — never by hand-editing generated init code. Commit before regenerating; CubeMX only preserves code inside `USER CODE` guards.

---
---

# Progress Tracker

**Status:** Week 1 in progress · Integration phase
**Last updated:** 12 September 2026

### Milestones

| Phase | Deliverable | Status |
|---|---|---|
| Week 1 | One project, all peripherals alive, FreeRTOS running | 🟡 In progress |
| Week 2 | Both schedulers on hardware, metrics streaming | ⬜ Not started |
| Week 3 | Phase 1 + Phase 2 raw data collected | ⬜ Not started |
| Week 4 | Analysis pipeline, plots, final report, live demo | ⬜ Not started |

---

## Week 1 — Integration

| Day | Task | Status |
|---|---|---|
| 1–2 | Combined CubeMX project: USART2, TIM4, SPI1, PA0 EXTI, FreeRTOS @ 10 kHz | ✅ Done |
| 1–2 | Clock tree 168 MHz, TIM6 timebase, NVIC priorities (EXTI0 = 5) | ✅ Done |
| 1–2 | `FreeRTOSConfig.h` tick override in USER CODE block, survives regeneration | ✅ Done |
| 1–2 | Blink test — LED on PD15, verified 10 kHz tick | ✅ Done |
| 1–2 | Git repo initialised, pushed to GitHub | 🟡 In progress |
| 3 | Copy `uart.c/h`, `motor.c/h`, `lis3dsh.c/h` into project | ⬜ |
| 3 | New `main.c` — init all peripherals before `osKernelStart()` | ⬜ |
| 4 | Build clean, resolve include/linker errors | ⬜ |
| 4 | Verify each peripheral standalone in combined project | ⬜ |
| 5 | Task set: 2 real + synthetic; add `task_config.h`, `workload.h/c` | ⬜ |
| 5 | Recalculate utilization formula (DHT22 contribution removed) | ⬜ |
| — | Weekend buffer | ⬜ |

**Day 4 verification checklist** — each must pass independently inside the combined project:

- [ ] Motor spins forward, reverse, stops; PWM duty control responsive
- [ ] LIS3DSH `WHO_AM_I` returns `0x3F`; X/Y/Z change on tilt
- [ ] `UART_Printf` output legible in terminal, including `%f` floats
- [ ] Button press increments count, debounce holds at 200 ms
- [ ] No HAL timeouts (confirms TIM6 timebase is working)

---

## Week 2 — Schedulers

| Day | Task | Status |
|---|---|---|
| 1–2 | `scheduler_dms.c` — deadline sort, static priority assignment, task creation | ⬜ |
| 1–2 | Verify via UART: all tasks run, motor never misses at low utilization | ⬜ |
| 3–4 | `scheduler_edf.c` — tick hook, `EDF_UpdatePriorities()`, bucket mapping | ⬜ |
| 3–4 | Verify dynamic priority changes are occurring (log transitions over UART) | ⬜ |
| 5 | `metrics.c` — packet format, DWT timing, miss logging | ⬜ |
| 5 | Clean timing data out over UART for both schedulers at one utilization point | ⬜ |
| — | Measure bare tick-hook cost with DWT before adding load | ⬜ |
| — | Weekend buffer | ⬜ |

---

## Week 3 — Experiments

| Day | Task | Status |
|---|---|---|
| 1–2 | Phase 1 baseline, 50–85% @ 5% steps — DMS | ⬜ |
| 1–2 | Phase 1 baseline, 50–85% @ 5% steps — EDF | ⬜ |
| 3–4 | Phase 2 overload, 90–130% @ 5% steps — DMS | ⬜ |
| 3–4 | Phase 2 overload, 90–130% @ 5% steps — EDF | ⬜ |
| 3–4 | Archive all raw UART logs with run metadata | ⬜ |
| 5 | Phase 3 recovery burst — button trigger, scope capture | ⬜ |
| — | Weekend buffer — re-run anything inconsistent | ⬜ |

---

## Week 4 — Analysis and report

| Day | Task | Status |
|---|---|---|
| 1–2 | `serial_receiver.py` — packet parsing, CRC check, CSV export | ⬜ |
| 3 | `analyze.py` — response time vs utilization, miss distribution, critical task protection, overhead | ⬜ |
| 4–5 | Final report — problem, method, results, DMS vs EDF conclusions, limitations | ⬜ |
| — | Polish, slides, rehearse 15-minute demo | ⬜ |

---

## Slack plan

Integration bugs are likely. If schedule pressure appears, cut in this order:

1. Phase 3 oscilloscope capture — log via UART instead
2. Utilization step granularity — 10% steps instead of 5%
3. Synthetic task count — trim from the default set

**Protect Phase 1 + Phase 2 data for both schedulers.** That pair is what answers the research question; everything else is enrichment.

---

## Scope changes

**DHT22 dropped.** The temperature/humidity sensor was to be Task 2 (period 100 ms, deadline 100 ms, WCET 3 ms). Its one-wire protocol needs a 1 MHz timer and a critical section with interrupts disabled for the full 40-bit read — roughly 5 ms of `__disable_irq()`. On a 10 kHz tick that blocks 50 scheduler ticks, which would have injected priority-inversion-like artefacts into exactly the timing measurements the project exists to make. It was also the one peripheral still failing integration.

Dropping it costs nothing scientifically: the task set is synthetic-extensible, so the same utilization points are reachable without it, and a bit-banged sensor contributes nothing that a calibrated busy-wait doesn't. Recorded here and in the report's limitations section for transparency.

## Known limitations to state in the report

- Port-optimised task selection disabled (CMSIS-OS v2 requires 56 priorities; CLZ selector caps at 32). Adds a constant per-switch cost, identical for both schedulers, measured rather than assumed.
- EDF priority granularity is bounded by available priority buckets, so it is an approximation of true EDF rather than an exact implementation.
- Single-core, single-board results. No claim is made about multiprocessor behaviour.
- DWT cycle counter wraps every ~25.6 s at 168 MHz; long-run measurements handle wraparound explicitly.
- Utilization is a nominal target computed from calibrated WCET, not a measured instantaneous value.

---

## Environment

STM32CubeIDE 2.1.1 · FreeRTOS 10.3.1 (CMSIS-OS v2) · Python 3.x with pandas, matplotlib, pyserial · serial terminal at 115200 baud

## References

Peripheral bring-up reports for USART2, DC motor, USER button, and LIS3DSH — including SPI mode troubleshooting and the float-printf linker fix — are in `docs/`.Two configuration notes that matter for reproducibility. The 10 kHz tick exceeds CubeMX's GUI limit of 1000, so it is overridden in the `USER CODE BEGIN Defines` block at the foot of `FreeRTOSConfig.h`. And `configUSE_PORT_OPTIMISED_TASK_SELECTION` cannot be enabled alongside CMSIS-OS v2, since the CLZ-based bitmap selector caps priorities at 32 while CMSIS-OS v2 requires 56 — so task selection uses the generic C path. This adds a small constant cost to every context switch, identically for both schedulers, and is measured rather than assumed.
  HAL timebase                            TIM6    (SysTick belongs to FreeRTOS)
  configUSE_IDLE_HOOK                     1       (utilization measurement)
  configMAX_PRIORITIES                    56


Motor runs from a separate 9 V supply; common ground across MCU, L298N, and CP2102 is essential.


### Clock and RTOS configuration
| Status heartbeat | GPIO | PD15 (LD6) |
| USER button | EXTI0, rising edge, 200 ms debounce | PA0 |
Task 0's deadline is deliberately shorter than its period. That's what makes this DMS rather than RMS, and it's the case where the two diverge.

---

## Hardware

| L298N H-bridge + DC motor | TIM4_CH3 PWM + GPIO direction | PD14 (ENA), PD12 (IN1), PD13 (IN2) |
**STM32F407VG Discovery** — Cortex-M4F @ 168 MHz, 192 KB RAM

| LIS3DSH accelerometer (onboard) | SPI1, Mode 1, 2.625 MHz | PA5/PA6/PA7, CS on PE3, INT1 on PE0 |
| Peripheral | Interface | Pins |
| USART2 → CP2102 USB-TTL | Async, 115200 (data export at 921600) | PA2 (TX), PA3 (RX) |

