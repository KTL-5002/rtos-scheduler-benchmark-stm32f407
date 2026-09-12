#ifndef LIS3DSH_H
#define LIS3DSH_H
#include "stm32f4xx_hal.h"

typedef struct {
    int16_t x, y, z;
    float x_mg, y_mg, z_mg;
} LIS3DSH_Data_t;

HAL_StatusTypeDef LIS3DSH_Init(SPI_HandleTypeDef *hspi);
void LIS3DSH_ReadXYZ(LIS3DSH_Data_t *data);

#endif
