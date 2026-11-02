/**
 * @file chassis.h
 * @brief 底盘：把 SET_MOTION 的整机动作映射到左右两条履带。
 *
 * 开环控制，没有编码器：速度百分比直接当占空比百分比，前进、后退两路同向，
 * 左转、右转两路反向原地转。实际状态即最近一次成功应用的动作和速度。
 * 驱动调用失败时两路都停下并锁存底盘故障，STOP 时再试着停一次，成功才清除。
 *
 * @note 不依赖 HAL，电机输出经回调交出去，可在 PC 端测试。
 */

#ifndef AITAID_CHASSIS_H
#define AITAID_CHASSIS_H

#include <stdbool.h>
#include <stdint.h>

/** @brief 底盘故障位，与协议 fault_flags 的 bit6 一致。 */
#define CHASSIS_FAULT (0x40U)

typedef struct
{
    /** @brief 设置一台电机的有符号占空比千分比，0 为滑行；返回是否成功。 */
    bool (*drive)(unsigned motor, int duty);
    uint8_t motion; /**< 当前实际动作，见 MCU_PROTOCOL_MOTION_*。 */
    uint8_t speed;  /**< 当前实际速度百分比，STOP 时为 0。 */
    bool fault;     /**< 是否锁存了底盘故障。 */
} chassis_t;

/** @brief 初始化为停止、无故障；不驱动电机。 */
void chassis_init(chassis_t *c, bool (*drive)(unsigned motor, int duty));

/**
 * @brief 速度百分比换算成履带占空比千分比。
 *
 * 0 为 0；其余按百分比换算，低于 MOTOR_DUTY_MIN 抬到它，高于履带上限截断。
 */
int chassis_duty(uint8_t speed);

/**
 * @brief 应用一个动作，参数须已由命令层校验。
 *
 * @retval true 两路都已设置。
 * @retval false 有一路失败：两路都停下，状态回到 STOP，锁存底盘故障。
 */
bool chassis_apply(chassis_t *c, uint8_t motion, uint8_t speed);

/** @brief 两路滑行，状态回到 STOP；不清故障。返回是否两路都成功。 */
bool chassis_stop(chassis_t *c);

/** @brief STOP 时调用：再停一次，两路都成功才清除底盘故障。 */
void chassis_clear_fault(chassis_t *c);

/** @brief 锁存的故障位，无故障为 0。 */
uint8_t chassis_fault_flags(const chassis_t *c);

#endif /* AITAID_CHASSIS_H */
