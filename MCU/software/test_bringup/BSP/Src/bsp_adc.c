#include "bsp_adc.h"

#include <stdbool.h>

#include "adc.h"
#include "stm32f4xx_hal_def.h"

/**
 * @brief 已检出的 ADC 配置模式，决定哪些读取接口可用。
 */
typedef enum
{
    ADC_MODE_UNKNOWN = 0,
    ADC_MODE_POLL_SINGLE,
    ADC_MODE_DMA_SCAN
} adc_mode_t;

static bool s_is_initialized = false;
static adc_mode_t s_mode = ADC_MODE_UNKNOWN;
static uint32_t s_dma_buffer[BSP_ADC_CHANNEL_COUNT] = {0};
static volatile bool s_is_adc_ready = false;
static volatile bool s_is_adc_error = false;

/**
 * @brief 依据 hadc1 的配置判断当前是单路轮询还是六路 DMA 扫描。
 *
 * @retval BSP_OK 配置匹配某一受支持模式，s_mode 已更新。
 * @retval BSP_ERR_CONFIG 句柄或配置与两种模式都不相符。
 *
 * @note 只读取 HAL 句柄中已写入的配置，不修改寄存器、不启动转换。
 */
static bsp_status_t detect_adc_mode(void)
{
    if (hadc1.Instance != ADC1)
    {
        return BSP_ERR_CONFIG;
    }
    if ((hadc1.Init.Resolution != ADC_RESOLUTION_12B) ||
        (hadc1.Init.DataAlign != ADC_DATAALIGN_RIGHT) ||
        (hadc1.Init.ContinuousConvMode != DISABLE) ||
        (hadc1.Init.DiscontinuousConvMode != DISABLE) ||
        (hadc1.Init.ExternalTrigConvEdge != ADC_EXTERNALTRIGCONVEDGE_NONE))
    {
        return BSP_ERR_CONFIG;
    }
    if ((hadc1.Init.ScanConvMode == ENABLE) &&
        (hadc1.Init.NbrOfConversion == BSP_ADC_CHANNEL_COUNT) &&
        (hadc1.Init.EOCSelection == ADC_EOC_SEQ_CONV) &&
        (hadc1.Init.DMAContinuousRequests == DISABLE) &&
        (hadc1.DMA_Handle != NULL) &&
        (hadc1.DMA_Handle->Init.Mode == DMA_NORMAL) &&
        (hadc1.DMA_Handle->Init.MemInc == DMA_MINC_ENABLE) &&
        (hadc1.DMA_Handle->Init.PeriphInc == DMA_PINC_DISABLE))
    {
        s_mode = ADC_MODE_DMA_SCAN;
        return BSP_OK;
    }
    if ((hadc1.Init.ScanConvMode == DISABLE) &&
        (hadc1.Init.NbrOfConversion == 1U) &&
        (hadc1.Init.EOCSelection == ADC_EOC_SINGLE_CONV))
    {
        s_mode = ADC_MODE_POLL_SINGLE;
        return BSP_OK;
    }
    return BSP_ERR_CONFIG;
}

bsp_status_t bsp_adc_init(void)
{
    bsp_status_t status = detect_adc_mode();
    if (status != BSP_OK)
    {
        s_is_initialized = false;
        return status;
    }
    s_is_initialized = true;
    return BSP_OK;
}

bsp_status_t bsp_adc_read(uint16_t *raw_value, uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (s_mode != ADC_MODE_POLL_SINGLE)
    {
        return BSP_ERR_CONFIG;
    }
    if ((raw_value == NULL) || (timeout_ms == 0U) ||
        (timeout_ms > BSP_ADC_MAX_TIMEOUT_MS))
    {
        return BSP_ERR_PARAM;
    }
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
        return BSP_ERR_HAL;
    }
    HAL_StatusTypeDef status = HAL_ADC_PollForConversion(&hadc1, timeout_ms);
    if (HAL_ADC_Stop(&hadc1) != HAL_OK)
    {
        return BSP_ERR_HAL;
    }
    if (status == HAL_TIMEOUT)
    {
        return BSP_ERR_TIMEOUT;
    }
    if (status != HAL_OK)
    {
        return BSP_ERR_HAL;
    }

    *raw_value = HAL_ADC_GetValue(&hadc1);
    return BSP_OK;
}

bsp_status_t bsp_adc_read_all(uint16_t raw_values[BSP_ADC_CHANNEL_COUNT],
                              uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (s_mode != ADC_MODE_DMA_SCAN)
    {
        return BSP_ERR_CONFIG;
    }
    if ((raw_values == NULL) || (timeout_ms == 0U) ||
        (timeout_ms > BSP_ADC_MAX_TIMEOUT_MS))
    {
        return BSP_ERR_PARAM;
    }
    s_is_adc_error = false;
    s_is_adc_ready = false;
    uint32_t start_ms = HAL_GetTick();
    if (HAL_ADC_Start_DMA(&hadc1, s_dma_buffer, BSP_ADC_CHANNEL_COUNT) !=
        HAL_OK)
    {
        return BSP_ERR_HAL;
    }

    bsp_status_t result = BSP_OK;
    while (1)
    {
        if (s_is_adc_error)
        {
            result = BSP_ERR_HAL;
            break;
        }
        if (s_is_adc_ready)
        {
            break;
        }
        if ((uint32_t)(HAL_GetTick() - start_ms) >= timeout_ms)
        {
            result = BSP_ERR_TIMEOUT;
            break;
        }
    }
    if (HAL_ADC_Stop_DMA(&hadc1) != HAL_OK)
    {
        return BSP_ERR_HAL;
    }
    if (s_is_adc_error)
    {
        return BSP_ERR_HAL;
    }
    if (result != BSP_OK)
    {
        return result;
    }
    for (uint32_t i = 0U; i < BSP_ADC_CHANNEL_COUNT; i++)
    {
        raw_values[i] = (uint16_t)s_dma_buffer[i];
    }
    return BSP_OK;
}

void bsp_adc_on_conv_cplt_from_isr(void)
{
    s_is_adc_ready = true;
}

void bsp_adc_on_error_from_isr(void)
{
    s_is_adc_error = true;
}
