/**
 * @file bsp_light.h
 * @brief 本板七路灯板的板级映射：把逻辑灯编号绑定到具体定时器通道。
 *
 * 本模块只维护“灯号 → 定时器/通道”的对应关系和七路实例，不实现 PWM 逻辑，
 * 每路灯由一个 bsp_light_pwm_t 驱动。逻辑灯号与物理引脚的对应见 MCU 引脚分配
 * 文档；换板时只需修改 bsp_light.c 中的映射表，不需要改动 bsp_light_pwm。
 */

#ifndef AITAID_BSP_LIGHT_H
#define AITAID_BSP_LIGHT_H

#include "bsp_common.h"
#include "bsp_light_pwm.h"

/**
 * @brief 灯板逻辑编号；枚举数值可作为实例表下标。
 */
typedef enum
{
    BSP_LIGHT_UP1 = 0, /**< 上灯板 1，PE9，TIM1_CH1。 */
    BSP_LIGHT_UP2,     /**< 上灯板 2，PE11，TIM1_CH2。 */
    BSP_LIGHT_UP3,     /**< 上灯板 3，PE14，TIM1_CH4。 */
    BSP_LIGHT_UP4,     /**< 上灯板 4，PE13，TIM1_CH3。 */
    BSP_LIGHT_DOWN1,   /**< 下灯板 1，PB0，TIM3_CH3。 */
    BSP_LIGHT_DOWN2,   /**< 下灯板 2，PB1，TIM3_CH4。 */
    BSP_LIGHT_DOWN3,   /**< 下灯板 3，PB4，TIM3_CH1。 */
    BSP_LIGHT_COUNT
} bsp_light_id_t;

/**
 * @brief 建立七路灯的实例并校验对应定时器。
 *
 * @retval BSP_OK 七路全部绑定成功，或模块已经初始化。
 * @retval BSP_ERR_CONFIG 某一路的定时器未初始化，或 ARR 不符合要求。
 *
 * @note 调用前必须完成 MX_TIM1_Init() 和 MX_TIM3_Init()。
 * @note 本函数只建立绑定，不点亮任何灯，也不启动 PWM 通道。
 */
bsp_status_t bsp_light_init(void);

/**
 * @brief 设置一盏灯的高电平占空比，首次设置时启动该路 PWM。
 *
 * @param[in] light_id 灯板逻辑编号，范围为 BSP_LIGHT_UP1 至 BSP_LIGHT_DOWN3。
 * @param[in] duty_permille 占空比千分比，范围为 0 至 BSP_LIGHT_PWM_DUTY_MAX。
 *
 * @retval BSP_OK 占空比已设置。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_PARAM 灯号或占空比超出范围。
 * @retval BSP_ERR_HAL 首次启动 PWM 失败。
 */
bsp_status_t bsp_light_set_duty(bsp_light_id_t light_id,
                                uint16_t duty_permille);

/**
 * @brief 将一盏灯的占空比设为零。
 *
 * @param[in] light_id 灯板逻辑编号。
 *
 * @retval BSP_OK 已置零，或该路尚未启动。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_PARAM 灯号超出范围。
 */
bsp_status_t bsp_light_stop(bsp_light_id_t light_id);

/**
 * @brief 取得某一路的底层通道句柄，供需要直接操作时使用。
 *
 * @param[in] light_id 灯板逻辑编号。
 *
 * @return 指向该路 bsp_light_pwm_t 的指针；模块未初始化或灯号无效时返回 NULL。
 */
bsp_light_pwm_t *bsp_light_handle(bsp_light_id_t light_id);

#endif /* AITAID_BSP_LIGHT_H */
