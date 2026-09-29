#include "bsp_led.h"

#include "main.h"
#include "stm32f4xx_hal.h"


void bsp_led_init(void)
{
    HAL_GPIO_WritePin(LED_STATE_GPIO_Port, LED_STATE_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED_COMM_GPIO_Port, LED_COMM_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED_ACT_GPIO_Port, LED_ACT_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
}
bsp_led_status_t bsp_led_set(bsp_led_id_t led_id, bool is_on)
{
    switch (led_id)
    {
        case BSP_LED_ID_STATE:
            HAL_GPIO_WritePin(LED_STATE_GPIO_Port, LED_STATE_Pin,
                              is_on ? GPIO_PIN_RESET : GPIO_PIN_SET);
            break;
        case BSP_LED_ID_COMM:
            HAL_GPIO_WritePin(LED_COMM_GPIO_Port, LED_COMM_Pin,
                              is_on ? GPIO_PIN_RESET : GPIO_PIN_SET);
            break;
        case BSP_LED_ID_ACTIVITY:
            HAL_GPIO_WritePin(LED_ACT_GPIO_Port, LED_ACT_Pin,
                              is_on ? GPIO_PIN_RESET : GPIO_PIN_SET);
            break;
        case BSP_LED_ID_ERROR:
            HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin,
                              is_on ? GPIO_PIN_RESET : GPIO_PIN_SET);
            break;
        default:
            return BSP_LED_STATUS_INVALID_ID;
    }
    return BSP_LED_STATUS_OK;
}
bsp_led_status_t bsp_led_toggle(bsp_led_id_t led_id)
{
    switch (led_id)
    {
        case BSP_LED_ID_STATE:
            HAL_GPIO_TogglePin(LED_STATE_GPIO_Port, LED_STATE_Pin);
            break;
        case BSP_LED_ID_COMM:
            HAL_GPIO_TogglePin(LED_COMM_GPIO_Port, LED_COMM_Pin);
            break;
        case BSP_LED_ID_ACTIVITY:
            HAL_GPIO_TogglePin(LED_ACT_GPIO_Port, LED_ACT_Pin);
            break;
        case BSP_LED_ID_ERROR:
            HAL_GPIO_TogglePin(LED_ERR_GPIO_Port, LED_ERR_Pin);
            break;
        default:
            return BSP_LED_STATUS_INVALID_ID;
    }
    return BSP_LED_STATUS_OK;
}
bsp_led_status_t bsp_led_get_state(bsp_led_id_t led_id, bool *is_on)
{
    if (is_on == NULL)
    {
        return BSP_LED_STATUS_INVALID_ARGUMENT;
    }

    switch (led_id)
    {
        case BSP_LED_ID_STATE:
            *is_on = (HAL_GPIO_ReadPin(LED_STATE_GPIO_Port, LED_STATE_Pin) ==
                      GPIO_PIN_RESET);
            break;
        case BSP_LED_ID_COMM:
            *is_on = (HAL_GPIO_ReadPin(LED_COMM_GPIO_Port, LED_COMM_Pin) ==
                      GPIO_PIN_RESET);
            break;
        case BSP_LED_ID_ACTIVITY:
            *is_on = (HAL_GPIO_ReadPin(LED_ACT_GPIO_Port, LED_ACT_Pin) ==
                      GPIO_PIN_RESET);
            break;
        case BSP_LED_ID_ERROR:
            *is_on = (HAL_GPIO_ReadPin(LED_ERR_GPIO_Port, LED_ERR_Pin) ==
                      GPIO_PIN_RESET);
            break;
        default:
            return BSP_LED_STATUS_INVALID_ID;
    }
    return BSP_LED_STATUS_OK;
}