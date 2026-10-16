/**
 * @file bringup_cases.c
 * @brief bring-up 测试用例表。
 *
 * 每个用例只做一件事：初始化对应 BSP、给一定输出、必要时等待操作员确认，
 * 然后返回 test_result_t。新增用例时在本文件写一个 test_xxx() 并在
 * g_bringup_cases[] 末尾加一行即可，运行器不需要改动。
 *
 * 电机用例采用限时点动，结束后停止输出，再等待操作员确认。
 */

#include <stdbool.h>

#include "bringup_test.h"
#include "bsp_adc.h"
#include "bsp_buzzer.h"
#include "bsp_common.h"
#include "bsp_key.h"
#include "bsp_led.h"
#include "bsp_light.h"
#include "bsp_linux_power.h"
#include "bsp_log.h"
#include "bsp_motor.h"

/* Linux 电源脉冲测试参数，单位毫秒。
   脉冲会周期性重复，便于示波器随时触发测量，不会"没看清就过去了"。
   @warning 重复发脉冲意味着会重复按电源键；连 Linux
   核心板测量前请先确认其电气要求， 或直接断开 Linux 观察 PB12。 */
#define PWR_TEST_PULSE_MS (200U)   /* 单次脉冲宽度 */
#define PWR_TEST_PERIOD_MS (1000U) /* 两次脉冲之间的间隔 */

/* 电机点动测试参数。
   点动时会周期性打印六路电位器相对起始值的偏差，用来找出"哪路电机对应哪个
   电位器"：只有真正联动的那个电位器数字会明显变化。
   已确认电机启动需要约 80% 占空比，每个方向最多点动 500ms。 */
#define JOG_DUTY_PERMILLE (800)
#define JOG_RUN_MS (500U)
#define JOG_POT_PERIOD_MS (250U) /* 电位器偏差的打印间隔 */
#define JOG_ADC_TIMEOUT_MS (10U) /* 单次六路扫描的等待超时 */

/* -------------------------------------------------------------------------- */
/* 软件类：不需要人工判读 */
/* -------------------------------------------------------------------------- */

static test_result_t test_log(void)
{
    if (bsp_log_write_string("BRINGUP log ok\r\n") != BSP_OK)
    {
        return TEST_FAIL_COMM;
    }
    return TEST_PASS;
}

test_result_t test_adc(void)
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
/* 人机类：需要操作员确认 */
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

    LOG_INFO("PWR", "PB12 pulses %ums every %ums; press HOME to pass",
             (unsigned int)PWR_TEST_PULSE_MS, (unsigned int)PWR_TEST_PERIOD_MS);

    bringup_confirm_arm();

    if (bsp_linux_power_start(PWR_TEST_PULSE_MS) != BSP_OK)
    {
        return TEST_FAIL_COMM;
    }

    uint32_t start_ms = HAL_GetTick();
    uint32_t last_pulse_ms = start_ms;
    test_result_t result = TEST_FAIL_NO_RESPONSE;

    /* 自己驱动循环：既周期性重发脉冲，又非阻塞地等待操作员确认。
       脉冲靠 bsp_linux_power_update() 到期释放。 */
    while ((uint32_t)(HAL_GetTick() - start_ms) < BRINGUP_CONFIRM_TIMEOUT_MS)
    {
        if (bringup_confirmed())
        {
            result = TEST_PASS;
            break;
        }
        if ((uint32_t)(HAL_GetTick() - last_pulse_ms) >= PWR_TEST_PERIOD_MS)
        {
            last_pulse_ms = HAL_GetTick();
            (void)bsp_linux_power_start(PWR_TEST_PULSE_MS);
        }
        (void)bsp_linux_power_update();
    }

    /* 结束前务必释放，避免把电源键一直按住 */
    (void)bsp_linux_power_release();
    return result;
}

/* -------------------------------------------------------------------------- */
/* 灯板：每路单独确认 */
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
/* 电机：每台每个方向单独确认 */
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

    /* 电位器联动：转这一路的同时盯住六路读数，用来确定电机与电位器的对应关系。
       读不到电位器不算电机失败，只提示，免得 ADC 的问题掩盖电机本身的问题。 */
    bool pot_ok = (bsp_adc_init() == BSP_OK);
    uint16_t baseline[BSP_ADC_CHANNEL_COUNT] = {0};
    if (pot_ok && (bsp_adc_read_all(baseline, JOG_ADC_TIMEOUT_MS) != BSP_OK))
    {
        pot_ok = false;
    }
    if (!pot_ok)
    {
        LOG_WARN("MOTOR", "pot readout unavailable; motor test continues");
    }

    /* 两次点动之间留出停止间隔。 */
    HAL_Delay(500U);
    bringup_confirm_arm();

    status = bsp_motor_set_duty(
        id, forward ? JOG_DUTY_PERMILLE : -JOG_DUTY_PERMILLE);
    if (status != BSP_OK)
    {
        LOG_ERROR("MOTOR", "%s set failed: %s", label, bsp_status_str(status));
        (void)bsp_motor_coast(id);
        return TEST_FAIL_COMM;
    }

    LOG_INFO("MOTOR", "%s %s at %d permille for 500ms; dPOT1..6 every %ums; HOME to stop",
             label, forward ? "FI" : "BI", (int)JOG_DUTY_PERMILLE,
             (unsigned int)JOG_POT_PERIOD_MS);

    uint32_t start_ms = HAL_GetTick();
    uint32_t last_pot_ms = start_ms;
    test_result_t result = TEST_FAIL_NO_RESPONSE;

    /* 自己驱动循环：一边等操作员按 HOME，一边周期性打印电位器偏差。 */
    while ((uint32_t)(HAL_GetTick() - start_ms) < JOG_RUN_MS)
    {
        if (bringup_confirmed())
        {
            result = TEST_PASS;
            break;
        }
        if (pot_ok &&
            ((uint32_t)(HAL_GetTick() - last_pot_ms) >= JOG_POT_PERIOD_MS))
        {
            last_pot_ms = HAL_GetTick();

            uint16_t raw[BSP_ADC_CHANNEL_COUNT] = {0};
            if (bsp_adc_read_all(raw, JOG_ADC_TIMEOUT_MS) == BSP_OK)
            {
                /* 打印相对起始值的偏差：联动的那个电位器会明显偏离 0。 */
                LOG_INFO("POT", "d=%d %d %d %d %d %d",
                         (int)raw[0] - (int)baseline[0],
                         (int)raw[1] - (int)baseline[1],
                         (int)raw[2] - (int)baseline[2],
                         (int)raw[3] - (int)baseline[3],
                         (int)raw[4] - (int)baseline[4],
                         (int)raw[5] - (int)baseline[5]);
            }
        }
    }

    if (bsp_motor_coast(id) != BSP_OK)
    {
        return TEST_FAIL_COMM;
    }
    if (result != TEST_PASS)
    {
        LOG_INFO("MOTOR", "%s stopped; press HOME to confirm motion", label);
        result = bringup_confirm(BRINGUP_CONFIRM_TIMEOUT_MS);
    }
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
/* 测试表 */
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
