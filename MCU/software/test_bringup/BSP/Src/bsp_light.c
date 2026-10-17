#include "bsp_light.h"

#include <stdbool.h>

#include "tim.h"

static bsp_light_pwm_t s_lights[BSP_LIGHT_COUNT];
static bool s_is_initialized = false;

static bool is_valid_id(bsp_light_id_t light_id)
{
    return ((uint32_t)light_id < (uint32_t)BSP_LIGHT_COUNT);
}

bsp_status_t bsp_light_init(void)
{
    if (s_is_initialized)
    {
        return BSP_OK;
    }

    /* 逻辑灯号 → 定时器通道映射；换板时只改这张表。 */
    static const struct
    {
        TIM_HandleTypeDef *htim;
        uint32_t channel;
    } mapping[BSP_LIGHT_COUNT] = {
        {&htim1, TIM_CHANNEL_1}, /* UP1   PE9  */
        {&htim1, TIM_CHANNEL_2}, /* UP2   PE11 */
        {&htim1, TIM_CHANNEL_4}, /* UP3   PE14 */
        {&htim1, TIM_CHANNEL_3}, /* UP4   PE13 */
        {&htim3, TIM_CHANNEL_3}, /* DOWN1 PB0  */
        {&htim3, TIM_CHANNEL_4}, /* DOWN2 PB1  */
        {&htim3, TIM_CHANNEL_1}, /* DOWN3 PB4  */
    };

    for (uint32_t i = 0U; i < (uint32_t)BSP_LIGHT_COUNT; ++i)
    {
        bsp_status_t status = bsp_light_pwm_init(&s_lights[i], mapping[i].htim,
                                                 mapping[i].channel);
        if (status != BSP_OK)
        {
            return status;
        }
    }

    s_is_initialized = true;
    return BSP_OK;
}

bsp_status_t bsp_light_set_duty(bsp_light_id_t light_id, uint16_t duty_permille)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (!is_valid_id(light_id))
    {
        return BSP_ERR_PARAM;
    }
    return bsp_light_pwm_set_duty(&s_lights[light_id], duty_permille);
}

bsp_status_t bsp_light_stop(bsp_light_id_t light_id)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (!is_valid_id(light_id))
    {
        return BSP_ERR_PARAM;
    }
    return bsp_light_pwm_stop(&s_lights[light_id]);
}

bsp_light_pwm_t *bsp_light_handle(bsp_light_id_t light_id)
{
    if (!s_is_initialized || !is_valid_id(light_id))
    {
        return NULL;
    }
    return &s_lights[light_id];
}
