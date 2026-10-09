#include "bsp_light_pwm.h"

static bool is_valid_channel(uint32_t channel)
{
    return (channel == TIM_CHANNEL_1) || (channel == TIM_CHANNEL_2) ||
           (channel == TIM_CHANNEL_3) || (channel == TIM_CHANNEL_4);
}

bsp_status_t bsp_light_pwm_init(bsp_light_pwm_t *self,
                                TIM_HandleTypeDef *htim, uint32_t channel)
{
    if ((self == NULL) || (htim == NULL))
    {
        return BSP_ERR_PARAM;
    }
    if (!is_valid_channel(channel))
    {
        return BSP_ERR_PARAM;
    }
    if ((htim->Instance == NULL) || (htim->State == HAL_TIM_STATE_RESET))
    {
        return BSP_ERR_CONFIG;
    }

    uint32_t period_counts = htim->Init.Period + 1U;
    if ((period_counts < 2U) || (period_counts > 65536U))
    {
        return BSP_ERR_CONFIG;
    }

    self->htim = htim;
    self->channel = channel;
    self->arr = htim->Init.Period;
    self->is_started = false;
    self->is_initialized = true;
    return BSP_OK;
}

bsp_status_t bsp_light_pwm_set_duty(bsp_light_pwm_t *self,
                                    uint16_t duty_permille)
{
    if (self == NULL)
    {
        return BSP_ERR_PARAM;
    }
    if (!self->is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (duty_permille > BSP_LIGHT_PWM_DUTY_MAX)
    {
        return BSP_ERR_PARAM;
    }

    uint32_t compare =
        ((self->arr + 1U) * (uint32_t)duty_permille) / BSP_LIGHT_PWM_DUTY_MAX;
    __HAL_TIM_SET_COMPARE(self->htim, self->channel, compare);

    if (self->is_started)
    {
        return BSP_OK;
    }
    if (HAL_TIM_PWM_Start(self->htim, self->channel) != HAL_OK)
    {
        __HAL_TIM_SET_COMPARE(self->htim, self->channel, 0U);
        return BSP_ERR_HAL;
    }
    self->is_started = true;
    return BSP_OK;
}

bsp_status_t bsp_light_pwm_stop(bsp_light_pwm_t *self)
{
    if (self == NULL)
    {
        return BSP_ERR_PARAM;
    }
    if (!self->is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }

    __HAL_TIM_SET_COMPARE(self->htim, self->channel, 0U);
    return BSP_OK;
}

bsp_status_t bsp_light_pwm_deinit(bsp_light_pwm_t *self)
{
    if (self == NULL)
    {
        return BSP_ERR_PARAM;
    }
    if (!self->is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }

    if (self->is_started)
    {
        if (HAL_TIM_PWM_Stop(self->htim, self->channel) != HAL_OK)
        {
            return BSP_ERR_HAL;
        }
        self->is_started = false;
    }

    __HAL_TIM_SET_COMPARE(self->htim, self->channel, 0U);
    self->is_initialized = false;
    self->htim = NULL;
    return BSP_OK;
}
