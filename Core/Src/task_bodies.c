#include "task_bodies.h"
#include "task_config.h"
#include "workload.h"
#include "metrics.h"
#include "motor.h"
#include "lis3dsh.h"
#include "uart.h"
#include "FreeRTOS.h"
#include "task.h"

/* Motor control — critical task, D < T */
void MotorTaskBody(void *argument)
{
    uint32_t id = (uint32_t)argument;
    TickType_t last_wake = xTaskGetTickCount();
    uint8_t speed = 40;

    for (;;) {
        uint32_t release = DWT->CYCCNT;

        Motor_Set(MOTOR_FORWARD, speed);

        /* Pad to nominal WCET so utilization is as specified.
           Replace with measured value once characterised. */
        Workload_BurnCycles(scaled_wcet_cycles[id]);

        Metrics_RecordCompletion(id, release, DWT->CYCCNT);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(task_set[id].period_ms));
    }
}

/* Accelerometer read — real SPI transaction, D = T */
void AccelTaskBody(void *argument)
{
    uint32_t id = (uint32_t)argument;
    TickType_t last_wake = xTaskGetTickCount();
    LIS3DSH_Data_t data;

    for (;;) {
        uint32_t release = DWT->CYCCNT;

        LIS3DSH_ReadXYZ(&data);

        Workload_BurnCycles(scaled_wcet_cycles[id]);

        Metrics_RecordCompletion(id, release, DWT->CYCCNT);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(task_set[id].period_ms));
    }
}

/* Synthetic load — calibrated burn only */
void SyntheticTaskBody(void *argument)
{
    uint32_t id = (uint32_t)argument;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        uint32_t release = DWT->CYCCNT;

        Workload_BurnCycles(scaled_wcet_cycles[id]);

        Metrics_RecordCompletion(id, release, DWT->CYCCNT);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(task_set[id].period_ms));
    }
}

/* Lowest-priority reporter — prints the metrics table periodically.
   Not part of the measured task set. */
void ReporterTaskBody(void *argument)
{
    (void)argument;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5000));
        Metrics_Print();
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_15);
    }
}
