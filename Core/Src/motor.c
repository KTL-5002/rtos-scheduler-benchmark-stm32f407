#include "motor.h"

#define IN1_PORT  GPIOD
#define IN1_PIN   GPIO_PIN_12
#define IN2_PORT  GPIOD
#define IN2_PIN   GPIO_PIN_13
#define PWM_MAX   1000U

extern TIM_HandleTypeDef htim4;

static void Set_Dir_Pins(uint8_t in1, uint8_t in2)
{
    HAL_GPIO_WritePin(IN1_PORT, IN1_PIN,
        in1 ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(IN2_PORT, IN2_PIN,
        in2 ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void Motor_Set(Motor_Dir_t dir, uint8_t speed)
{
    if (speed > 100) speed = 100;
    uint32_t ccr = (speed * PWM_MAX) / 100;

    switch (dir) {
        case MOTOR_FORWARD:
            Set_Dir_Pins(1, 0);
            break;
        case MOTOR_REVERSE:
            Set_Dir_Pins(0, 1);
            break;
        case MOTOR_BRAKE:
            Set_Dir_Pins(1, 1);
            ccr = PWM_MAX;
            break;
        default:
            Set_Dir_Pins(0, 0);
            ccr = 0;
            break;
    }
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_3, ccr);
}

void Motor_Ramp(Motor_Dir_t dir, uint8_t target,
                uint32_t step_ms)
{
    static uint8_t current = 0;
    while (current != target) {
        current += (target > current) ? 1 : -1;
        Motor_Set(dir, current);
        HAL_Delay(step_ms);
    }
}
