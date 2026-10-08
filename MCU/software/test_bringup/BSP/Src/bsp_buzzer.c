#include "bsp_buzzer.h"

#include <stdbool.h>

#include "stm32f407xx.h"
#include "tim.h"

static bool s_is_buzzer_initialized = false;
static bool s_is_buzzer_active = false;

bsp_buzzer_status_t bsp_buzzer_init(void)
{
    if (s_is_buzzer_initialized)
    {
        return BSP_BUZZER_STATUS_OK;
    }
    if (htim13.Instance != TIM13)
    {
        return BSP_BUZZER_STATUS_TIMER_ERROR;
    }
    __HAL_TIM_SET_COMPARE(&htim13, TIM_CHANNEL_1, 0U);
    s_is_buzzer_initialized = true;
    return BSP_BUZZER_STATUS_OK;
}

bsp_buzzer_status_t bsp_buzzer_start(uint32_t frequency_hz)
{
    if (frequency_hz == 0U)
    {
        return BSP_BUZZER_STATUS_INVALID_ARGUMENT;
    }
    if (s_is_buzzer_initialized == false)
    {
        return BSP_BUZZER_STATUS_NOT_INITIALIZED;
    }

    uint32_t timer_clock_hz = HAL_RCC_GetPCLK1Freq() * 2U;
    uint32_t prescaler = htim13.Init.Prescaler + 1U;
    uint32_t timer_counter_hz = timer_clock_hz / prescaler;
    uint32_t period_counts = timer_counter_hz / frequency_hz;

    if ((period_counts < 2U) || (period_counts > 65536U))
    {
        return BSP_BUZZER_STATUS_INVALID_ARGUMENT;
    }

    uint32_t arr = period_counts - 1U;
    uint32_t compare = period_counts / 2U;
    if (arr > 0xFFFFU)
    {
        return BSP_BUZZER_STATUS_INVALID_ARGUMENT;
    }

    __HAL_TIM_SET_AUTORELOAD(&htim13, arr);
    __HAL_TIM_SET_COMPARE(&htim13, TIM_CHANNEL_1, compare);
    __HAL_TIM_SET_COUNTER(&htim13, 0U);
    if (HAL_TIM_GenerateEvent(&htim13, TIM_EVENTSOURCE_UPDATE) !=
        HAL_OK) /*使 ARR 和 CCR 的新值立即生效*/
    {
        __HAL_TIM_SET_COMPARE(&htim13, TIM_CHANNEL_1, 0U);
        return BSP_BUZZER_STATUS_TIMER_ERROR;
    }

    /* 已经运行，不需要再次启动 */
    if (s_is_buzzer_active)
    {
        return BSP_BUZZER_STATUS_OK;
    }
    /* 尚未运行，启动PWM输出*/
    if (HAL_TIM_PWM_Start(&htim13, TIM_CHANNEL_1) != HAL_OK)
    {
        __HAL_TIM_SET_COMPARE(&htim13, TIM_CHANNEL_1, 0U);
        return BSP_BUZZER_STATUS_TIMER_ERROR;
    }
    s_is_buzzer_active = true;
    return BSP_BUZZER_STATUS_OK;
}

bsp_buzzer_status_t bsp_buzzer_stop(void)
{
    if (s_is_buzzer_initialized == false)
    {
        return BSP_BUZZER_STATUS_NOT_INITIALIZED;
    }
    if (!s_is_buzzer_active)
    {
        return BSP_BUZZER_STATUS_OK;
    }
    if (HAL_TIM_PWM_Stop(&htim13, TIM_CHANNEL_1) != HAL_OK)
    {
        return BSP_BUZZER_STATUS_TIMER_ERROR;
    }
    __HAL_TIM_SET_COMPARE(&htim13, TIM_CHANNEL_1, 0U);
    s_is_buzzer_active = false;

    return BSP_BUZZER_STATUS_OK;
}

bsp_buzzer_status_t bsp_buzzer_get_state(bool *is_active)
{
    if (s_is_buzzer_initialized == false)
    {
        return BSP_BUZZER_STATUS_NOT_INITIALIZED;
    }
    if (is_active == NULL)
    {
        return BSP_BUZZER_STATUS_INVALID_ARGUMENT;
    }
    *is_active = s_is_buzzer_active;
    return BSP_BUZZER_STATUS_OK;
}