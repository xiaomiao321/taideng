/**
 * @file debug_control.h
 * @brief 调试终端文本命令的解析与电机安全控制（心跳、HOME 锁定、延时启动）。
 *
 * 本模块不依赖 HAL：所有硬件动作通过 debug_hw_t 回调交给调用者，因此可以在
 * PC 上用 HostTest/debug_control_selftest.c 完整测试。
 *
 * 安全规则：
 * - 除 PING、STOP、INFO、MOFF、JGET、JTRACE 外，任何会动输出的命令都要求
 *   DEBUG_CONTROL_HEARTBEAT_TIMEOUT_MS 内收到过 PING；
 * - 心跳超时、HOME 按下或硬件失败都会停止全部输出并锁定，
 *   只有显式 STOP ALL（且 HOME 已松开）才解锁；
 * - 电机启动前先停 DEBUG_CONTROL_SETTLE_MS，避免带电直接换向。
 */

#ifndef AITAID_DEBUG_CONTROL_H
#define AITAID_DEBUG_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "motor_limits.h"

/** @brief 一行命令的最大长度（含结尾空字符），单位为字节。 */
#define DEBUG_CONTROL_LINE_MAX (192U)

/**
 * @brief 调用者提供的硬件回调。test、confirm、joint、manual、aux 可为 NULL，
 *        为 NULL 时对应命令回 ERR，INFO 也不会报告该能力。
 */
typedef struct
{
    /** @brief 停止全部输出；返回 false 表示硬件失败。 */
    bool (*stop)(void);
    /** @brief 驱动一路电机。index 为 0 起的电机下标，duty 为千分比。 */
    bool (*run)(unsigned index, int duty);
    /** @brief 发送一行回复，内容已含 CRLF。 */
    void (*reply)(const char *line);
    /** @brief 启动一项全板测试；返回 NULL 表示已接受，否则为错误名。 */
    const char *(*test)(unsigned seq, const char *name, int value,
                        uint32_t duration);
    /** @brief 人工确认测试结果；返回 NULL 表示已接受，否则为错误名。 */
    const char *(*confirm)(bool pass);
    /** @brief 关节命令；id 为 0 起的关节下标。返回 NULL 或错误名。 */
    const char *(*joint)(const char *op, unsigned seq, unsigned id,
                         const int32_t *values, unsigned count, uint32_t now);
    /** @brief 进入多路手动控制前取消其他控制者；返回 false 表示硬件失败。 */
    bool (*manual)(void);
    /** @brief 其他命令（LSET、LMODE、LTX、MPWM）；返回 NULL 或错误名。 */
    const char *(*aux)(const char *op, const char *const *args, unsigned count);
} debug_hw_t;

/**
 * @brief 控制状态。调用者只读，不得直接修改。
 *
 * 单路 RUN 命令的 motor 与 pending_motor 是文本协议里的 M 编号（1～8），
 * 0 表示没有；多路数组的下标是 0 起的电机下标。
 */
typedef struct
{
    debug_hw_t hw;
    uint32_t heartbeat_ms;         /**< 最近一次 PING 的时刻。 */
    uint32_t start_ms;             /**< 单路电机开始转动的时刻。 */
    uint32_t duration_ms;          /**< 单路电机持续时间，0 表示不限时。 */
    uint32_t pending_ms;           /**< 单路命令排队的时刻。 */
    unsigned motor;                /**< 正在转的单路电机 M 编号，0 表示没有。 */
    unsigned pending_motor;        /**< 等待启动的单路电机 M 编号。 */
    int duty;                      /**< 单路电机当前占空比。 */
    int pending_duty;              /**< 单路电机待启动的占空比。 */
    bool linked;                   /**< 心跳有效。 */
    bool locked;                   /**< 已锁定，需 STOP ALL 解锁。 */
    bool home;                     /**< HOME 键当前是否按下。 */
    bool pending;                  /**< 有单路命令在等待启动。 */
    int multi_duty[MOTOR_COUNT];   /**< 多路手动：当前占空比。 */
    int multi_target[MOTOR_COUNT]; /**< 多路手动：目标占空比。 */
    uint32_t multi_start[MOTOR_COUNT];    /**< 多路手动：开始转动时刻。 */
    uint32_t multi_duration[MOTOR_COUNT]; /**< 多路手动：持续时间，0 不限时。 */
    uint32_t multi_wait[MOTOR_COUNT];     /**< 多路手动：开始等待启动时刻。 */
    unsigned multi_pending; /**< 多路手动：等待启动的电机位掩码。 */
} debug_control_t;

/** @brief 初始化并停止全部输出；停止失败时直接进入锁定状态。 */
void debug_control_init(debug_control_t *c, debug_hw_t hw);

/**
 * @brief 处理一行命令（不含换行），并通过 hw.reply 回复。
 *
 * @param[in] line 以空字符结尾的命令，长度小于 DEBUG_CONTROL_LINE_MAX。
 * @param[in] now 当前时刻，单位毫秒。
 */
void debug_control_line(debug_control_t *c, const char *line, uint32_t now);

/**
 * @brief 周期调用：检查心跳与 HOME，按时启动和停止电机。
 *
 * @param[in] now 当前时刻，单位毫秒。
 * @param[in] home HOME 键是否按下（或自上次调用以来按过）。
 */
void debug_control_update(debug_control_t *c, uint32_t now, bool home);

/**
 * @brief 停止全部输出、锁定，并回复 reason；停止失败时改回复 EVENT HARDWARE。
 *
 * @param[in] reason 完整的一行事件文本，含 CRLF。
 */
void debug_control_fault(debug_control_t *c, const char *reason);

#endif /* AITAID_DEBUG_CONTROL_H */
