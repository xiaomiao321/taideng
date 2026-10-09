/**
 * @file bsp_motor_hbridge.h
 * @brief 通用双输入 H 桥 PWM 驱动（handle 形式）。
 *
 * 本模块只认识“一个定时器句柄 + 正转通道 + 反转通道”，不知道接的是哪个
 * 电机，也不引用任何 CubeMX 全局句柄。每个 H 桥由调用者持有一个
 * bsp_motor_hbridge_t，因此同一个驱动可以创建任意多个互不干扰的实例。
 *
 * 适用于 RZ7899 这类“FI/BI 两路输入”的 H 桥：FI 有效为正转，BI 有效为
 * 反转，两路同时有效为短路制动，两路都无效为滑行。本模块只控制占空比，
 * 不含换向死区等待、斜坡加减速、堵转保护或电流检测。
 *
 * 本模块不修改 PSC、ARR、输出极性或 PWM 模式。所有接口由同一个主循环或
 * 任务串行调用，不可重入，不从 ISR 调用；不使用 HAL_Delay()，不输出日志。
 *
 * @note 头文件依赖 STM32 HAL 的 TIM_HandleTypeDef，适用范围是“STM32 + HAL”。
 * @note 对 TIM1/TIM8 这类高级定时器，启动任一通道会同时使能该定时器的主输出
 *       （MOE）。同一高级定时器上不要混用“需要保持关闭”的通道。
 */

#ifndef AITAID_BSP_MOTOR_HBRIDGE_H
#define AITAID_BSP_MOTOR_HBRIDGE_H

#include "bsp_common.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32f4xx_hal.h"

/** @brief 有符号占空比上限，+1000 为全速正转。 */
#define BSP_MOTOR_HBRIDGE_DUTY_MAX (1000)

/** @brief 有符号占空比下限，-1000 为全速反转。 */
#define BSP_MOTOR_HBRIDGE_DUTY_MIN (-1000)

/**
 * @brief 单个 H 桥句柄。
 *
 * 由调用者持有，初始化前应先清零；初始化后不应手工修改其字段。
 */
typedef struct
{
    TIM_HandleTypeDef *htim;    /**< 绑定的定时器实例。 */
    uint32_t channel_fi;        /**< 正转 PWM 通道（TIM_CHANNEL_x）。 */
    uint32_t channel_bi;        /**< 反转 PWM 通道（TIM_CHANNEL_x）。 */
    uint32_t arr;               /**< 初始化时缓存的 ARR，用于 CCR 换算。 */
    int16_t duty_permille;      /**< 最近一次设定的有符号占空比。 */
    bool is_initialized;        /**< 本实例是否已初始化。 */
} bsp_motor_hbridge_t;

/**
 * @brief 绑定一个 H 桥的两路 PWM，并把输出置为滑行（两路低电平）。
 *
 * @param[out] self 本 H 桥句柄，不允许为 NULL。
 * @param[in] htim 已初始化的定时器句柄，不允许为 NULL。
 * @param[in] channel_fi 正转 PWM 通道，TIM_CHANNEL_1 至 TIM_CHANNEL_4 之一。
 * @param[in] channel_bi 反转 PWM 通道，需与 channel_fi 不同。
 *
 * @retval BSP_OK 绑定成功，两路已启动且输出低电平，电机处于滑行状态。
 * @retval BSP_ERR_PARAM self 或 htim 为 NULL，通道无效，或两通道相同。
 * @retval BSP_ERR_CONFIG 定时器尚未初始化，或 ARR+1 不在 2 至 65536 内。
 * @retval BSP_ERR_HAL 启动 PWM 通道失败，本实例未初始化。
 *
 * @note 调用前必须完成对应 MX_TIMx_Init()，且两路通道需配置为向上计数、
 *       PWM mode 1、高电平有效、初始 Pulse=0。
 * @note 本函数把两路 CCR 清零后再启动，使引脚确定输出低电平而不是浮空。
 * @note 运行期间不得再修改该定时器的 PSC 或 ARR。
 */
bsp_status_t bsp_motor_hbridge_init(bsp_motor_hbridge_t *self,
                                    TIM_HandleTypeDef *htim,
                                    uint32_t channel_fi,
                                    uint32_t channel_bi);

/**
 * @brief 设置 H 桥的有符号占空比。
 *
 * @param[in,out] self 已初始化的 H 桥句柄，不允许为 NULL。
 * @param[in] duty_permille 有符号占空比，范围 -1000 至 +1000。
 *                          正值正转，负值反转，0 为滑行（两路低电平）。
 *
 * @retval BSP_OK 占空比已设置。
 * @retval BSP_ERR_PARAM self 为 NULL，或占空比超出范围。
 * @retval BSP_ERR_NOT_INITIALIZED 本实例尚未初始化。
 *
 * @note 换向时先把相反的通道清零，再给目标通道赋值，避免两路同时导通。
 * @note 中间没有死区等待；RZ7899 这类芯片自身有死区，若换成无死区的桥，
 *       需在换向前先调用 coast 并等待。
 * @note 依据初始化时缓存的 ARR 计算 CCR=(ARR+1)*|duty|/1000，
 *       使用 uint32_t 中间值。
 */
bsp_status_t bsp_motor_hbridge_set_duty(bsp_motor_hbridge_t *self,
                                        int16_t duty_permille);

/**
 * @brief 让 H 桥两路输出低电平，电机滑行。
 *
 * @param[in,out] self 已初始化的 H 桥句柄，不允许为 NULL。
 *
 * @retval BSP_OK 已置为滑行。
 * @retval BSP_ERR_PARAM self 为 NULL。
 * @retval BSP_ERR_NOT_INITIALIZED 本实例尚未初始化。
 *
 * @note 两路 CCR 清零；不停止 PWM 通道。可重复调用。
 */
bsp_status_t bsp_motor_hbridge_coast(bsp_motor_hbridge_t *self);

/**
 * @brief 让 H 桥两路 100% 输出，对电机短路制动。
 *
 * @param[in,out] self 已初始化的 H 桥句柄，不允许为 NULL。
 *
 * @retval BSP_OK 已进入短路制动。
 * @retval BSP_ERR_PARAM self 为 NULL。
 * @retval BSP_ERR_NOT_INITIALIZED 本实例尚未初始化。
 *
 * @warning 短路制动在高速或长时间保持时电流和发热较大，使用前确认驱动器
 *          和电机可以承受。
 */
bsp_status_t bsp_motor_hbridge_brake(bsp_motor_hbridge_t *self);

/**
 * @brief 读取最近一次设定的有符号占空比。
 *
 * @param[in] self 已初始化的 H 桥句柄，不允许为 NULL。
 * @param[out] duty_permille 接收最近一次设定值，不允许为 NULL。
 *
 * @retval BSP_OK 读取成功。
 * @retval BSP_ERR_PARAM self 或 duty_permille 为 NULL。
 * @retval BSP_ERR_NOT_INITIALIZED 本实例尚未初始化。
 *
 * @note 表示“最近一次指令”，调用 coast 或 brake 后该值为 0，不代表电机已经停稳。
 */
bsp_status_t bsp_motor_hbridge_get_duty(const bsp_motor_hbridge_t *self,
                                        int16_t *duty_permille);

/**
 * @brief 两路归零、停止 PWM 通道并解除绑定。
 *
 * @param[in,out] self 已初始化的 H 桥句柄，不允许为 NULL。
 *
 * @retval BSP_OK 已停止并解除绑定。
 * @retval BSP_ERR_PARAM self 为 NULL。
 * @retval BSP_ERR_NOT_INITIALIZED 本实例尚未初始化。
 * @retval BSP_ERR_HAL 停止 PWM 通道失败，本实例保持可用。
 */
bsp_status_t bsp_motor_hbridge_deinit(bsp_motor_hbridge_t *self);

#endif /* AITAID_BSP_MOTOR_HBRIDGE_H */
