/**
 * @file motor_limits.h
 * @brief 本板电机的占空比策略：哪些占空比允许输出。
 *
 * 电机统一用从 0 开始的下标表示：下标 0～5 为 M1～M6 小关节电机，
 * 下标 6、7 为 M7、M8 履带大电机，与 bsp_motor_id_t 的数值一致。
 * 文本协议里的 M1～M8 只在解析和打印处换算。
 *
 * @note 不依赖 HAL，可在 PC 端测试中直接包含。
 */

#ifndef AITAID_MOTOR_LIMITS_H
#define AITAID_MOTOR_LIMITS_H

#include <stdbool.h>

/** @brief 电机总数。 */
#define MOTOR_COUNT (8U)

/** @brief 小关节电机数量；下标小于该值的是小电机，其余是履带大电机。 */
#define MOTOR_SMALL_COUNT (6U)

/**
 * @brief 允许输出的最小占空比绝对值，千分比。
 *
 * 滑行 PWM 下低于约 50% 电机转不动；刹车 PWM 下能转的占空比更低，
 * 调试时需要扫低占空比，所以这里只挡掉明显无意义的值。
 */
#define MOTOR_DUTY_MIN (100)

/** @brief 小关节电机占空比绝对值上限，千分比。 */
#define MOTOR_DUTY_SMALL_MAX (1000)

/** @brief 履带大电机占空比绝对值上限，千分比。 */
#define MOTOR_DUTY_LARGE_MAX (800)

/**
 * @brief 判断某路电机能否输出给定占空比。
 *
 * @param[in] index 电机下标，0 至 MOTOR_COUNT - 1。
 * @param[in] duty 占空比千分比，正负表示方向。
 *
 * @retval true 允许：0（滑行/停止），或绝对值在 MOTOR_DUTY_MIN 到该电机上限之间。
 * @retval false 下标越界，或占空比不在允许范围内。
 */
static inline bool motor_duty_valid(unsigned index, int duty)
{
    if (index >= MOTOR_COUNT)
    {
        return false;
    }
    if (duty == 0)
    {
        return true; /* 滑行/停止总是允许 */
    }
    int max = (index < MOTOR_SMALL_COUNT) ? MOTOR_DUTY_SMALL_MAX
                                          : MOTOR_DUTY_LARGE_MAX;
    return ((duty >= MOTOR_DUTY_MIN) && (duty <= max)) ||
           ((duty <= -MOTOR_DUTY_MIN) && (duty >= -max));
}

#endif /* AITAID_MOTOR_LIMITS_H */
