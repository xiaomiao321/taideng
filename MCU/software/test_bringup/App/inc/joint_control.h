/**
 * @file joint_control.h
 * @brief 单关节位置闭环：电位器读数换算成角度，开关式驱动电机到目标角度。
 *
 * 角度单位为毫度。标定给出两点（raw_a, angle_a）与（raw_b, angle_b），
 * 读数与角度按直线换算；ADC 读数可以随角度递减。两点可以实测（CAL），
 * 也可以由电位器理论参数加一个实测参考点算出（joint_cal_from_nominal）。
 * 允许运动的范围 [limit_min, limit_max] 与标定点分开设置，
 * 应按机构实际能转的范围设定，避免顶到机械限位。
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

/** @brief 电位器标称电气行程，毫度。 */
#define JOINT_POT_TRAVEL_MDEG (270000)

/** @brief 电位器阻值未知时采用的标称值，欧姆。 */
#define JOINT_POT_DEFAULT_OHMS (45000U)

/** @brief 电位器两端各串的固定电阻（3V3-1k-[电位器]-1k-GND），欧姆。 */
#define JOINT_POT_END_OHMS (1000U)

/** @brief 理论标定允许的电位器阻值范围，欧姆。 */
#define JOINT_POT_MIN_OHMS (1000U)
#define JOINT_POT_MAX_OHMS (1000000U)

/** @brief 标定数据序列化格式的版本；格式变化时加一，旧数据不再加载。 */
#define JOINT_CAL_BLOB_VERSION (2U)

/** @brief 序列化后每个关节占用的字节数。 */
#define JOINT_CAL_BLOB_ENTRY_SIZE (28U)

/** @brief 序列化后的总字节数：4 字节头加每个关节一项，是 4 的倍数。 */
#define JOINT_CAL_BLOB_SIZE (4U + JOINT_COUNT * JOINT_CAL_BLOB_ENTRY_SIZE)

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
    int32_t limit_min;  /**< 允许运动范围下限，毫度。 */
    int32_t limit_max;  /**< 允许运动范围上限，毫度。 */
} joint_cal_t;

/**
 * @brief 理论标定的输入：电位器标称参数加一个实测参考点。
 *
 * 电位器 1:1 装在关节轴上，转过 JOINT_POT_TRAVEL_MDEG 时读数从一端走到
 * 另一端；两端读数由分压电路和阻值算出，参考点确定零位偏移。
 */
typedef struct
{
    uint16_t raw_ref;     /**< 参考位置的 ADC 读数。 */
    int32_t angle_ref;    /**< 参考位置的关节角度，毫度。 */
    int sensor_direction; /**< 角度增大时读数增大为 +1，减小为 -1。 */
    int direction;        /**< 电机正转（FI）使角度增大为 +1，减小为 -1。 */
    unsigned duty;        /**< 驱动占空比，千分比。 */
    unsigned tolerance;   /**< 到达容差，毫度。 */
    int32_t limit_min;    /**< 允许运动范围下限，毫度。 */
    int32_t limit_max;    /**< 允许运动范围上限，毫度。 */
    uint32_t pot_ohms;    /**< 电位器总阻值，欧姆；0 表示采用标称值。 */
} joint_nominal_t;

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

/**
 * @brief 由理论参数算出两点标定。
 *
 * 两个标定点取在允许范围的两端，读数按分压公式计算。
 *
 * @param[out] out 成功时写入完整的标定参数。
 * @return NULL 表示成功；参数无效、参考点或允许范围超出电位器电气行程时
 *         返回 "CAL_PARAM"。
 */
const char *joint_cal_from_nominal(const joint_nominal_t *n, joint_cal_t *out);

/**
 * @brief 只修改已标定关节的允许运动范围，不改换算关系。
 *
 * @return NULL 表示成功；"NOT_CALIBRATED"、"BUSY"（有关节在运动）或
 *         "LIMIT_PARAM"（范围无效或超出电位器可测区间）。
 */
const char *joint_set_limits(joint_control_t *j, unsigned index,
                             int32_t limit_min, int32_t limit_max);

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

/**
 * @brief 把全部关节的标定导出为定长字节串，用于断电保存。
 *
 * 格式（多字节均为小端）：
 *   头：版本 u8、关节数 u8、保留 u16；
 *   每个关节：已标定 u8、方向 i8、占空比 u16、raw_a u16、raw_b u16、
 *            angle_a i32、angle_b i32、容差 u32、limit_min i32、limit_max i32。
 */
void joint_cal_export(const joint_control_t *j,
                      uint8_t blob[JOINT_CAL_BLOB_SIZE]);

/**
 * @brief 从 joint_cal_export() 的结果恢复标定。
 *
 * 版本或关节数不符时什么都不做；每个关节的参数仍经 joint_calibrate() 校验，
 * 不合法的项被跳过，未标记已标定的关节保持原状。
 *
 * @return 成功恢复的关节数。
 * @note 只应在没有关节运动时调用（如初始化阶段）。
 */
unsigned joint_cal_import(joint_control_t *j,
                          const uint8_t blob[JOINT_CAL_BLOB_SIZE]);

#endif /* AITAID_JOINT_CONTROL_H */
