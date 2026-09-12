#ifndef MOTOR_H
#define MOTOR_H
#include "stm32f4xx_hal.h"

typedef enum {
    MOTOR_STOP    = 0,
    MOTOR_FORWARD = 1,
    MOTOR_REVERSE = 2,
    MOTOR_BRAKE   = 3,
} Motor_Dir_t;

void Motor_Set(Motor_Dir_t dir, uint8_t speed);
void Motor_Ramp(Motor_Dir_t dir, uint8_t target,
                uint32_t step_ms);

#endif
