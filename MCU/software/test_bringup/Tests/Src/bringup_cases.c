/**
 * @file bringup_cases.c
 * @brief bring-up 测试用例表。
 *
 * 每个用例只做一件事：初始化对应 BSP、给一定输出、必要时等待操作员确认，
 * 然后返回 test_result_t。新增用例时在本文件写一个 test_xxx() 并在
 * g_bringup_cases[] 末尾加一行即可，运行器不需要改动。
 *
 * @warning 电机用例会驱动电机 PWM。整轮 run 之前请断开电机或断开电机电源，
 *          否则每个电机用例在等待确认期间电机会持续转动。
 */

#include "bringup_test.h"

#include <stdbool.h>

#include "bsp_adc.h"
#include "bsp_buzzer.h"
#include "bsp_common.h"
#include "bsp_key.h"
#include "bsp_led.h"
#include "bsp_light.h"
#include "bsp_linux_power.h"
#include "bsp_log.h"
#include "bsp_motor.h"

/* -------------------------------------------------------------------------- */
/* 软件类：不需要人工判读                                                       */
/* -------------------------------------------------------------------------- */

static test_result_t test_log(void)
{
    if (bsp_log_write_string("BRINGUP log ok\r\n") != BSP_OK)
    {
        return TEST_FAIL_COMM;
    }
    return TEST_PASS;
}

static test_result_t test_adc(void)
{
    bsp_status_t status = bsp_adc_init();
    if (status != BSP_OK)
    {
        LOG_ERROR("ADC", "init failed: %s", bsp_status_str(status));
        return TEST_FAIL_CONFIG;
    }

    uint16_t raw[BSP_ADC_CHANNEL_COUNT] = {0};
    status = bsp_adc_read_all(raw, 10U);
    if (status != BSP_OK)
    {
        LOG_ERROR("ADC", "read failed: %s", bsp_status_str(status));
        return (status == BSP_ERR_TIMEOUT) ? TEST_FAIL_NO_RESPONSE
                                           : TEST_FAIL_COMM;
    }

    for (uint32_t i = 0U; i < (uint32_t)BSP_ADC_CHANNEL_COUNT; ++i)
    {
        LOG_INFO("ADC", "POT%u = %u", (unsigned int)(i + 1U),
                 (unsigned int)raw[i]);
    }
    return TEST_PASS;
}

/* -------------------------------------------------------------------------- */
/* 人机类：需要操作员确认                                                       */
/* -------------------------------------------------------------------------- */

static test_result_t test_led(void)
{
    static const char *const led_names[BSP_LED_ID_COUNT] = {"STATE", "COMM",
                                                            "ACT", "ERR"};

    if (bsp_led_init() != BSP_OK)
    {
        return TEST_FAIL_CONFIG;
    }

    for (uint32_t i = 0U; i < (uint32_t)BSP_LED_ID_COUNT; ++i)
    {
        LOG_INFO("LED", "%s on", led_names[i]);
        (void)bsp_led_set((bsp_led_id_t)i, true);
        HAL_Delay(1000U);
        (void)bsp_led_set((bsp_led_id_t)i, false);
    }
    LOG_INFO("LED", "4 LEDs should cycle; press HOME to pass");
    return bringup_confirm(BRINGUP_CONFIRM_TIMEOUT_MS);
}

static test_result_t test_key(void)
{
    if (bsp_key_init() != BSP_OK)
    {
        return TEST_FAIL_CONFIG;
    }
    /* 本项就是按键测试：按 HOME 即通过 */
    LOG_INFO("KEY", "press HOME key now");
    return bringup_confirm(BRINGUP_CONFIRM_TIMEOUT_MS);
}

static test_result_t test_buzzer(void)
{
    if (bsp_buzzer_init() != BSP_OK)
    {
        return TEST_FAIL_CONFIG;
    }
    if (bsp_buzzer_start(2700U) != BSP_OK)
    {
        return TEST_FAIL_COMM;
    }
    HAL_Delay(1000U);
    (void)bsp_buzzer_stop();

    LOG_INFO("BUZZ", "expect a ~1s beep; press HOME to pass");
    return bringup_confirm(BRINGUP_CONFIRM_TIMEOUT_MS);
}

static test_result_t test_power(void)
{
    if (bsp_linux_power_init() != BSP_OK)
    {
        return TEST_FAIL_CONFIG;
    }
    LOG_INFO("PWR", "PB12 outputs ~200ms pulse (scope); press HOME to pass");
    if (bsp_linux_power_start(200U) != BSP_OK)
    {
        return TEST_FAIL_COMM;
    }

    /* 脉冲必须靠周期调用 update 才会释放 */
    uint32_t start_ms = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start_ms) < 300U)
    {
        (void)bsp_linux_power_update();
    }
    return bringup_confirm(BRINGUP_CONFIRM_TIMEOUT_MS);
}

/* -------------------------------------------------------------------------- */
/* 灯板：每路单独确认                                                           */
/* -------------------------------------------------------------------------- */

static test_result_t test_lamp_one(bsp_light_id_t id, const char *label)
{
    bsp_status_t status = bsp_light_init();
    if (status != BSP_OK)
    {
        LOG_ERROR("LAMP", "init failed: %s", bsp_status_str(status));
        return TEST_FAIL_CONFIG;
    }

    status = bsp_light_set_duty(id, 500U);
    if (status != BSP_OK)
    {
        LOG_ERROR("LAMP", "%s set failed: %s", label, bsp_status_str(status));
        return TEST_FAIL_COMM;
    }

    LOG_INFO("LAMP", "%s = 50%%, expect 20kHz; press HOME to pass", label);
    test_result_t result = bringup_confirm(BRINGUP_CONFIRM_TIMEOUT_MS);
    (void)bsp_light_stop(id);
    return result;
}

static test_result_t test_lamp_up1(void)
{
    return test_lamp_one(BSP_LIGHT_UP1, "UP1");
}
static test_result_t test_lamp_up2(void)
{
    return test_lamp_one(BSP_LIGHT_UP2, "UP2");
}
static test_result_t test_lamp_up3(void)
{
    return test_lamp_one(BSP_LIGHT_UP3, "UP3");
}
static test_result_t test_lamp_up4(void)
{
    return test_lamp_one(BSP_LIGHT_UP4, "UP4");
}
static test_result_t test_lamp_down1(void)
{
    return test_lamp_one(BSP_LIGHT_DOWN1, "DOWN1");
}
static test_result_t test_lamp_down2(void)
{
    return test_lamp_one(BSP_LIGHT_DOWN2, "DOWN2");
}
static test_result_t test_lamp_down3(void)
{
    return test_lamp_one(BSP_LIGHT_DOWN3, "DOWN3");
}

/* -------------------------------------------------------------------------- */
/* 电机：每台每个方向单独确认                                                   */
/* -------------------------------------------------------------------------- */

static test_result_t test_motor_one(bsp_motor_id_t id, const char *label,
                                    bool forward)
{
    bsp_status_t status = bsp_motor_init();
    if (status != BSP_OK)
    {
        LOG_ERROR("MOTOR", "init failed: %s", bsp_status_str(status));
        return TEST_FAIL_CONFIG;
    }

    status = bsp_motor_set_duty(id, forward ? 500 : -500);
    if (status != BSP_OK)
    {
        LOG_ERROR("MOTOR", "%s set failed: %s", label, bsp_status_str(status));
        return TEST_FAIL_COMM;
    }

    LOG_INFO("MOTOR", "%s %s = 50%%, expect 20kHz; press HOME to pass", label,
             forward ? "FI" : "BI");
    test_result_t result = bringup_confirm(BRINGUP_CONFIRM_TIMEOUT_MS);
    (void)bsp_motor_coast(id);
    return result;
}

static test_result_t test_motor_m1_fi(void)
{
    return test_motor_one(BSP_MOTOR_M1, "M1", true);
}
static test_result_t test_motor_m1_bi(void)
{
    return test_motor_one(BSP_MOTOR_M1, "M1", false);
}
static test_result_t test_motor_m2_fi(void)
{
    return test_motor_one(BSP_MOTOR_M2, "M2", true);
}
static test_result_t test_motor_m2_bi(void)
{
    return test_motor_one(BSP_MOTOR_M2, "M2", false);
}
static test_result_t test_motor_m3_fi(void)
{
    return test_motor_one(BSP_MOTOR_M3, "M3", true);
}
static test_result_t test_motor_m3_bi(void)
{
    return test_motor_one(BSP_MOTOR_M3, "M3", false);
}
static test_result_t test_motor_m4_fi(void)
{
    return test_motor_one(BSP_MOTOR_M4, "M4", true);
}
static test_result_t test_motor_m4_bi(void)
{
    return test_motor_one(BSP_MOTOR_M4, "M4", false);
}
static test_result_t test_motor_m5_fi(void)
{
    return test_motor_one(BSP_MOTOR_M5, "M5", true);
}
static test_result_t test_motor_m5_bi(void)
{
    return test_motor_one(BSP_MOTOR_M5, "M5", false);
}
static test_result_t test_motor_m6_fi(void)
{
    return test_motor_one(BSP_MOTOR_M6, "M6", true);
}
static test_result_t test_motor_m6_bi(void)
{
    return test_motor_one(BSP_MOTOR_M6, "M6", false);
}
static test_result_t test_motor_m7_fi(void)
{
    return test_motor_one(BSP_MOTOR_M7, "M7", true);
}
static test_result_t test_motor_m7_bi(void)
{
    return test_motor_one(BSP_MOTOR_M7, "M7", false);
}
static test_result_t test_motor_m8_fi(void)
{
    return test_motor_one(BSP_MOTOR_M8, "M8", true);
}
static test_result_t test_motor_m8_bi(void)
{
    return test_motor_one(BSP_MOTOR_M8, "M8", false);
}

/* -------------------------------------------------------------------------- */
/* 测试表                                                                      */
/* -------------------------------------------------------------------------- */

const bringup_test_case_t g_bringup_cases[] = {
    {"LOG", test_log},
    {"LED", test_led},
    {"KEY", test_key},
    {"BUZZER", test_buzzer},
    {"PWR", test_power},
    {"ADC6", test_adc},
    {"LAMP_UP1", test_lamp_up1},
    {"LAMP_UP2", test_lamp_up2},
    {"LAMP_UP3", test_lamp_up3},
    {"LAMP_UP4", test_lamp_up4},
    {"LAMP_DOWN1", test_lamp_down1},
    {"LAMP_DOWN2", test_lamp_down2},
    {"LAMP_DOWN3", test_lamp_down3},
    {"MOTOR_M1_FI", test_motor_m1_fi},
    {"MOTOR_M1_BI", test_motor_m1_bi},
    {"MOTOR_M2_FI", test_motor_m2_fi},
    {"MOTOR_M2_BI", test_motor_m2_bi},
    {"MOTOR_M3_FI", test_motor_m3_fi},
    {"MOTOR_M3_BI", test_motor_m3_bi},
    {"MOTOR_M4_FI", test_motor_m4_fi},
    {"MOTOR_M4_BI", test_motor_m4_bi},
    {"MOTOR_M5_FI", test_motor_m5_fi},
    {"MOTOR_M5_BI", test_motor_m5_bi},
    {"MOTOR_M6_FI", test_motor_m6_fi},
    {"MOTOR_M6_BI", test_motor_m6_bi},
    {"MOTOR_M7_FI", test_motor_m7_fi},
    {"MOTOR_M7_BI", test_motor_m7_bi},
    {"MOTOR_M8_FI", test_motor_m8_fi},
    {"MOTOR_M8_BI", test_motor_m8_bi},
};

const uint32_t g_bringup_case_count =
    sizeof(g_bringup_cases) / sizeof(g_bringup_cases[0]);
