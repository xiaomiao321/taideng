#include "bsp_adc.h"

#include <stdbool.h>

#include "adc.h"
#include "stm32f4xx_hal_def.h"

static bool s_is_initialized = false;
static uint32_t s_dma_buffer[BSP_ADC_CHANNEL_COUNT] = {0};
static volatile bool s_is_adc_ready = false;
static volatile bool s_is_adc_error = false;
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

bsp_adc_status_t bsp_adc_read_all(uint16_t raw_values[BSP_ADC_CHANNEL_COUNT],
                                  uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_ADC_STATUS_NOT_INITIALIZED;
    }
    if ((raw_values == NULL) || (timeout_ms == 0U) ||
        (timeout_ms > BSP_ADC_MAX_TIMEOUT_MS))
    {
        return BSP_ADC_STATUS_INVALID_ARGUMENT;
    }
    s_is_adc_error = false;
    s_is_adc_ready = false;
    uint32_t start_ms = HAL_GetTick();
    if (HAL_ADC_Start_DMA(&hadc1, s_dma_buffer, BSP_ADC_CHANNEL_COUNT) !=
        HAL_OK)
    {
        return BSP_ADC_STATUS_ADC_ERROR;
    }

    bsp_adc_status_t result = BSP_ADC_STATUS_OK;
    while (1)
    {
        if (s_is_adc_error)
        {
            result = BSP_ADC_STATUS_ADC_ERROR;
            break;
        }
        if (s_is_adc_ready)
        {
            break;
        }
        if ((uint32_t)(HAL_GetTick() - start_ms) >= timeout_ms)
        {
            result = BSP_ADC_STATUS_TIMEOUT;
            break;
        }
    }
    if (HAL_ADC_Stop_DMA(&hadc1) != HAL_OK)
    {
        return BSP_ADC_STATUS_ADC_ERROR;
    }
    if (s_is_adc_error)
    {
        return BSP_ADC_STATUS_ADC_ERROR;
    }
    if (result != BSP_ADC_STATUS_OK)
    {
        return result;
    }
    for (uint32_t i = 0U; i < BSP_ADC_CHANNEL_COUNT; i++)
    {
        raw_values[i] = (uint16_t)s_dma_buffer[i];
    }
    return BSP_ADC_STATUS_OK;
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc == &hadc1)
    {
        s_is_adc_ready = true;
    }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc == &hadc1)
    {
        s_is_adc_error = true;
    }
}