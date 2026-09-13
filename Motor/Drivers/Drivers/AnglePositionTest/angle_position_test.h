/**
  ******************************************************************************
  * @file    angle_position_test.h
  * @brief   角度闭环定位联调案例 (RZ7899 电机驱动 + PotAngle 角度反馈)
  *
  * 用途: 把电机输出轴依次转动到一串目标角度并停稳, 用于验证
  *       "电位器角度传感器 + 电机驱动" 的闭环联调效果 (如 30→60→...→260°).
  *
  * 每个控制周期做这些事:
  *   1. 调用 PotAngle_Update() 刷新角度反馈;
  *   2. 误差 = 当前目标 - 实测角度;
  *   3. 远距用 speed_far, 近距 (<= near_deg) 用 speed_near 两段逼近;
  *   4. 进入容差 (tol_deg) 后 RZ7899_Stop(BRAKE) 制动, 停稳 dwell_ms
  *      后自动进入下一个目标; 序列跑完保持在最后角度 (done=1);
  *   5. 保护: 连续 stall_ms 误差无进展 -> 停机并锁定 fault=1
  *      (调用 AnglePositionTest_Restart() 恢复).
  *
  * 使用前提:
  *   - RZ7899_Init() 已调用, 且 TIM2 更新中断在驱动 RZ7899_Update();
  *   - PotAngle_Init() 已调用 (PA4 电位器角度反馈);
  *   - RZ7899_SetDutyLimits() 已按电机标定值设置 (如履带电机 540/1000);
  *   - 建议 ~1kHz 调用 AnglePositionTest_Update() (间隔 1ms).
  *
  * ⚠️ 首次调试注意:
  *   - 上电即开始动作, 保持随时可断电;
  *   - 若电机朝远离目标的方向转 (误差越来越大): 把 dir_sign 改成 -1
  *     (或把电机两根线对调); 方向不对时看门狗会在 stall_ms 后停机;
  *   - 电位器有机械硬限位 (270°), 确认目标角度都在其行程内.
  ******************************************************************************
  */

#ifndef ANGLE_POSITION_TEST_H
#define ANGLE_POSITION_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "rz7899.h"   /* 电机驱动库 (Drivers/RZ7899) */
#include "pot_angle.h" /* 角度反馈库 (Drivers/PotAngle) */

/* ---------------- 默认参数 (可用宏改默认值, 也可运行时改句柄字段) ---------------- */

/** @brief 到位容差 (°): 误差 <= 该值即制动停机 */
#ifndef ANGLE_POS_TEST_TOL_DEG
#define ANGLE_POS_TEST_TOL_DEG 2U
#endif

/** @brief 近距阈值 (°): 距离 <= 该值时改用 speed_near 慢速逼近 */
#ifndef ANGLE_POS_TEST_NEAR_DEG
#define ANGLE_POS_TEST_NEAR_DEG 20U
#endif

/** @brief 远距逼近速度刻度 (0 ~ 1000, 经 min/max 映射成实际占空比) */
#ifndef ANGLE_POS_TEST_SPEED_FAR
#define ANGLE_POS_TEST_SPEED_FAR 400
#endif

/** @brief 近距逼近速度刻度 (太慢可能带不动, 带不动会被看门狗判卡死) */
#ifndef ANGLE_POS_TEST_SPEED_NEAR
#define ANGLE_POS_TEST_SPEED_NEAR 150
#endif

/** @brief 方向符号 (+1 / -1): 电机"正转"使角度减小时改成 -1 */
#ifndef ANGLE_POS_TEST_DIR_SIGN
#define ANGLE_POS_TEST_DIR_SIGN (+1)
#endif

/** @brief 卡死看门狗 (ms): 连续这么长时间误差无进展 -> 停机锁定 fault */
#ifndef ANGLE_POS_TEST_STALL_MS
#define ANGLE_POS_TEST_STALL_MS 3000U
#endif

/** @brief 每步停稳时间 (ms): 到位制动后停留这么久再走下一个目标 */
#ifndef ANGLE_POS_TEST_DWELL_MS
#define ANGLE_POS_TEST_DWELL_MS 1000U
#endif

/** @brief 联调实例句柄: 一个句柄对应 "一个电机 + 一个角度反馈" */
typedef struct
{
    RZ7899_Handle *motor; /**< 电机句柄 (由调用者拥有, 如 &motor1) */
    PotAngle_Handle *pot; /**< 角度反馈句柄 (由调用者拥有, 如 &pot1) */

    /* ---- 参数 (运行时可改, 大部分下一拍生效) ---- */
    const uint16_t *seq; /**< 目标序列数组 (°), 默认内置 {30,60,90,120,150,200,260} */
    uint16_t seq_count;  /**< 序列长度 */

    uint16_t tol_deg;    /**< 到位容差 (°) */
    uint16_t near_deg;   /**< 近距阈值 (°) */
    int16_t speed_far;   /**< 远距速度刻度 */
    int16_t speed_near;  /**< 近距速度刻度 */
    int8_t dir_sign;     /**< 方向符号 (+1 / -1) */
    uint32_t stall_ms;   /**< 卡死看门狗 (ms) */
    uint32_t dwell_ms;   /**< 每步停稳时间 (ms) */

    /* ---- 运行状态 / 观测 (Ozone Watch 这些字段即可) ---- */
    volatile uint8_t step;        /**< 当前步骤号 (1 ~ seq_count); 完成时 = seq_count */
    volatile uint8_t done;        /**< 1 = 序列全部完成, 保持在最后角度 */
    volatile uint8_t fault;       /**< 1 = 看门狗触发已停机 (Restart 恢复) */
    volatile uint16_t target_deg; /**< 当前目标角度 (°) */
    volatile int16_t err_deg;     /**< 误差 = 目标 - 实测 (°) */
    volatile int16_t cmd;         /**< 当前速度指令 (0 = 已停稳) */

    /* ---- 内部状态 (无需关心) ---- */
    uint8_t index;      /**< 当前步骤索引 (0 起) */
    uint32_t dwell_cnt; /**< 到位停稳计时 */
    uint32_t stall_cnt; /**< 无进展计时 */
    int32_t best_err;   /**< 历史最小误差 (进展判定) */
} AnglePositionTest_Handle;

/* -------------------------------------------------------------------------- */
/* 接口                                                                        */
/* -------------------------------------------------------------------------- */

/**
  * @brief  初始化联调案例: 绑定电机与角度反馈, 装载默认参数与默认序列
  * @param  test  联调句柄
  * @param  motor 电机句柄 (需已 RZ7899_Init 并设置好占空比映射)
  * @param  pot   角度反馈句柄 (需已 PotAngle_Init)
  * @retval true  成功
  * @retval false 参数为空
  * @note   内部会调用 AnglePositionTest_Restart() (制动电机、状态归零);
  *         之后按 ~1kHz 调 AnglePositionTest_Update() 即自动跑序列.
  */
bool AnglePositionTest_Init(AnglePositionTest_Handle *test,
                            RZ7899_Handle *motor,
                            PotAngle_Handle *pot);

/**
  * @brief  推进一个控制周期 (内部含 PotAngle_Update 采样, 建议 1kHz 调用)
  * @param  test 联调句柄 (需已 AnglePositionTest_Init)
  * @note   只写观测字段与速度指令, 不含延时; 完成/故障状态见 done / fault.
  */
void AnglePositionTest_Update(AnglePositionTest_Handle *test);

/**
  * @brief  替换目标序列并从头开始跑
  * @param  test      联调句柄 (需已 Init)
  * @param  seq       新序列数组 (°); 数组须在重跑期间一直有效 (静态/全局)
  * @param  seq_count 序列长度 (建议 <= 255)
  * @note   等价于 "SetSequence + Restart": 会制动电机并从第 1 个目标重跑.
  */
void AnglePositionTest_SetSequence(AnglePositionTest_Handle *test,
                                   const uint16_t *seq,
                                   uint16_t seq_count);

/**
  * @brief  重新开始: 制动电机、清除完成/故障标志、从序列第 1 个目标重跑
  * @param  test 联调句柄
  * @note   看门狗触发 (fault=1) 后, 排除问题 (方向/卡死) 再调用本函数恢复.
  */
void AnglePositionTest_Restart(AnglePositionTest_Handle *test);

#ifdef __cplusplus
}
#endif

#endif /* ANGLE_POSITION_TEST_H */
