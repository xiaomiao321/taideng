#include "bsp_adc.h"

#include <stdbool.h>

#include "adc.h"
#include "stm32f4xx_hal_def.h"

static bool s_is_initialized = false;
/** @brief DMA 目标缓冲：单次扫描只用前六个，连续扫描时一轮六个依次排开。 */
static uint32_t s_dma_buffer[BSP_ADC_CHANNEL_COUNT * BSP_ADC_BURST_MAX_SCANS] =
    {0};
static volatile bool s_is_adc_ready = false;
static volatile bool s_is_adc_error = false;

/**
 * @brief 校验 hadc1 是否为六路 DMA 扫描配置。
 *
 * @retval BSP_OK 配置符合。
 * @retval BSP_ERR_CONFIG 句柄或配置不符。
 *
 * @note 只读取 HAL 句柄中已写入的配置，不修改寄存器、不启动转换。
 */
static bsp_status_t check_adc_config(void)
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
        return BSP_OK;
    }
    return BSP_ERR_CONFIG;
}

bsp_status_t bsp_adc_init(void)
{
    bsp_status_t status = check_adc_config();
    if (status != BSP_OK)
    {
        s_is_initialized = false;
        return status;
    }
    s_is_initialized = true;
    return BSP_OK;
}

bsp_status_t bsp_adc_read_all(uint16_t raw_values[BSP_ADC_CHANNEL_COUNT],
                              uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
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

bsp_status_t bsp_adc_read_burst(uint16_t (*scans)[BSP_ADC_CHANNEL_COUNT],
                                uint32_t count, uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if ((scans == NULL) || (count == 0U) || (count > BSP_ADC_BURST_MAX_SCANS) ||
        (timeout_ms == 0U) || (timeout_ms > BSP_ADC_MAX_TIMEOUT_MS))
    {
        return BSP_ERR_PARAM;
    }
    s_is_adc_error = false;
    s_is_adc_ready = false;
    uint32_t start_ms = HAL_GetTick();

    /* 连续转换：一轮扫完自动开始下一轮，DMA 收满 count 轮后不再取数。
     * 之后 ADC 还会空转到下面的 Stop 为止，没人读的数据会置溢出标志；数据在
     * DMA 完成时已经收齐，所以关掉溢出中断，不把它当成错误。溢出标志由下一次
     * HAL_ADC_Start_DMA() 清除。 */
    SET_BIT(hadc1.Instance->CR2, ADC_CR2_CONT);
    if (HAL_ADC_Start_DMA(&hadc1, s_dma_buffer,
                          count * BSP_ADC_CHANNEL_COUNT) != HAL_OK)
    {
        CLEAR_BIT(hadc1.Instance->CR2, ADC_CR2_CONT);
        return BSP_ERR_HAL;
    }
    __HAL_ADC_DISABLE_IT(&hadc1, ADC_IT_OVR);

    bsp_status_t result = BSP_OK;
    while (1)
    {
        /* 先看完成：数据收齐之后才出现的错误标志不影响这一批数据。 */
        if (s_is_adc_ready)
        {
            break;
        }
        if (s_is_adc_error)
        {
            result = BSP_ERR_HAL;
            break;
        }
        if ((uint32_t)(HAL_GetTick() - start_ms) >= timeout_ms)
        {
            result = BSP_ERR_TIMEOUT;
            break;
        }
    }
    HAL_StatusTypeDef stop_status = HAL_ADC_Stop_DMA(&hadc1);
    CLEAR_BIT(hadc1.Instance->CR2, ADC_CR2_CONT);
    if (stop_status != HAL_OK)
    {
        return BSP_ERR_HAL;
    }
    if (result != BSP_OK)
    {
        return result;
    }
    for (uint32_t k = 0U; k < count; k++)
    {
        for (uint32_t i = 0U; i < BSP_ADC_CHANNEL_COUNT; i++)
        {
            scans[k][i] =
                (uint16_t)s_dma_buffer[k * BSP_ADC_CHANNEL_COUNT + i];
        }
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
