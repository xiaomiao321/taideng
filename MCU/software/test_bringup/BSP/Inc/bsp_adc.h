/**
 * @file bsp_adc.h
 * @brief 电位器 ADC 的阻塞采样接口。
 *
 * 六路扫描加 DMA，依赖 CubeMX 中 ADC1 的六路扫描配置。
 * 当前不提供滤波或角度换算。
 * 接口由同一个主循环或任务串行调用，不从 ISR 调用。
 * 本模块不输出日志，不使用 HAL_Delay()。
 */

#ifndef AITAID_BSP_ADC_H
#define AITAID_BSP_ADC_H

#include "bsp_common.h"

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
 * @brief 初始化 ADC 模块的软件状态，并校验外设配置是否符合驱动假设。
 *
 * @retval BSP_OK 配置符合六路 DMA 扫描，模块已初始化。
 * @retval BSP_ERR_CONFIG ADC1 句柄或配置不符。
 *
 * @note 调用前必须完成 MX_DMA_Init() 和 MX_ADC1_Init()。
 *       本函数不重新配置 ADC，不启动转换；重复调用无额外硬件动作。
 * @note 校验只读取 HAL 句柄中 CubeMX 写入的配置，不修改任何寄存器。
 * @note 要求：12 位、右对齐、扫描开启、连续转换关闭、软件触发，
 *       转换数量为 BSP_ADC_CHANNEL_COUNT，EOC 为序列完成，
 *       DMA Continuous Requests 关闭，且 hadc1.DMA_Handle 已绑定 Normal、
 *       内存递增、外设不递增的句柄。
 * @note 校验通过后 hadc1 由本模块独占，不与其他 ADC 读取代码交替调用。
 */
bsp_status_t bsp_adc_init(void);

/**
 * @brief 启动一次六路扫描，等待 DMA 收齐一组数据，然后停止 ADC/DMA。
 *
 * @param[out] raw_values 接收六路原始值的数组，不允许为 NULL。
 *                       调用者必须提供至少 BSP_ADC_CHANNEL_COUNT 个 uint16_t。
 *                       下标 0 至 5 分别对应 POT1 至 POT6。
 * @param[in] timeout_ms 等待整组数据的超时，范围为 1 至
 *                      BSP_ADC_MAX_TIMEOUT_MS，单位为毫秒。
 *
 * @retval BSP_OK 整组采样及停止均成功，已写入六个原始值。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_PARAM 数组指针为空或超时参数超出范围。
 * @retval BSP_ERR_TIMEOUT 等待整组数据超时，且 ADC/DMA 已成功停止。
 * @retval BSP_ERR_HAL ADC/DMA 启动、采样或停止失败。
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
 * @note 本模块不定义 HAL 回调。HAL_ADC_ConvCpltCallback() 与
 *       HAL_ADC_ErrorCallback() 由集成层（hal_callback_dispatch.c）在确认实例为
 *       ADC1 后转发到 bsp_adc_on_conv_cplt_from_isr() 和
 *       bsp_adc_on_error_from_isr()；两者只更新 volatile 标志，
 *       不打印日志，也不复制输出数据。
 *       只处理整组完成，不把半传输完成当作成功。
 * @note 主循环等待完成、错误或超时，不用 HAL_ADC_PollForConversion()
 *       判断 DMA 完成。经过时间使用 HAL_GetTick() 的无符号差值。
 * @note 启动成功后，无论完成、错误或超时，都必须调用 HAL_ADC_Stop_DMA()。
 *       停止失败优先返回 BSP_ERR_HAL。错误标志优先于完成和超时标志。
 * @note 整组采样及停止均成功后，才将内部六个读数转换为 uint16_t 并
 *       复制到 raw_values；所有失败情况下输出数组保持不变。
 *       DMA 不直接写调用者的数组，模块不保存调用者的指针。
 * @note 本函数阻塞等待，依赖 DMA 和 ADC 中断正常运行；不得在关闭中断
 *       的区域或 ISR 内调用。timeout_ms 不是采样间隔。
 */
bsp_status_t bsp_adc_read_all(uint16_t raw_values[BSP_ADC_CHANNEL_COUNT],
                              uint32_t timeout_ms);

/**
 * @brief 记录一次 ADC1 整组转换完成事件，由 HAL 回调在集成层转发。
 *
 * @note 仅设置内部的完成标志，供正在等待的 bsp_adc_read_all() 使用。
 * @warning 只能在中断上下文调用，且调用前已确认对应外设为 ADC1。
 * @warning 本函数不处理半传输完成；半传输事件不得调用本函数。
 */
void bsp_adc_on_conv_cplt_from_isr(void);

/**
 * @brief 记录一次 ADC1 转换错误事件，由 HAL 回调在集成层转发。
 *
 * @note 仅设置内部的错误标志，供 bsp_adc_read_all() 在下次轮询时返回错误。
 * @warning 只能在中断上下文调用，且调用前已确认对应外设为 ADC1。
 */
void bsp_adc_on_error_from_isr(void);

#endif /* AITAID_BSP_ADC_H */
