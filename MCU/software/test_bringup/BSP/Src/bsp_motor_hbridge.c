#include "bsp_motor_hbridge.h"

static bool is_valid_channel(uint32_t channel)
{
    return (channel == TIM_CHANNEL_1) || (channel == TIM_CHANNEL_2) ||
           (channel == TIM_CHANNEL_3) || (channel == TIM_CHANNEL_4);
}

static uint32_t duty_to_compare(uint32_t arr, uint32_t duty_permille)
{
    return ((arr + 1U) * duty_permille) / 1000U;
}

bsp_status_t bsp_motor_hbridge_init(bsp_motor_hbridge_t *self,
                                    TIM_HandleTypeDef *htim,
                                    uint32_t channel_fi, uint32_t channel_bi)
{
    if ((self == NULL) || (htim == NULL))
    {
        return BSP_ERR_PARAM;
    }
    if ((!is_valid_channel(channel_fi)) || (!is_valid_channel(channel_bi)) ||
        (channel_fi == channel_bi))
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
    self->channel_fi = channel_fi;
    self->channel_bi = channel_bi;
    self->arr = htim->Init.Period;
    self->duty_permille = 0;
    self->is_slow_decay = false;
    self->is_initialized = true;

    /* 两路先清零再启动，使引脚确定输出低电平（滑行），而不是浮空。 */
    __HAL_TIM_SET_COMPARE(self->htim, self->channel_fi, 0U);
    __HAL_TIM_SET_COMPARE(self->htim, self->channel_bi, 0U);
    if (HAL_TIM_PWM_Start(self->htim, self->channel_fi) != HAL_OK)
    {
        self->is_initialized = false;
        return BSP_ERR_HAL;
    }
    if (HAL_TIM_PWM_Start(self->htim, self->channel_bi) != HAL_OK)
    {
        (void)HAL_TIM_PWM_Stop(self->htim, self->channel_fi);
        self->is_initialized = false;
        return BSP_ERR_HAL;
    }
    return BSP_OK;
}

bsp_status_t bsp_motor_hbridge_set_duty(bsp_motor_hbridge_t *self,
                                        int16_t duty_permille)
{
    if (self == NULL)
    {
        return BSP_ERR_PARAM;
    }
    if (!self->is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if ((duty_permille < BSP_MOTOR_HBRIDGE_DUTY_MIN) ||
        (duty_permille > BSP_MOTOR_HBRIDGE_DUTY_MAX))
    {
        return BSP_ERR_PARAM;
    }

    uint32_t full =
        duty_to_compare(self->arr, (uint32_t)BSP_MOTOR_HBRIDGE_DUTY_MAX);
    if (duty_permille > 0)
    {
        uint32_t on = duty_to_compare(self->arr, (uint32_t)duty_permille);
        /* 先把反转通道清零，再给正转通道赋值，避免两路同时导通。
         * 刹车模式下 FI 常高、BI 在关断段拉高，两路同高为短路制动。 */
        __HAL_TIM_SET_COMPARE(self->htim, self->channel_bi, 0U);
        __HAL_TIM_SET_COMPARE(self->htim, self->channel_fi,
                              self->is_slow_decay ? full : on);
        if (self->is_slow_decay)
        {
            __HAL_TIM_SET_COMPARE(self->htim, self->channel_bi, full - on);
        }
    }
    else if (duty_permille < 0)
    {
        uint32_t on = duty_to_compare(self->arr, (uint32_t)(-duty_permille));
        __HAL_TIM_SET_COMPARE(self->htim, self->channel_fi, 0U);
        __HAL_TIM_SET_COMPARE(self->htim, self->channel_bi,
                              self->is_slow_decay ? full : on);
        if (self->is_slow_decay)
        {
            __HAL_TIM_SET_COMPARE(self->htim, self->channel_fi, full - on);
        }
    }
    else
    {
        __HAL_TIM_SET_COMPARE(self->htim, self->channel_fi, 0U);
        __HAL_TIM_SET_COMPARE(self->htim, self->channel_bi, 0U);
    }

    self->duty_permille = duty_permille;
    return BSP_OK;
}

bsp_status_t bsp_motor_hbridge_set_decay(bsp_motor_hbridge_t *self,
                                         bool slow_decay)
{
    if (self == NULL)
    {
        return BSP_ERR_PARAM;
    }
    if (!self->is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (self->duty_permille != 0)
    {
        return BSP_ERR_CONFIG;
    }

    self->is_slow_decay = slow_decay;
    return BSP_OK;
}

bsp_status_t bsp_motor_hbridge_coast(bsp_motor_hbridge_t *self)
{
    if (self == NULL)
    {
        return BSP_ERR_PARAM;
    }
    if (!self->is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }

    __HAL_TIM_SET_COMPARE(self->htim, self->channel_fi, 0U);
    __HAL_TIM_SET_COMPARE(self->htim, self->channel_bi, 0U);
    self->duty_permille = 0;
    return BSP_OK;
}

bsp_status_t bsp_motor_hbridge_brake(bsp_motor_hbridge_t *self)
{
    if (self == NULL)
    {
        return BSP_ERR_PARAM;
    }
    if (!self->is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }

    uint32_t full =
        duty_to_compare(self->arr, (uint32_t)BSP_MOTOR_HBRIDGE_DUTY_MAX);
    __HAL_TIM_SET_COMPARE(self->htim, self->channel_fi, full);
    __HAL_TIM_SET_COMPARE(self->htim, self->channel_bi, full);
    self->duty_permille = 0;
    return BSP_OK;
}

bsp_status_t bsp_motor_hbridge_get_duty(const bsp_motor_hbridge_t *self,
                                        int16_t *duty_permille)
{
    if ((self == NULL) || (duty_permille == NULL))
    {
        return BSP_ERR_PARAM;
    }
    if (!self->is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }

    *duty_permille = self->duty_permille;
    return BSP_OK;
}

bsp_status_t bsp_motor_hbridge_deinit(bsp_motor_hbridge_t *self)
{
    if (self == NULL)
    {
        return BSP_ERR_PARAM;
    }
    if (!self->is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }

    __HAL_TIM_SET_COMPARE(self->htim, self->channel_fi, 0U);
    __HAL_TIM_SET_COMPARE(self->htim, self->channel_bi, 0U);
    if ((HAL_TIM_PWM_Stop(self->htim, self->channel_fi) != HAL_OK) ||
        (HAL_TIM_PWM_Stop(self->htim, self->channel_bi) != HAL_OK))
    {
        return BSP_ERR_HAL;
    }

    self->is_initialized = false;
    self->htim = NULL;
    self->duty_permille = 0;
    return BSP_OK;
}
