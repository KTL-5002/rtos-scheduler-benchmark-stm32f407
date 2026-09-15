#include "lis3dsh.h"
#include "uart.h"
#define LIS3DSH_WHO_AM_I   0x0F
#define LIS3DSH_CTRL_REG4  0x20
#define LIS3DSH_CTRL_REG5  0x24
#define LIS3DSH_STATUS     0x27
#define LIS3DSH_OUT_X_L    0x28
#define LIS3DSH_READ       0x80
#define LIS3DSH_MULTI      0x40

static SPI_HandleTypeDef *_hspi;

#define CS_LOW()   HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_RESET)
#define CS_HIGH()  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_SET)

static void WriteReg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg & 0x3F, val };
    CS_LOW();
    HAL_SPI_Transmit(_hspi, buf, 2, 10);
    CS_HIGH();
}

static uint8_t ReadReg(uint8_t reg)
{
    uint8_t tx = reg | LIS3DSH_READ;
    uint8_t rx = 0;
    CS_LOW();
    HAL_SPI_Transmit(_hspi, &tx, 1, 10);
    HAL_SPI_Receive (_hspi, &rx, 1, 10);
    CS_HIGH();
    return rx;
}

static void ReadBurst(uint8_t reg, uint8_t *buf, uint8_t len)
{
    // Read one byte at a time instead of burst
    for (uint8_t i = 0; i < len; i++) {
        buf[i] = ReadReg(reg + i);
    }
}

HAL_StatusTypeDef LIS3DSH_Init(SPI_HandleTypeDef *hspi)
{
    _hspi = hspi;
    CS_HIGH();
    HAL_Delay(200);

    uint8_t id = ReadReg(LIS3DSH_WHO_AM_I);
    UART_Printf("WHO_AM_I = 0x%02X\r\n", id);
    if (id != 0x3F) return HAL_ERROR;

    // Step 1: Power down first
    WriteReg(LIS3DSH_CTRL_REG4, 0x00);
    HAL_Delay(50);

    // Step 2: Set full scale ±2g, BW 800Hz
    WriteReg(LIS3DSH_CTRL_REG5, 0x00);
    HAL_Delay(10);

    // Step 3: Power on 100Hz, enable all axes, BDU on
    WriteReg(LIS3DSH_CTRL_REG4, 0x6F);
    HAL_Delay(100);

    // Step 4: Verify it wrote correctly
    uint8_t verify = ReadReg(LIS3DSH_CTRL_REG4);
    UART_Printf("CTRL_REG4 = 0x%02X (expect 0x6F)\r\n", verify);

    return HAL_OK;
}
void LIS3DSH_ReadXYZ(LIS3DSH_Data_t *data)
{
    uint8_t raw[6];

    ReadBurst(LIS3DSH_OUT_X_L, raw, 6);

    // Debug raw bytes


    data->x = (int16_t)(raw[1] << 8 | raw[0]);
    data->y = (int16_t)(raw[3] << 8 | raw[2]);
    data->z = (int16_t)(raw[5] << 8 | raw[4]);

    data->x_mg = data->x * 0.06f;
    data->y_mg = data->y * 0.06f;
    data->z_mg = data->z * 0.06f;
}
