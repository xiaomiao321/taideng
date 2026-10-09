#include "bsp_linux_power.h"

#include <stdbool.h>
#include <stdint.h>

#include "main.h"
#include "stm32f407xx.h"


static bool s_is_initialized = false;
static uint32_t s_start_tick_ms = 0U;
static uint32_t s_duration_ms = 0U;
static bool s_is_pulse_active = false;
bsp_status_t bsp_linux_power_init(void)
{
    s_start_tick_ms = 0U;
    s_duration_ms = 0U;
    s_is_pulse_active = false;
    HAL_GPIO_WritePin(LINUX_PWR_PULSE_GPIO_Port, LINUX_PWR_PULSE_Pin,
                      GPIO_PIN_RESET);
    s_is_initialized = true;
    return BSP_OK;
}

bsp_status_t bsp_linux_power_start(uint32_t duration_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (duration_ms == 0U || duration_ms > BSP_LINUX_POWER_MAX_DURATION_MS)
    {
        return BSP_ERR_PARAM;
    }
    if (s_is_pulse_active == true)
    {
        return BSP_BUSY;
    }
    s_start_tick_ms = HAL_GetTick();
    s_duration_ms = duration_ms;
    HAL_GPIO_WritePin(LINUX_PWR_PULSE_GPIO_Port, LINUX_PWR_PULSE_Pin,
                      GPIO_PIN_SET);
    s_is_pulse_active = true;
    return BSP_OK;
}

bsp_status_t bsp_linux_power_update(void)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (s_is_pulse_active == false)
    {
        return BSP_OK;
    }

    uint32_t now_tick = HAL_GetTick();
    if ((now_tick - s_start_tick_ms) >= s_duration_ms)
    {
        HAL_GPIO_WritePin(LINUX_PWR_PULSE_GPIO_Port, LINUX_PWR_PULSE_Pin,
                          GPIO_PIN_RESET);
        s_is_pulse_active = false;
    }

    return BSP_OK;
}
