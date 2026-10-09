#include "bsp_motor.h"

#include <stdbool.h>

#include "tim.h"

static bsp_motor_hbridge_t s_motors[BSP_MOTOR_COUNT];
static bool s_is_initialized = false;

static bool is_valid_id(bsp_motor_id_t motor_id)
{
    return ((uint32_t)motor_id < (uint32_t)BSP_MOTOR_COUNT);
}

bsp_status_t bsp_motor_init(void)
{
    if (s_is_initialized)
    {
        return BSP_OK;
    }

    /* 逻辑电机号 → 定时器 FI/BI 通道映射；换板时只改这张表。
     * M6 的 FI/BI 与引脚自然顺序相反，不要按 PC8/PC9 的顺序写。 */
    static const struct
    {
        TIM_HandleTypeDef *htim;
        uint32_t channel_fi;
        uint32_t channel_bi;
    } mapping[BSP_MOTOR_COUNT] = {
        {&htim2, TIM_CHANNEL_3, TIM_CHANNEL_4},  /* M1 PA2/PA3   */
        {&htim2, TIM_CHANNEL_1, TIM_CHANNEL_2},  /* M2 PA0/PA1   */
        {&htim9, TIM_CHANNEL_1, TIM_CHANNEL_2},  /* M3 PE5/PE6   */
        {&htim12, TIM_CHANNEL_1, TIM_CHANNEL_2}, /* M4 PB14/PB15 */
        {&htim8, TIM_CHANNEL_1, TIM_CHANNEL_2},  /* M5 PC6/PC7   */
        {&htim8, TIM_CHANNEL_4, TIM_CHANNEL_3},  /* M6 PC9/PC8   */
        {&htim4, TIM_CHANNEL_1, TIM_CHANNEL_2},  /* M7 PB6/PB7   */
        {&htim4, TIM_CHANNEL_3, TIM_CHANNEL_4},  /* M8 PB8/PB9   */
    };

    for (uint32_t i = 0U; i < (uint32_t)BSP_MOTOR_COUNT; ++i)
    {
        bsp_status_t status = bsp_motor_hbridge_init(
            &s_motors[i], mapping[i].htim, mapping[i].channel_fi,
            mapping[i].channel_bi);
        if (status != BSP_OK)
        {
            return status;
        }
    }

    s_is_initialized = true;
    return BSP_OK;
}

bsp_status_t bsp_motor_set_duty(bsp_motor_id_t motor_id, int16_t duty_permille)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (!is_valid_id(motor_id))
    {
        return BSP_ERR_PARAM;
    }
    return bsp_motor_hbridge_set_duty(&s_motors[motor_id], duty_permille);
}

bsp_status_t bsp_motor_coast(bsp_motor_id_t motor_id)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (!is_valid_id(motor_id))
    {
        return BSP_ERR_PARAM;
    }
    return bsp_motor_hbridge_coast(&s_motors[motor_id]);
}

bsp_status_t bsp_motor_brake(bsp_motor_id_t motor_id)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (!is_valid_id(motor_id))
    {
        return BSP_ERR_PARAM;
    }
    return bsp_motor_hbridge_brake(&s_motors[motor_id]);
}

bsp_status_t bsp_motor_stop_all(void)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }

    bsp_status_t first_error = BSP_OK;
    for (uint32_t i = 0U; i < (uint32_t)BSP_MOTOR_COUNT; ++i)
    {
        bsp_status_t status = bsp_motor_hbridge_coast(&s_motors[i]);
        if ((status != BSP_OK) && (first_error == BSP_OK))
        {
            first_error = status;
        }
    }
    return first_error;
}

bsp_motor_hbridge_t *bsp_motor_handle(bsp_motor_id_t motor_id)
{
    if (!s_is_initialized || !is_valid_id(motor_id))
    {
        return NULL;
    }
    return &s_motors[motor_id];
}
