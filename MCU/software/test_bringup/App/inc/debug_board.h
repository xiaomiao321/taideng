/**
 * @file debug_board.h
 * @brief 全板逐项测试的状态机：按名字启动一项测试，限时驱动输出，等待人工确认。
 *
 * 测试表 debug_board_names[] 的顺序固定为：
 *   单项测试（LOG、LED、KEY、BUZZER、PWR、ADC6，与 board_kind_t 前 6 项一一对应）
 *   → 灯板各路（LAMP_UP1～LAMP_DOWN3）→ 每台电机正反两个方向（MOTOR_Mx_FI/BI）。
 *
 * 本模块不依赖 HAL，硬件动作通过 board_hw_t 回调交给调用者，可在 PC 上测试。
 */

#ifndef AITAID_DEBUG_BOARD_H
#define AITAID_DEBUG_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "motor_limits.h"

/** @brief 测试表开头与 board_kind_t 一一对应的单项测试数量（LOG～ADC6）。 */
#define DEBUG_BOARD_SINGLE_COUNT (6U)

/** @brief 灯板通道数，应与 BSP_LIGHT_COUNT 一致。 */
#define DEBUG_BOARD_LAMP_COUNT (7U)

/** @brief LED 测试依次点亮的指示灯数量，应与 BSP_LED_ID_COUNT 一致。 */
#define DEBUG_BOARD_LED_COUNT (4U)

/** @brief 测试表中第一项灯板测试的下标。 */
#define DEBUG_BOARD_LAMP_FIRST (DEBUG_BOARD_SINGLE_COUNT)

/** @brief 测试表中第一项电机测试的下标；每台电机占 FI、BI 两项。 */
#define DEBUG_BOARD_MOTOR_FIRST                                                \
    (DEBUG_BOARD_LAMP_FIRST + DEBUG_BOARD_LAMP_COUNT)

/** @brief 测试表总项数。 */
#define DEBUG_BOARD_TEST_COUNT (DEBUG_BOARD_MOTOR_FIRST + 2U * MOTOR_COUNT)

/** @brief 测试类别；前 DEBUG_BOARD_SINGLE_COUNT 项与测试表开头的顺序一致。 */
typedef enum
{
    BOARD_LOG,
    BOARD_LED,
    BOARD_KEY,
    BOARD_BUZZER,
    BOARD_PWR,
    BOARD_ADC,
    BOARD_LAMP,
    BOARD_MOTOR
} board_kind_t;

/** @brief 测试阶段。 */
typedef enum
{
    BOARD_IDLE,     /**< 没有测试在进行。 */
    BOARD_SETTLE,   /**< 已接受，等待一小段时间后再驱动输出。 */
    BOARD_DRIVE,    /**< 正在驱动输出。 */
    BOARD_WAIT,     /**< 输出已关闭，等待人工 CONFIRM。 */
    BOARD_KEY_WAIT, /**< KEY 测试：等待按下 HOME。 */
    BOARD_PHASE_COUNT
} board_phase_t;

/** @brief 调用者提供的硬件回调。 */
typedef struct
{
    /** @brief 驱动一项输出；index 为 LED、灯板通道或电机的 0 起下标。 */
    bool (*apply)(board_kind_t kind, unsigned index, int value);
    /** @brief 关闭全部输出；返回 false 表示硬件失败。 */
    bool (*off)(void);
    /** @brief 发送一行回复，内容已含 CRLF。 */
    void (*reply)(const char *line);
} board_hw_t;

/** @brief 测试状态。调用者只读，不得直接修改。 */
typedef struct
{
    board_hw_t hw;
    const char *name;     /**< 当前测试名，指向 debug_board_names[]。 */
    board_kind_t kind;    /**< 当前测试类别。 */
    board_phase_t phase;  /**< 当前阶段。 */
    unsigned seq;         /**< 启动该测试的命令序号。 */
    unsigned index;       /**< LED、灯板通道或电机的 0 起下标。 */
    unsigned led_step;    /**< LED 测试已点到第几个灯。 */
    int value;            /**< 输出值；电机反向测试为负数。 */
    uint32_t started_ms;  /**< 当前阶段开始的时刻。 */
    uint32_t duration_ms; /**< 每个驱动阶段的持续时间。 */
} debug_board_t;

/** @brief 测试名表，共 DEBUG_BOARD_TEST_COUNT 项。 */
extern const char *const debug_board_names[DEBUG_BOARD_TEST_COUNT];

/** @brief 初始化为空闲。 */
void debug_board_init(debug_board_t *b, board_hw_t hw);

/**
 * @brief 按名字启动一项测试；正在进行的测试先被取消。
 *
 * @return NULL 表示已接受；否则为错误名 "TEST_PARAM"。
 */
const char *debug_board_start(debug_board_t *b, unsigned seq, const char *name,
                              int value, uint32_t duration, uint32_t now);

/**
 * @brief 人工确认 WAIT 阶段的测试结果。
 *
 * @return NULL 表示已接受；不在 WAIT 阶段时返回 "NOT_WAITING"。
 */
const char *debug_board_confirm(debug_board_t *b, bool pass);

/** @brief 周期调用：推进测试阶段。home 为 HOME 是否按下或按过。 */
void debug_board_update(debug_board_t *b, uint32_t now, bool home);

/** @brief 取消正在进行的测试并关闭输出；空闲时什么都不做。 */
void debug_board_cancel(debug_board_t *b);

/** @brief 当前阶段的短名称，用于状态上报。 */
const char *debug_board_phase(const debug_board_t *b);

#endif /* AITAID_DEBUG_BOARD_H */
