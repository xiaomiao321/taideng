/**
 * @file joint_control.h
 * @brief 单关节位置闭环：电位器读数换算成角度，开关式驱动电机到目标角度。
 *
 * 角度单位为毫度。标定给出两点（raw_a, angle_a）与（raw_b, angle_b），
 * 读数与角度按直线换算；ADC 读数可以随角度递减。
 * 远离目标时持续驱动，接近目标时改为短脉冲点动，进入容差后保持一段时间
 * 判定到达；传感器失效、超时、越限、反向或停滞都会停机并报故障。
 *
 * 本模块不依赖 HAL，硬件动作通过 joint_hw_t 回调交给调用者，可在 PC 上测试。
 */

#ifndef AITAID_JOINT_CONTROL_H
#define AITAID_JOINT_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

/** @brief 关节数量，对应 M1～M6 和电位器 POT1～POT6。 */
#define JOINT_COUNT (6U)

/** @brief 电位器读数距上次采样超过该时间即视为失效，单位毫秒。 */
#define JOINT_SENSOR_STALE_MS (50U)

/** @brief 一个关节的标定参数。 */
typedef struct
{
    uint16_t raw_a;     /**< 标定点 A 的 ADC 读数。 */
    uint16_t raw_b;     /**< 标定点 B 的 ADC 读数。 */
    int32_t angle_a;    /**< 标定点 A 的角度，毫度；必须小于 angle_b。 */
    int32_t angle_b;    /**< 标定点 B 的角度，毫度。 */
    int direction;      /**< 电机正转（FI）使角度增大为 +1，减小为 -1。 */
    unsigned duty;      /**< 驱动占空比，千分比。 */
    unsigned tolerance; /**< 到达容差，毫度。 */
} joint_cal_t;

/** @brief 关节状态。 */
typedef enum
{
    JOINT_IDLE,
    JOINT_SETTLE, /**< 换向前停顿。 */
    JOINT_MOVING,
    JOINT_DONE,
    JOINT_FAULT,
    JOINT_PHASE_COUNT
} joint_phase_t;

/** @brief 单个关节的运行状态。 */
typedef struct
{
    joint_cal_t cal;
    bool calibrated;
    uint16_t raw;   /**< 最近一次 ADC 读数。 */
    int32_t angle;  /**< 最近一次换算角度，毫度。 */
    int32_t target; /**< 目标角度，毫度。 */
    int duty;       /**< 当前输出占空比。 */
    joint_phase_t phase;
    const char *fault; /**< 最近一次故障名，没有故障时为 "NONE"。 */
} joint_state_t;

/** @brief 调用者提供的硬件回调。 */
typedef struct
{
    /** @brief 驱动关节电机；index 为 0 起的关节下标。 */
    bool (*drive)(unsigned index, int duty);
    /** @brief 发送一行回复，内容已含 CRLF。 */
    void (*reply)(const char *line);
} joint_hw_t;

/** @brief 全部关节的控制状态。同一时刻最多一个关节在运动。调用者只读。 */
typedef struct
{
    joint_hw_t hw;
    joint_state_t states[JOINT_COUNT];
    int active;            /**< 正在运动的关节下标，-1 表示没有。 */
    int last_sign;         /**< 上一次驱动方向。 */
    unsigned seq;          /**< 启动运动的命令序号。 */
    uint32_t sample_ms;    /**< 最近一次有效采样的时刻。 */
    uint32_t start_ms;     /**< 运动开始时刻。 */
    uint32_t progress_ms;  /**< 最近一次误差明显减小的时刻。 */
    uint32_t stable_ms;    /**< 进入容差的时刻。 */
    uint32_t switch_ms;    /**< 最近一次换向的时刻。 */
    uint32_t pulse_ms;     /**< 点动脉冲当前段开始的时刻。 */
    uint32_t timeout_ms;   /**< 本次运动的超时。 */
    int32_t initial_error; /**< 开始时的误差绝对值。 */
    int32_t best_error;    /**< 运动中达到过的最小误差。 */
    bool valid;            /**< 最近一次采样是否有效。 */
    bool stable;           /**< 是否处于容差内。 */
    bool pulse_on;         /**< 点动脉冲是否处于驱动段。 */
    bool near_mode;        /**< 是否已进入接近目标的点动模式。 */
} joint_control_t;

/** @brief 初始化：全部关节未标定、空闲。 */
void joint_init(joint_control_t *j, joint_hw_t hw);

/** @brief 标定参数是否合法。 */
bool joint_cal_valid(const joint_cal_t *c);

/** @brief 按标定把 ADC 读数换算成角度，毫度。 */
int32_t joint_angle(const joint_cal_t *c, uint16_t raw);

/** @brief 写入标定；返回 NULL 表示成功，否则为错误名。 */
const char *joint_calibrate(joint_control_t *j, unsigned index, joint_cal_t c);

/** @brief 启动一次运动；返回 NULL 表示已接受，否则为错误名。 */
const char *joint_move(joint_control_t *j, unsigned seq, unsigned index,
                       int32_t target, uint32_t timeout, uint32_t now);

/**
 * @brief 输入一组采样。
 *
 * @param[in] raw JOINT_COUNT 个 ADC 读数。
 * @param[in] valid 本次采样是否成功；失败时不更新读数。
 */
void joint_feed(joint_control_t *j, const uint16_t *raw, bool valid,
                uint32_t now);

/**
 * @brief 周期调用：推进运动控制。
 *
 * @retval false 本次调用产生了新的控制故障（运动已停止）。
 */
bool joint_tick(joint_control_t *j, uint32_t now);

/** @brief 取消运动并停机；返回 false 表示停机时硬件失败。 */
bool joint_cancel(joint_control_t *j);

/** @brief 阶段的短名称，用于状态上报。 */
const char *joint_phase_name(joint_phase_t phase);

#endif /* AITAID_JOINT_CONTROL_H */
