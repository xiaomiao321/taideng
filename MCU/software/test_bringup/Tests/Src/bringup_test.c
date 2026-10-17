/**
 * @file bringup_test.c
 * @brief bring-up 测试运行器：执行测试表、转换结果、打印汇总。
 */

#include "bringup_test.h"

#include "bsp_common.h"
#include "bsp_key.h"
#include "bsp_log.h"

#include "stm32f4xx_hal.h"

const char *test_result_str(test_result_t result)
{
    switch (result)
    {
        case TEST_PASS:
            return "PASS";
        case TEST_FAIL_NO_RESPONSE:
            return "FAIL:NO_RESPONSE";
        case TEST_FAIL_CONFIG:
            return "FAIL:CONFIG";
        case TEST_FAIL_COMM:
            return "FAIL:COMM";
        case TEST_FAIL_DATA:
            return "FAIL:DATA";
        case TEST_SKIP:
            return "SKIP";
        default:
            return "UNKNOWN";
    }
}

static uint32_t s_confirm_baseline = 0U;
static bool s_confirm_armed = false;

void bringup_confirm_arm(void)
{
    s_confirm_baseline = 0U;
    s_confirm_armed = (bsp_key_get_press_count(&s_confirm_baseline) == BSP_OK);
}

bool bringup_confirmed(void)
{
    uint32_t now = 0U;

    if (!s_confirm_armed)
    {
        return false;
    }
    if (bsp_key_get_press_count(&now) != BSP_OK)
    {
        return false;
    }
    return (now != s_confirm_baseline);
}

test_result_t bringup_confirm(uint32_t timeout_ms)
{
    bringup_confirm_arm();
    if (!s_confirm_armed)
    {
        return TEST_FAIL_CONFIG;
    }

    uint32_t start_ms = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start_ms) < timeout_ms)
    {
        if (bringup_confirmed())
        {
            return TEST_PASS;
        }
    }

    return TEST_FAIL_NO_RESPONSE;
}

void bringup_run_all(void)
{
    uint32_t pass_count = 0U;
    uint32_t fail_count = 0U;
    uint32_t skip_count = 0U;

    LOG_INFO("TEST", "==== bring-up: %u tests ====",
             (unsigned int)g_bringup_case_count);

    for (uint32_t i = 0U; i < g_bringup_case_count; ++i)
    {
        const bringup_test_case_t *tc = &g_bringup_cases[i];
        test_result_t result = TEST_SKIP;

        if (tc->run != NULL)
        {
            result = tc->run();
        }

        if (result == TEST_PASS)
        {
            ++pass_count;
        }
        else if (result == TEST_SKIP)
        {
            ++skip_count;
        }
        else
        {
            ++fail_count;
        }

        LOG_INFO("TEST", "[%u/%u] %s -> %s", (unsigned int)(i + 1U),
                 (unsigned int)g_bringup_case_count, tc->name,
                 test_result_str(result));
    }

    LOG_INFO("TEST",
             "==== %u PASS / %u FAIL / %u SKIP ====", (unsigned int)pass_count,
             (unsigned int)fail_count, (unsigned int)skip_count);
}
