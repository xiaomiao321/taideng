#include "bsp_adc.h"

#include <stdbool.h>

#include "adc.h"
#include "stm32f4xx_hal_def.h"

static bool s_is_initialized = false;
void bsp_adc_init(void)
{
    s_is_initialized = true;
}

bsp_adc_status_t bsp_adc_read(uint16_t *raw_value, uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_ADC_STATUS_NOT_INITIALIZED;
    }
    if ((raw_value == NULL) || (timeout_ms == 0U) ||
        (timeout_ms > BSP_ADC_MAX_TIMEOUT_MS))
    {
        return BSP_ADC_STATUS_INVALID_ARGUMENT;
    }
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
        return BSP_ADC_STATUS_ADC_ERROR;
    }
    HAL_StatusTypeDef status = HAL_ADC_PollForConversion(&hadc1, timeout_ms);
    if (HAL_ADC_Stop(&hadc1) != HAL_OK)
    {
        return BSP_ADC_STATUS_ADC_ERROR;
    }
    if (status == HAL_TIMEOUT)
    {
        return BSP_ADC_STATUS_TIMEOUT;
    }
    if (status != HAL_OK)
    {
        return BSP_ADC_STATUS_ADC_ERROR;
    }

    *raw_value = HAL_ADC_GetValue(&hadc1);
    return BSP_ADC_STATUS_OK;
}
