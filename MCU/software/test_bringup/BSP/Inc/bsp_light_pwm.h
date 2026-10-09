/**
 * @file bsp_light_pwm.h
 * @brief 单路 PWM 输出的通用驱动（handle 形式）。
 *
 * 本模块只认识“一个定时器句柄 + 一个通道”，不知道它接的是哪盏灯，
 * 也不引用任何 CubeMX 全局句柄。每路通道由调用者持有一个
 * bsp_light_pwm_t，因此同一个驱动可以创建任意多个互不干扰的实例。
 *
 * 板级映射（哪盏灯用哪个定时器/通道）放在 bsp_light.h，不在本模块内。
 * 本模块不修改 PSC、ARR、输出极性或 PWM 模式，只读写 CCR 并启停通道。
 * 所有接口由同一个主循环或任务串行调用，不可重入，不从 ISR 调用；
 * 不使用 HAL_Delay()，不输出日志。
 *
 * @note 头文件依赖 STM32 HAL 的 TIM_HandleTypeDef，因此本驱动的适用范围是
 *       “STM32 + HAL”。换系列或换实例时，只需修改调用处传入的句柄与通道。
 */

#ifndef AITAID_BSP_LIGHT_PWM_H
#define AITAID_BSP_LIGHT_PWM_H

#include "bsp_common.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32f4xx_hal.h"

/**
 * @brief 占空比刻度上限，1000 表示 100%，250 表示 25%。
 */
#define BSP_LIGHT_PWM_DUTY_MAX (1000U)

/**
 * @brief 单路 PWM 通道句柄。
 *
 * 由调用者持有，初始化前应先清零；初始化后不应手工修改其字段。
 */
typedef struct
{
    TIM_HandleTypeDef *htim;    /**< 绑定的定时器实例。 */
    uint32_t channel;           /**< TIM_CHANNEL_x。 */
    uint32_t arr;               /**< 初始化时缓存的 ARR，用于 CCR 换算。 */
    bool is_initialized;        /**< 本实例是否已初始化。 */
    bool is_started;            /**< 本通道是否已启动 PWM 输出。 */
} bsp_light_pwm_t;

/**
 * @brief 绑定一个定时器通道并缓存其 ARR，不启动 PWM 输出。
 *
 * @param[out] self 本通道句柄，不允许为 NULL。
 * @param[in] htim 已初始化的定时器句柄，不允许为 NULL。
 * @param[in] channel TIM_CHANNEL_1 至 TIM_CHANNEL_4 之一。
 *
 * @retval BSP_OK 绑定成功，可继续调用 set_duty 或 stop。
 * @retval BSP_ERR_PARAM self 或 htim 为 NULL，或通道编号无效。
 * @retval BSP_ERR_CONFIG 定时器尚未初始化，或 ARR+1 不在 2 至 65536 内。
 *
 * @note 调用前必须完成对应 MX_TIMx_Init()，且通道需配置为向上计数、
 *       PWM mode 1、高电平有效、初始 Pulse=0。
 * @note 检查 ARR+1 在 2 至 65536 内，使 100% 对应的 CCR=ARR+1 能放入
 *       16 位比较寄存器。运行期间不得再修改该定时器的 PSC 或 ARR。
 * @note 本函数不写 CCR、不启动通道；重复调用会重新绑定。
 */
bsp_status_t bsp_light_pwm_init(bsp_light_pwm_t *self,
                                TIM_HandleTypeDef *htim,
                                uint32_t channel);

/**
 * @brief 设置本路高电平占空比，首次设置时启动该 PWM 通道。
 *
 * @param[in,out] self 已初始化的通道句柄，不允许为 NULL。
 * @param[in] duty_permille 占空比千分比，范围为 0 至 BSP_LIGHT_PWM_DUTY_MAX。
 *                          0 为持续低电平，1000 为持续高电平。
 *
 * @retval BSP_OK 占空比已设置；首次设置时通道已启动。
 * @retval BSP_ERR_PARAM self 为 NULL 或占空比超出范围。
 * @retval BSP_ERR_NOT_INITIALIZED 本实例尚未初始化。
 * @retval BSP_ERR_HAL 首次启动 PWM 通道失败。
 *
 * @note 依据初始化时缓存的 ARR 计算 CCR=(ARR+1)*duty_permille/1000，
 *       使用 uint32_t 中间值；不要写成 ARR*duty_permille/1000。
 * @note 首次调用先写 CCR，再调用 HAL_TIM_PWM_Start()，只有启动成功才置本
 *       实例的启动标志；失败时把 CCR 恢复为 0 并返回 BSP_ERR_HAL。
 * @note 已启动时仅更新 CCR，不重复调用 HAL_TIM_PWM_Start()。
 *       同一个定时器的多个通道共用频率，占空比各自独立。
 */
bsp_status_t bsp_light_pwm_set_duty(bsp_light_pwm_t *self,
                                    uint16_t duty_permille);

/**
 * @brief 将本路占空比设为零，保持已启动通道输出低电平。
 *
 * @param[in,out] self 已初始化的通道句柄，不允许为 NULL。
 *
 * @retval BSP_OK 已置零，或本通道尚未启动。
 * @retval BSP_ERR_PARAM self 为 NULL。
 * @retval BSP_ERR_NOT_INITIALIZED 本实例尚未初始化。
 *
 * @note 只写 CCR=0，不调用 HAL_TIM_PWM_Stop()，不改变启动标志，
 *       也不影响同一定时器的其他通道。
 * @note 本函数表示停止 PWM 高电平脉冲；灯板是否熄灭取决于其输入有效极性。
 */
bsp_status_t bsp_light_pwm_stop(bsp_light_pwm_t *self);

/**
 * @brief 停止本路 PWM 输出并解除绑定。
 *
 * @param[in,out] self 已初始化的通道句柄，不允许为 NULL。
 *
 * @retval BSP_OK 已停止并解除绑定。
 * @retval BSP_ERR_PARAM self 为 NULL。
 * @retval BSP_ERR_NOT_INITIALIZED 本实例尚未初始化。
 * @retval BSP_ERR_HAL 停止 PWM 通道失败，本实例保持可用。
 *
 * @note 已启动时调用 HAL_TIM_PWM_Stop()，随后把 CCR 置零并清除初始化标志。
 *       不影响同一定时器的其他通道。
 */
bsp_status_t bsp_light_pwm_deinit(bsp_light_pwm_t *self);

#endif /* AITAID_BSP_LIGHT_PWM_H */
