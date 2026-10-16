/**
 * @file bringup_test.h
 * @brief 板级 bring-up 测试框架：测试表、结果分类与汇总。
 *
 * 每个测试是一个返回 test_result_t 的函数，集中登记在 bringup_cases.c 的
 * g_bringup_cases[] 表中。bringup_run_all() 按表顺序执行，逐项打印结果，
 * 一项失败不会阻断后续项，最后打印 PASS/FAIL/SKIP 汇总。
 *
 * 需要人工判读的项目（示波器波形、灯亮、声音、脉冲）用 bringup_confirm()
 * 等待操作员按 HOME 键确认：确认即 PASS，超时即 FAIL_NO_RESPONSE。
 *
 * @note 新增测试：在 bringup_cases.c 里写一个 test_xxx() 并在表里加一行。
 * @note 测试表会驱动灯板和电机 PWM；每个电机方向限时点动后停止，再等待确认。
 */

#ifndef AITAID_BRINGUP_TEST_H
#define AITAID_BRINGUP_TEST_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 操作员确认的默认等待时间，单位为毫秒。
 *
 * @note 期间被测输出保持有效，便于用示波器观察；超时按失败处理。
 */
#define BRINGUP_CONFIRM_TIMEOUT_MS (15000U)

/**
 * @brief 单个测试的结果。
 */
typedef enum
{
    TEST_PASS = 0,         /**< 通过。 */
    TEST_FAIL_NO_RESPONSE, /**< 硬件无响应：无波形/无输出，或操作员未在超时内确认。 */
    TEST_FAIL_CONFIG,      /**< 配置与驱动预期不符（句柄、外设模式）。 */
    TEST_FAIL_COMM,        /**< 与 HAL/外设交互失败。 */
    TEST_FAIL_DATA,        /**< 有数据但内容异常。 */
    TEST_SKIP              /**< 依赖未满足，本次未执行。 */
} test_result_t;

/** @brief 单个测试的实现函数，返回本项结果。 */
typedef test_result_t (*bringup_test_fn_t)(void);

/**
 * @brief 测试表的一项。
 */
typedef struct
{
    const char *name;      /**< 测试名，用于日志，如 "LAMP_UP1"。 */
    bringup_test_fn_t run; /**< 测试实现，不允许为 NULL。 */
} bringup_test_case_t;

/**
 * @brief 按表执行全部测试并打印汇总。
 *
 * @note 逐项执行；某项失败不会阻断后续项。
 * @note 每项结束后打印一行 "<i/n> NAME -> RESULT"。
 */
void bringup_run_all(void);

/**
 * @brief 等待操作员按 HOME 键确认。
 *
 * @param[in] timeout_ms 等待时间，单位为毫秒。
 *
 * @retval TEST_PASS 在超时前检测到一次 HOME 按下。
 * @retval TEST_FAIL_NO_RESPONSE 超时未确认。
 * @retval TEST_FAIL_CONFIG 无法读取按键计数。
 *
 * @note 用于需要人工判读的项目；调用期间被测输出应保持有效。
 */
test_result_t bringup_confirm(uint32_t timeout_ms);

/**
 * @brief 记录当前 HOME 按键计数作为确认基线。
 *
 * @note 与 bringup_confirmed() 搭配，用于测试需要自己驱动周期性输出、
 *       不能用 bringup_confirm() 阻塞等待的场合（例如需要重复发脉冲）。
 */
void bringup_confirm_arm(void);

/**
 * @brief 查询自 bringup_confirm_arm() 以来是否检测到一次 HOME 按下。
 *
 * @retval true 已确认。
 * @retval false 尚未确认，或无法读取按键计数。
 */
bool bringup_confirmed(void);

/**
 * @brief 返回结果码的短名称，用于日志。
 *
 * @param[in] result 结果码。
 * @return 指向静态字符串的指针，传入未定义值时返回 "UNKNOWN"。
 */
const char *test_result_str(test_result_t result);

/** @brief 测试表，由 bringup_cases.c 提供。 */
extern const bringup_test_case_t g_bringup_cases[];

/** @brief 测试表项数，由 bringup_cases.c 提供。 */
extern const uint32_t g_bringup_case_count;

#endif /* AITAID_BRINGUP_TEST_H */
