/**
 * @file bsp_motor.h
 * @brief 本板八路电机的板级映射：把逻辑电机编号绑定到具体定时器通道。
 *
 * 本模块只维护“电机号 → 定时器/两路通道”的对应关系和八个 H 桥实例，不实现
 * PWM 逻辑，每个电机由一个 bsp_motor_hbridge_t 驱动。逻辑电机号与物理引脚的
 * 对应见 MCU 引脚分配文档；换板时只需修改 bsp_motor.c 中的映射表。
 *
 * @warning M6 的 FI/BI 与引脚自然顺序相反：FI=PC9/TIM8_CH4，BI=PC8/TIM8_CH3。
 *          第一次带电机联调前必须低占空比、单通道确认方向。
 */

#ifndef AITAID_BSP_MOTOR_H
#define AITAID_BSP_MOTOR_H

#include "bsp_common.h"
#include "bsp_motor_hbridge.h"

/**
 * @brief 电机逻辑编号；枚举数值可作为实例表下标。
 */
typedef enum
{
    BSP_MOTOR_M1 = 0, /**< PA2/PA3，TIM2_CH3/CH4。 */
    BSP_MOTOR_M2,     /**< PA0/PA1，TIM2_CH1/CH2。 */
    BSP_MOTOR_M3,     /**< PE5/PE6，TIM9_CH1/CH2。 */
    BSP_MOTOR_M4,     /**< PB14/PB15，TIM12_CH1/CH2。 */
    BSP_MOTOR_M5,     /**< PC6/PC7，TIM8_CH1/CH2。 */
    BSP_MOTOR_M6,     /**< PC9/PC8，TIM8_CH4/CH3（FI/BI 与引脚顺序相反）。 */
    BSP_MOTOR_M7,     /**< PB6/PB7，TIM4_CH1/CH2。 */
    BSP_MOTOR_M8,     /**< PB8/PB9，TIM4_CH3/CH4。 */
    BSP_MOTOR_COUNT
} bsp_motor_id_t;

/**
 * @brief 建立八路电机的实例并把全部输出置为滑行。
 *
 * @retval BSP_OK 八路全部绑定成功，或模块已经初始化。
 * @retval BSP_ERR_CONFIG 某一路的定时器未初始化，或 ARR 不符合要求。
 * @retval BSP_ERR_HAL 某一路启动 PWM 失败。
 *
 * @note 调用前必须完成 MX_TIM2/4/8/9/12_Init()。
 * @note 初始化期间所有电机保持停止（两路低电平），符合上电安全状态要求。
 */
bsp_status_t bsp_motor_init(void);

/**
 * @brief 设置一台电机的有符号占空比。
 *
 * @param[in] motor_id 电机逻辑编号，范围为 BSP_MOTOR_M1 至 BSP_MOTOR_M8。
 * @param[in] duty_permille 有符号占空比，M1～M6 幅值 500～1000；M7/M8 幅值 500～800；允许 0 停止。
 *                          正值正转，负值反转，0 为滑行。
 *
 * @retval BSP_OK 占空比已设置。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_PARAM 电机号或占空比超出范围。
 */
bsp_status_t bsp_motor_set_duty(bsp_motor_id_t motor_id, int16_t duty_permille);

/**
 * @brief 让一台电机滑行（两路低电平）。
 *
 * @param[in] motor_id 电机逻辑编号。
 *
 * @retval BSP_OK 已置为滑行。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_PARAM 电机号超出范围。
 */
bsp_status_t bsp_motor_coast(bsp_motor_id_t motor_id);

/**
 * @brief 对一台电机短路制动（两路 100%）。
 *
 * @param[in] motor_id 电机逻辑编号。
 *
 * @retval BSP_OK 已进入短路制动。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_PARAM 电机号超出范围。
 *
 * @warning 短路制动的电流和发热较大，使用前确认驱动器和电机可以承受。
 */
bsp_status_t bsp_motor_brake(bsp_motor_id_t motor_id);

/**
 * @brief 让全部八路电机滑行。
 *
 * @retval BSP_OK 全部成功。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval 其他 有任一路失败时返回第一个错误，但其余各路仍会继续处理。
 *
 * @note 用于急停或故障处理；单路失败不阻断其余电机停止。
 */
bsp_status_t bsp_motor_stop_all(void);

/**
 * @brief 急停：直接把八路电机的两路比较值清零，使其滑行。
 *
 * @note 供 Error_Handler() 和 HardFault 等异常处理函数使用：不检查初始化状态，
 *       不调用 HAL 函数，不改变驱动内部记录的占空比；可在任何上下文调用，
 *       包括 MX_TIMx_Init() 之前（此时什么都不做）。
 * @note 比较寄存器开启了预装载时，新值在下一个 PWM 周期生效（20kHz 下不超过
 *       50us）。
 * @note 只清比较值，不停止定时器，引脚保持确定的低电平而不是浮空。
 */
void bsp_motor_emergency_stop(void);

/**
 * @brief 取得某一路的底层 H 桥句柄，供需要直接操作时使用。
 *
 * @param[in] motor_id 电机逻辑编号。
 *
 * @return 指向该路 bsp_motor_hbridge_t 的指针；模块未初始化或编号无效时返回 NULL。
 */
bsp_motor_hbridge_t *bsp_motor_handle(bsp_motor_id_t motor_id);

#endif /* AITAID_BSP_MOTOR_H */
