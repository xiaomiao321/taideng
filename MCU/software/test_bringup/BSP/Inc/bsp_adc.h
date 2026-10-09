/**
 * @file bsp_adc.h
 * @brief 电位器 ADC 的阻塞采样接口。
 *
 * 单路版本使用轮询，六路版本使用扫描及 DMA；两者依赖不同的 CubeMX 配置。
 * 当前不提供滤波或角度换算。
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
 * @brief 一组扫描包含的电位器数量。
 */
#define BSP_ADC_CHANNEL_COUNT (6U)

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
 * @note 调用前必须完成 MX_ADC1_Init()；六路版本还必须提前执行 MX_DMA_Init()。
 *       本函数仅设置软件初始化标志，
 *       不重新配置 ADC，不启动转换；重复调用无额外硬件动作。
 * @note ADC1 的具体配置见所使用的 read 接口；初始化不切换采样模式。
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
 * @note 仅用于单路配置：12 位、右对齐、Channel 12 / Rank 1、软件触发，
 *       关闭扫描和连续转换，EOC 为单次转换完成。
 *       切换到六路 DMA 配置后，不得调用本接口读取 POT1。
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

/**
 * @brief 启动一次六路扫描，等待 DMA 收齐一组数据，然后停止 ADC/DMA。
 *
 * @param[out] raw_values 接收六路原始值的数组，不允许为 NULL。
 *                       调用者必须提供至少 BSP_ADC_CHANNEL_COUNT 个 uint16_t。
 *                       下标 0 至 5 分别对应 POT1 至 POT6。
 * @param[in] timeout_ms 等待整组数据的超时，范围为 1 至
 *                      BSP_ADC_MAX_TIMEOUT_MS，单位为毫秒。
 *
 * @retval BSP_ADC_STATUS_OK 整组采样及停止均成功，已写入六个原始值。
 * @retval BSP_ADC_STATUS_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ADC_STATUS_INVALID_ARGUMENT 数组指针为空或超时参数超出范围。
 * @retval BSP_ADC_STATUS_TIMEOUT 等待整组数据超时，且 ADC/DMA 已成功停止。
 * @retval BSP_ADC_STATUS_ADC_ERROR ADC/DMA 启动、采样或停止失败。
 *
 * @note 仅用于六路配置：12 位、右对齐、扫描开启、连续转换关闭、软件触发，
 *       转换数量为 6，EOC 为序列完成，DMA Continuous Requests 关闭。
 *       Rank 1 至 6 的通道依次为 12、11、10、13、14、15。
 * @note DMA 使用 Normal 模式，外设地址不递增、内存地址递增，
 *       外设和内存数据宽度均为 Word；DMA 目标为模块私有的
 *       static uint32_t 数组，包含六个元素，位于 DMA 可访问的普通 SRAM。
 *       HAL_ADC_Start_DMA() 的 Length 传 6，表示元素数，不是字节数。
 * @note 检查顺序为初始化、参数；检查失败时不操作 ADC/DMA。
 *       启动前清除本次完成及错误标志，再调用 HAL_ADC_Start_DMA()。
 * @note HAL_ADC_ConvCpltCallback() 和 HAL_ADC_ErrorCallback() 在确认
 *       是 ADC1 后仅更新 volatile 标志，不打印日志，也不复制输出数据。
 *       只处理整组完成，不把半传输完成当作成功。
 * @note 主循环等待完成、错误或超时，不用 HAL_ADC_PollForConversion()
 *       判断 DMA 完成。经过时间使用 HAL_GetTick() 的无符号差值。
 * @note 启动成功后，无论完成、错误或超时，都必须调用 HAL_ADC_Stop_DMA()。
 *       停止失败优先返回 ADC_ERROR。错误标志优先于完成和超时标志。
 * @note 整组采样及停止均成功后，才将内部六个读数转换为 uint16_t 并
 *       复制到 raw_values；所有失败情况下输出数组保持不变。
 *       DMA 不直接写调用者的数组，模块不保存调用者的指针。
 * @note 本函数阻塞等待，依赖 DMA 和 ADC 中断正常运行；不得在关闭中断
 *       的区域或 ISR 内调用。timeout_ms 不是采样间隔。
 */
bsp_adc_status_t bsp_adc_read_all(uint16_t raw_values[BSP_ADC_CHANNEL_COUNT],
                                  uint32_t timeout_ms);

#endif /* AITAID_BSP_ADC_H */
