#include "bsp_key.h"

#include "main.h"

static bool s_is_initialized = false;
static volatile uint32_t s_press_count = 0;
static uint32_t s_last_press_tick = 0;

bsp_status_t bsp_key_init(void)
{
    s_press_count = 0;
    s_last_press_tick = 0;
    s_is_initialized = true;
    return BSP_OK;
}
bsp_status_t bsp_key_read(bool *is_pressed)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (is_pressed == NULL)
    {
        return BSP_ERR_PARAM;
    }
    *is_pressed =
        (HAL_GPIO_ReadPin(HOME_KEY_GPIO_Port, HOME_KEY_Pin) == GPIO_PIN_RESET);
    return BSP_OK;
}

void bsp_key_on_exti_from_isr(void)
{
    if (s_is_initialized == false)
    {
        return;
    }
    uint32_t now_ms = HAL_GetTick();
    if (now_ms - s_last_press_tick < BSP_KEY_DEBOUNCE_MS)
    {
        return;
    }
    s_last_press_tick = now_ms;
    s_press_count++;
}

bsp_status_t bsp_key_get_press_count(uint32_t *press_count)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (press_count == NULL)
    {
        return BSP_ERR_PARAM;
    }
    *press_count = s_press_count;
    return BSP_OK;
}
