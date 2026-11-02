/**
 * @file joint_traj.h
 * @brief 关节点到点轨迹：五次多项式，位置、速度、加速度全程连续。
 *
 * 终点速度和加速度为 0；起点可以带速度和加速度，用于运动中途换目标时从当前
 * 参考状态接着规划，衔接处同样连续。速度连续即没有刚性冲击，加速度连续即
 * 没有柔性冲击。
 *
 * 单位：位置毫度，速度毫度/秒，加速度毫度/秒²，时间毫秒。本模块是纯计算，
 * 不依赖 HAL，可在 PC 上测试。
 */

#ifndef AITAID_JOINT_TRAJ_H
#define AITAID_JOINT_TRAJ_H

#include <stdint.h>

/**
 * @brief 静止到静止的五次多项式，峰值速度为 D/T 的这个倍数。
 *
 * p = D·(10s³ − 15s⁴ + 6s⁵)，s = t/T，速度峰值在 s = 0.5 处。
 */
#define JOINT_TRAJ_PEAK_SPEED_GAIN (1.875f)

/** @brief 同一曲线的峰值加速度为 D/T² 的这个倍数，在 s = 0.5 ± √3/6 处。 */
#define JOINT_TRAJ_PEAK_ACCEL_GAIN (5.7735f)

/** @brief 一段轨迹。 */
typedef struct
{
    float p0;             /**< 起点位置。 */
    float v0;             /**< 起点速度。 */
    float a0;             /**< 起点加速度。 */
    float p1;             /**< 终点位置。 */
    float c3;             /**< 三次项系数。 */
    float c4;             /**< 四次项系数。 */
    float c5;             /**< 五次项系数。 */
    uint32_t duration_ms; /**< 时长。 */
} joint_traj_t;

/** @brief 轨迹上某一时刻的参考状态。 */
typedef struct
{
    float p; /**< 位置。 */
    float v; /**< 速度。 */
    float a; /**< 加速度。 */
} joint_traj_point_t;

/**
 * @brief 规划一段从 start 到 p1 的轨迹，终点速度和加速度为 0。
 *
 * 时长取以下三者的最大值，向上取整到毫秒：按 vmax 算的静止到静止时长
 * 1.875·|D|/vmax，按 amax 算的 √(5.7735·|D|/amax)，以及 min_ms。起点速度和
 * 加速度不为 0 时，峰值可能略超过 vmax、amax。
 *
 * @param[in] vmax 速度上限，必须为正。
 * @param[in] amax 加速度上限，必须为正。
 * @param[in] min_ms 最短时长，必须为正。
 */
void joint_traj_plan(joint_traj_t *t, joint_traj_point_t start, float p1,
                     float vmax, float amax, uint32_t min_ms);

/**
 * @brief 求轨迹开始后 elapsed_ms 时刻的参考状态。
 *
 * @note 超过时长后停在终点，速度和加速度为 0。
 */
joint_traj_point_t joint_traj_eval(const joint_traj_t *t, float elapsed_ms);

#endif /* AITAID_JOINT_TRAJ_H */
