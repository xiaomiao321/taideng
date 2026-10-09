/**
 * @file bsp_adc.h
 * @brief POT1 单路 ADC 的阻塞采样接口。
 *
 * 当前只读取 PC2 / ADC1_IN12，不提供六路扫描、滤波或角度换算。
 * 接口由同一个主循环或任务串行调用，不从 ISR 调用。
 * 本模块不输出日志，不使用 HAL_Delay()。
 */

#ifndef AITAID_BSP_ADC_H
#define AITAID_BSP_ADC_H

#include <stdint.h>

/**
 * @brief 12 位 ADC 原始读数的最大值。
 */
#define BSP_ADC_RAW_MAX (4095U)

/**
 * @brief 本阶段允许的最大转换等待时间，单位为毫秒。
 */
#define BSP_ADC_MAX_TIMEOUT_MS (1000U)

/**
 * @brief ADC 采样操作的返回状态。
 */
typedef enum
{
    BSP_ADC_STATUS_OK = 0,
    BSP_ADC_STATUS_NOT_INITIALIZED,
    BSP_ADC_STATUS_INVALID_ARGUMENT,
    BSP_ADC_STATUS_TIMEOUT,
    BSP_ADC_STATUS_ADC_ERROR
} bsp_adc_status_t;

/**
 * @brief 初始化 ADC 模块的软件状态。
 *
 * @note 调用前必须完成 MX_ADC1_Init()。本函数仅设置软件初始化标志，
 *       不重新配置 ADC，不启动转换；重复调用无额外硬件动作。
 * @note ADC1 必须为 12 位、右对齐、单路 Channel 12 / Rank 1、
 *       软件触发，关闭扫描和连续转换，EOC 为单次转换完成。
 * @note 初始化后 hadc1 由本模块独占，不与其他 ADC 读取代码交替调用。
 */
void bsp_adc_init(void);

/**
 * @brief 启动一次 POT1 转换，等待完成并读取原始值，然后停止 ADC。
 *
 * @param[out] raw_value 接收 0 至 BSP_ADC_RAW_MAX 的读数，不允许为 NULL。
 * @param[in] timeout_ms 等待转换完成的超时，范围为 1 至
 *                      BSP_ADC_MAX_TIMEOUT_MS，单位为毫秒。
 *
 * @retval BSP_ADC_STATUS_OK 采样及停止均成功，已写入 raw_value。
 * @retval BSP_ADC_STATUS_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ADC_STATUS_INVALID_ARGUMENT 指针为空或超时参数超出范围。
 * @retval BSP_ADC_STATUS_TIMEOUT 等待转换超时，且 ADC 已成功停止。
 * @retval BSP_ADC_STATUS_ADC_ERROR 启动、等待或停止 ADC 失败。
 *
 * @note 检查顺序为初始化、参数；检查失败时不操作 ADC。
 * @note 使用 HAL_ADC_Start()、HAL_ADC_PollForConversion()、
 *       HAL_ADC_GetValue() 和 HAL_ADC_Stop() 完成一次采样。
 * @note 启动成功后，无论等待成功、超时或失败，都必须尝试停止 ADC。
 *       停止失败优先返回 ADC_ERROR，即使此前发生了等待超时。
 * @note 先将读数保存在局部变量中，只有采样及停止均成功才写入输出参数。
 *       所有失败情况下 raw_value 保持不变，不保存调用者的指针。
 * @note 本函数会阻塞等待转换完成；timeout_ms 是等待转换的超时参数，
 *       不包含启动和停止操作，也不是两次采样之间的间隔。
 */
bsp_adc_status_t bsp_adc_read(uint16_t *raw_value, uint32_t timeout_ms);

#endif /* AITAID_BSP_ADC_H */
