#include "bsp_motor.h"
#include "motor_limits.h"

#include <stdbool.h>

#include "tim.h"

_Static_assert(BSP_MOTOR_COUNT == MOTOR_COUNT,
               "motor_limits.h and bsp_motor.h disagree on the motor count");

/* 逻辑电机号 → 定时器 FI/BI 通道映射；换板时只改这张表。
 * M6 的 FI/BI 与引脚自然顺序相反，不要按 PC8/PC9 的顺序写。
 * 放在文件作用域，急停时可以不经过 bsp_motor_init() 直接使用。 */
static const struct
{
    TIM_HandleTypeDef *htim;
    uint32_t channel_fi;
    uint32_t channel_bi;
} k_mapping[BSP_MOTOR_COUNT] = {
    {&htim2, TIM_CHANNEL_3, TIM_CHANNEL_4},  /* M1 PA2/PA3   */
    {&htim2, TIM_CHANNEL_1, TIM_CHANNEL_2},  /* M2 PA0/PA1   */
    {&htim9, TIM_CHANNEL_1, TIM_CHANNEL_2},  /* M3 PE5/PE6   */
    {&htim12, TIM_CHANNEL_1, TIM_CHANNEL_2}, /* M4 PB14/PB15 */
    {&htim8, TIM_CHANNEL_1, TIM_CHANNEL_2},  /* M5 PC6/PC7   */
    {&htim8, TIM_CHANNEL_4, TIM_CHANNEL_3},  /* M6 PC9/PC8   */
    {&htim4, TIM_CHANNEL_1, TIM_CHANNEL_2},  /* M7 PB6/PB7   */
    {&htim4, TIM_CHANNEL_3, TIM_CHANNEL_4},  /* M8 PB8/PB9   */
};

static bsp_motor_hbridge_t s_motors[BSP_MOTOR_COUNT];
static bool s_is_initialized = false;
/** @brief 初始化时各路定时器的预分频，对应 BSP_MOTOR_PWM_BASE_KHZ。 */
static uint32_t s_base_prescaler[BSP_MOTOR_COUNT];
static uint32_t s_pwm_khz = BSP_MOTOR_PWM_BASE_KHZ;
static bool s_is_slow_decay = false;

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

    for (uint32_t i = 0U; i < (uint32_t)BSP_MOTOR_COUNT; ++i)
    {
        bsp_status_t status = bsp_motor_hbridge_init(
            &s_motors[i], k_mapping[i].htim, k_mapping[i].channel_fi,
            k_mapping[i].channel_bi);
        if (status != BSP_OK)
        {
            return status;
        }
        s_base_prescaler[i] = k_mapping[i].htim->Instance->PSC;
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
    if (!motor_duty_valid((unsigned)motor_id, duty_permille))
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

void bsp_motor_emergency_stop(void)
{
    for (uint32_t i = 0U; i < (uint32_t)BSP_MOTOR_COUNT; ++i)
    {
        TIM_HandleTypeDef *htim = k_mapping[i].htim;
        if (htim->Instance == NULL)
        {
            continue; /* MX_TIMx_Init() 尚未执行，PWM 也还没输出 */
        }
        __HAL_TIM_SET_COMPARE(htim, k_mapping[i].channel_fi, 0U);
        __HAL_TIM_SET_COMPARE(htim, k_mapping[i].channel_bi, 0U);
    }
}

bsp_status_t bsp_motor_set_pwm(uint32_t khz, bool slow_decay)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if ((khz == 0U) || (khz > BSP_MOTOR_PWM_BASE_KHZ) ||
        ((BSP_MOTOR_PWM_BASE_KHZ % khz) != 0U))
    {
        return BSP_ERR_PARAM;
    }
    for (uint32_t i = 0U; i < (uint32_t)BSP_MOTOR_COUNT; ++i)
    {
        if (s_motors[i].duty_permille != 0)
        {
            return BSP_ERR_CONFIG;
        }
    }

    uint32_t divider = BSP_MOTOR_PWM_BASE_KHZ / khz;
    for (uint32_t i = 0U; i < (uint32_t)BSP_MOTOR_COUNT; ++i)
    {
        uint32_t prescaler = (s_base_prescaler[i] + 1U) * divider - 1U;
        if (prescaler > 0xFFFFU)
        {
            return BSP_ERR_PARAM;
        }
    }
    for (uint32_t i = 0U; i < (uint32_t)BSP_MOTOR_COUNT; ++i)
    {
        TIM_HandleTypeDef *htim = k_mapping[i].htim;
        /* 同一定时器上的两路电机会各写一次，值相同。 */
        __HAL_TIM_SET_PRESCALER(htim, (s_base_prescaler[i] + 1U) * divider - 1U);
        htim->Instance->EGR = TIM_EGR_UG;
        (void)bsp_motor_hbridge_set_decay(&s_motors[i], slow_decay);
    }
    s_pwm_khz = khz;
    s_is_slow_decay = slow_decay;
    return BSP_OK;
}

void bsp_motor_get_pwm(uint32_t *khz, bool *slow_decay)
{
    *khz = s_pwm_khz;
    *slow_decay = s_is_slow_decay;
}

bsp_motor_hbridge_t *bsp_motor_handle(bsp_motor_id_t motor_id)
{
    if (!s_is_initialized || !is_valid_id(motor_id))
    {
        return NULL;
    }
    return &s_motors[motor_id];
}
