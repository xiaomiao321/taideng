/**
  ******************************************************************************
  * @file    rz7899.c
  * @brief   RZ7899 H 桥电机驱动库实现 (双路 PWM 控制)
  *
  * 设计要点:
  *   1. 只操作 CCR, 不动 PSC/ARR -> PWM 频率完全由 CubeMX 决定;
  *   2. 除制动外, 任何时刻最多只有一路通道输出非零占空比;
  *   3. 换向前先归零并等待电流衰减, 再切换到新方向;
  *   4. 所有接口非阻塞 (函数内不含任何延时): 接口只设定目标, 换向死区 /
  *      启动助推 / 斜坡 均由 RZ7899_Update() 逐 PWM 周期推进 (必须放在定时器
  *      更新中断里, 每次调用 = 一个 PWM 周期);
  *   5. 软停止: RZ7899_Stop(COAST) 先按斜坡减速到 0 (停止斜坡时间, 默认 100ms);
  *   6. 斜坡"时间预算": 斜坡进行中改目标不重新计时, 按剩余预算
  *      (ramp_total - cycles) 从当前占空比重新瞄准;
  *   7. 自动频率: RZ7899_Init() 从 htim 读出 PSC/ARR 与定时器时钟, 算出实际
  *      PWM 频率; 启停时间(ms) 由它换算成周期数, 改 CubeMX 后无需手动同步.
  ******************************************************************************
  */

#include "rz7899.h"

/* 说明: 公开接口的完整文档 (参数/返回值/用法) 在 rz7899.h 中;
   本文件保留实现细节注释与私有函数说明. */

/* -------------------------------------------------------------------------- */
/* 私有函数                                                                    */
/* -------------------------------------------------------------------------- */

/**
  * @brief  把 0 ~ 1000 的速度刻度换算成 CCR 并写入指定通道
  * @param  motor   电机句柄 (period_counts 已缓存)
  * @param  channel PWM 通道
  * @param  duty    占空比刻度 0 ~ RZ7899_SPEED_MAX
  * @note   CCR = (ARR + 1) * duty / 1000
  *         duty = 1000 时 CCR = ARR + 1, 计数器最大只到 ARR, 因此输出恒为
  *         有效电平, 即真正的 100%.
  */
static void rz7899_write_duty(RZ7899_Handle *motor, uint32_t channel, uint16_t duty)
{
    uint32_t pulse = ((uint32_t)motor->period_counts * (uint32_t)duty) / (uint32_t)RZ7899_SPEED_MAX;

    __HAL_TIM_SET_COMPARE(motor->htim, channel, pulse);
}

/**
  * @brief  指定通道输出 0% (恒为无效电平)
  */
static void rz7899_write_zero(RZ7899_Handle *motor, uint32_t channel)
{
    __HAL_TIM_SET_COMPARE(motor->htim, channel, 0U);
}

/**
  * @brief  指定通道输出 100% (恒为有效电平)
  * @note   CCR 需要等于 ARR + 1 而不是 ARR; 写 ARR 只能得到 (ARR)/(ARR+1).
  */
static void rz7899_write_full(RZ7899_Handle *motor, uint32_t channel)
{
    __HAL_TIM_SET_COMPARE(motor->htim, channel, motor->period_counts);
}

/**
  * @brief  把指令速度刻度映射到实际输出占空比刻度
  * @note   实际 = min_duty + (max_duty - min_duty) * speed / 1000
  *         这样指令 1 就能越过低速死区, 指令 1000 也不会超过高速限幅.
  */
static uint16_t rz7899_map_duty(const RZ7899_Handle *motor, uint16_t speed)
{
    uint32_t span = (uint32_t)motor->max_duty - (uint32_t)motor->min_duty;

    return (uint16_t)((uint32_t)motor->min_duty + (span * (uint32_t)speed) / (uint32_t)RZ7899_SPEED_MAX);
}

/**
  * @brief  取当前方向对应的 PWM 通道
  */
static uint32_t rz7899_active_channel(const RZ7899_Handle *motor)
{
    return (motor->direction == RZ7899_DIR_FORWARD) ? motor->channel_forward : motor->channel_reverse;
}

/**
  * @brief  取某个 TIM 实例的输入时钟频率 (Hz)
  * @note   STM32F1: TIM1/TIM8 在 APB2, TIM2~TIM7 在 APB1;
  *         APB 分频系数不为 1 时, 定时器时钟 = 2 × PCLK (F1 的 ×2 规则).
  *         读的是 RCC 当前实际配置, 与 SystemClock_Config 保持一致.
  */
static uint32_t rz7899_timer_clock_hz(TIM_TypeDef *instance)
{
    uint32_t pclk;
    bool on_apb2 = false;

#if defined(TIM1)
    if (instance == TIM1)
    {
        on_apb2 = true;
    }
#endif
#if defined(TIM8)
    if (instance == TIM8)
    {
        on_apb2 = true;
    }
#endif
#if defined(TIM9)
    if (instance == TIM9)
    {
        on_apb2 = true;
    }
#endif
#if defined(TIM10)
    if (instance == TIM10)
    {
        on_apb2 = true;
    }
#endif
#if defined(TIM11)
    if (instance == TIM11)
    {
        on_apb2 = true;
    }
#endif

    pclk = on_apb2 ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();

    /* APB 分频 = 1 -> 定时器时钟 = PCLK; 否则 = 2 × PCLK */
    return (pclk == HAL_RCC_GetHCLKFreq()) ? pclk : (pclk * 2U);
}

/* -------------------------------------------------------------------------- */
/* 接口实现                                                                    */
/* -------------------------------------------------------------------------- */

bool RZ7899_Init(RZ7899_Handle *motor,
                 TIM_HandleTypeDef *htim,
                 uint32_t forward_channel,
                 uint32_t reverse_channel)
{
    uint32_t period;

    if ((motor == NULL) || (htim == NULL) || (htim->Instance == NULL))
    {
        return false;
    }
    if (forward_channel == reverse_channel)
    {
        return false;
    }

    /* 防御性检查: 若定时器尚未初始化, ARR 为 0, 换算不出有效占空比.
       正常电机 PWM 的 ARR 不会只有 0~1 (那是 36MHz 级的载波). */
    period = __HAL_TIM_GET_AUTORELOAD(htim) + 1U;
    if (period < 2U)
    {
        return false;
    }

    motor->htim = htim;
    motor->channel_forward = forward_channel;
    motor->channel_reverse = reverse_channel;
    motor->period_counts = period;
    motor->min_duty = 0U;
    motor->max_duty = (uint16_t)RZ7899_SPEED_MAX;
    motor->direction = RZ7899_DIR_STOP;
    motor->speed = 0U;
    motor->duty = 0U;
    motor->state = RZ7899_STATE_IDLE;
    motor->cycles = 0U;
    motor->target_duty = 0U;
    motor->ramp_start_duty = 0U;
    motor->ramp_total = 0U;
    motor->stopping = false;
    motor->soft_start = false;

    /* 自动读出实际 PWM 频率: 定时器时钟 / ((PSC+1) × (ARR+1))
       PSC/ARR 读寄存器, 时钟读 RCC 当前配置 -> 改 CubeMX 后无需手动同步 */
    {
        uint32_t psc = (uint32_t)(htim->Instance->PSC) + 1U;
        uint32_t freq = rz7899_timer_clock_hz(htim->Instance) / psc / period;

        motor->pwm_freq_hz = (freq == 0U) ? 1U : freq;
    }

    /* 把默认的启停时间 (ms) 换算成周期数 (运行时可用 RZ7899_SetRampTimes 再改) */
    (void)RZ7899_SetRampTimes(motor, (uint16_t)RZ7899_START_RAMP_MS, (uint16_t)RZ7899_STOP_RAMP_MS);

    /* 先归零再启动: 即使 CubeMX 的初始 Pulse 不为 0, 上电也不会转动 */
    rz7899_write_zero(motor, motor->channel_forward);
    rz7899_write_zero(motor, motor->channel_reverse);

    if (HAL_TIM_PWM_Start(htim, forward_channel) != HAL_OK)
    {
        return false;
    }
    if (HAL_TIM_PWM_Start(htim, reverse_channel) != HAL_OK)
    {
        /* 回滚: 避免留下单通道运行的中间状态 */
        (void)HAL_TIM_PWM_Stop(htim, forward_channel);
        return false;
    }

    return true;
}

bool RZ7899_SetDutyLimits(RZ7899_Handle *motor, uint16_t min_duty, uint16_t max_duty)
{
    if ((motor == NULL) || (motor->htim == NULL))
    {
        return false;
    }
    if ((min_duty > max_duty) || (max_duty > (uint16_t)RZ7899_SPEED_MAX))
    {
        return false;
    }

    motor->min_duty = min_duty;
    motor->max_duty = max_duty;

    return true;
}

bool RZ7899_SetRampTimes(RZ7899_Handle *motor, uint16_t start_ms, uint16_t stop_ms)
{
    uint32_t cycles;

    if ((motor == NULL) || (motor->htim == NULL))
    {
        return false;
    }

    /* 周期数 = 时间(ms) × 实际频率(Hz) / 1000; 0 视为 1 个周期 */
    cycles = ((uint32_t)start_ms * motor->pwm_freq_hz) / 1000U;
    motor->ramp_start_cycles = (cycles == 0U) ? 1U : cycles;

    cycles = ((uint32_t)stop_ms * motor->pwm_freq_hz) / 1000U;
    motor->ramp_stop_cycles = (cycles == 0U) ? 1U : cycles;

    return true;
}

/**
  * @brief  统一的速度指令处理 (非阻塞): 只设定目标, 必要时启动换向死区等待
  * @param  soft true = 软启动 (由 Update 斜坡逼近目标); false = 立即到达
  */
static void rz7899_begin(RZ7899_Handle *motor, int16_t speed, bool soft)
{
    RZ7899_Direction target;
    uint16_t speed_scale;
    uint16_t target_duty;

    if ((motor == NULL) || (motor->htim == NULL))
    {
        return;
    }

    /* 1. 限幅: 只允许 -1000 ~ +1000 */
    if (speed > (int16_t)RZ7899_SPEED_MAX)
    {
        speed = (int16_t)RZ7899_SPEED_MAX;
    }
    else if (speed < (int16_t)(-RZ7899_SPEED_MAX))
    {
        speed = (int16_t)(-RZ7899_SPEED_MAX);
    }

    /* 2. 速度为 0 等价于软停止 (斜坡降到 0 后滑行) */
    if (speed == 0)
    {
        RZ7899_Stop(motor, RZ7899_STOP_COAST);
        return;
    }

    target = (speed > 0) ? RZ7899_DIR_FORWARD : RZ7899_DIR_REVERSE;
    speed_scale = (uint16_t)((speed > 0) ? speed : -speed);
    target_duty = rz7899_map_duty(motor, speed_scale); /* 低速死区 / 高速限幅映射 */

    /* 已在斜坡逼近同一目标: 只刷新标志, 不重置斜坡进度/预算
       (防止高频重复调用 Start 导致斜坡永远走不完) */
    if ((motor->state == RZ7899_STATE_RAMP) && (!motor->stopping) &&
        (motor->target_duty == target_duty))
    {
        motor->speed = speed_scale;
        motor->soft_start = soft;
        return;
    }

    /* 记录目标 (若正在换向/助推, 只更新目标, 由 RZ7899_Update 继续推进) */
    motor->speed = speed_scale;
    motor->target_duty = target_duty;
    motor->soft_start = soft;

    /* 3. 方向改变 (含从停止启动): 两路先归零, 进入换向死区等待 */
    if (motor->direction != target)
    {
        rz7899_write_zero(motor, motor->channel_forward);
        rz7899_write_zero(motor, motor->channel_reverse);
        motor->direction = target;
        motor->duty = 0U;
        motor->stopping = false;
        motor->state = RZ7899_STATE_SWITCH;
        motor->cycles = 0U;
        return;
    }

    if ((motor->state == RZ7899_STATE_SWITCH) || (motor->state == RZ7899_STATE_KICK))
    {
        return; /* 启动过程中: 目标已更新, 交给 RZ7899_Update() */
    }

    if (soft)
    {
        if (motor->state == RZ7899_STATE_RAMP)
        {
            /* 斜坡进行中改变目标: 不重新计时, "做减法" -
               剩余预算 = 总预算 - 已用周期, 从当前占空比在剩余预算内
               重新瞄准新目标 (连续指令不会把总时长越拖越长) */
            uint32_t remaining = motor->ramp_total - motor->cycles;

            if (remaining == 0U)
            {
                remaining = 1U; /* 防御: 视为一个周期内到位 */
            }
            motor->ramp_start_duty = motor->duty;
            motor->cycles = 0U;
            motor->ramp_total = remaining;
            motor->stopping = false; /* 中途改为速度指令: 不再是停止斜坡 */
        }
        else
        {
            /* 新的一段软变: 从当前占空比出发, 使用完整预算 */
            motor->state = RZ7899_STATE_RAMP;
            motor->ramp_start_duty = motor->duty;
            motor->ramp_total = motor->ramp_start_cycles;
            motor->cycles = 0U;
            motor->stopping = false;
        }
    }
    else
    {
        /* 同方向硬变: 立即到位 (先写对面通道 0%, 顺序不可颠倒) */
        rz7899_write_zero(motor, (target == RZ7899_DIR_FORWARD) ? motor->channel_reverse : motor->channel_forward);
        rz7899_write_duty(motor, rz7899_active_channel(motor), target_duty);
        motor->duty = target_duty;
        motor->stopping = false; /* 立即到达会取消可能存在的停止斜坡 */
        motor->state = RZ7899_STATE_RUN;
    }
}

void RZ7899_SetSpeed(RZ7899_Handle *motor, int16_t speed)
{
    rz7899_begin(motor, speed, false);
}

void RZ7899_Start(RZ7899_Handle *motor, int16_t speed)
{
    rz7899_begin(motor, speed, true);
}

void RZ7899_Update(RZ7899_Handle *motor)
{
    if ((motor == NULL) || (motor->htim == NULL))
    {
        return;
    }

    switch (motor->state)
    {
    case RZ7899_STATE_SWITCH:
        /* 换向死区: 两路保持 0%, 等绕组电流衰减 (RZ7899_DIR_SWITCH_CYCLES 个周期) */
        motor->cycles++;
        if (motor->cycles < (uint32_t)RZ7899_DIR_SWITCH_CYCLES)
        {
            break;
        }
        motor->cycles = 0U;

        if ((motor->soft_start) && (RZ7899_START_KICK_CYCLES > 0U))
        {
            uint16_t kick = (uint16_t)RZ7899_START_KICK_DUTY;

            if (kick > (uint16_t)RZ7899_SPEED_MAX)
            {
                kick = (uint16_t)RZ7899_SPEED_MAX;
            }
            rz7899_write_duty(motor, rz7899_active_channel(motor), kick);
            motor->duty = kick;
            motor->state = RZ7899_STATE_KICK;
        }
        else if (motor->soft_start)
        {
            motor->ramp_start_duty = motor->duty; /* 当前为 0 */
            motor->ramp_total = motor->ramp_start_cycles;
            motor->stopping = false;
            motor->state = RZ7899_STATE_RAMP;
        }
        else
        {
            /* 立即到达 (RZ7899_SetSpeed 的换向路径) */
            rz7899_write_duty(motor, rz7899_active_channel(motor), motor->target_duty);
            motor->duty = motor->target_duty;
            motor->stopping = false;
            motor->state = RZ7899_STATE_RUN;
        }
        break;

    case RZ7899_STATE_KICK:
        /* 启动助推: 满占空比冲若干周期后回到 0, 再进入斜坡 */
        motor->cycles++;
        if (motor->cycles < (uint32_t)RZ7899_START_KICK_CYCLES)
        {
            break;
        }
        motor->cycles = 0U;
        rz7899_write_duty(motor, rz7899_active_channel(motor), 0U);
        motor->duty = 0U;
        motor->ramp_start_duty = 0U;
        motor->ramp_total = motor->ramp_start_cycles;
        motor->stopping = false;
        motor->state = RZ7899_STATE_RAMP;
        break;

    case RZ7899_STATE_RAMP:
        /* 斜坡: 在 ramp_total 个 PWM 周期内从 ramp_start_duty 线性逼近目标
           (目标可以是速度, 也可以是软停止的 0; 中途改目标按剩余预算重规划).
           每周期: duty = start + (target - start) × elapsed / total
           — 用绝对位置公式而不是逐周期累加, 取整误差不累积 */
        motor->cycles++;

        {
            uint32_t total = motor->ramp_total;
            int32_t start = (int32_t)motor->ramp_start_duty;
            int32_t target = (int32_t)motor->target_duty;
            int32_t elapsed = (int32_t)motor->cycles;
            int32_t duty;

            if (total == 0U)
            {
                total = 1U; /* 防御: 0 视为一个周期内到位 */
            }

            if (elapsed >= (int32_t)total)
            {
                duty = target;
                motor->duty = (uint16_t)duty;
                motor->cycles = 0U;

                if (motor->stopping)
                {
                    /* 软停止完成: 两路归零, 进入滑行待机 */
                    rz7899_write_zero(motor, motor->channel_forward);
                    rz7899_write_zero(motor, motor->channel_reverse);
                    motor->stopping = false;
                    motor->direction = RZ7899_DIR_STOP;
                    motor->speed = 0U;
                    motor->state = RZ7899_STATE_IDLE;
                }
                else
                {
                    rz7899_write_duty(motor, rz7899_active_channel(motor), motor->duty);
                    motor->state = RZ7899_STATE_RUN;
                }
            }
            else
            {
                duty = start + (((target - start) * elapsed) / (int32_t)total);
                motor->duty = (uint16_t)duty;
                rz7899_write_duty(motor, rz7899_active_channel(motor), motor->duty);
            }
        }
        break;

    case RZ7899_STATE_RUN:
    case RZ7899_STATE_IDLE:
    default:
        break;
    }
}

void RZ7899_Stop(RZ7899_Handle *motor, RZ7899_StopMode mode)
{
    if ((motor == NULL) || (motor->htim == NULL))
    {
        return;
    }

    if (mode == RZ7899_STOP_BRAKE)
    {
        /* ---- 急停: 立即短路制动, 不经过斜坡 ---- */
        /* FI=BI=1: 桥臂把电机两端短接, 反电动势产生制动转矩 */
        rz7899_write_full(motor, motor->channel_forward);
        rz7899_write_full(motor, motor->channel_reverse);

        motor->direction = RZ7899_DIR_STOP;
        motor->speed = 0U;
        motor->duty = 0U;
        motor->target_duty = 0U;
        motor->ramp_start_duty = 0U;
        motor->ramp_total = 0U;
        motor->stopping = false;
        motor->soft_start = false;
        motor->state = RZ7899_STATE_IDLE;
        motor->cycles = 0U;
        return;
    }

    /* ---- 软停止 (滑行): 先斜坡减速到 0, 再输出高阻 ---- */

    /* 已经停止 / 没有输出 (含换向死区、制动保持): 直接归零待机 */
    if ((motor->state == RZ7899_STATE_IDLE) || (motor->duty == 0U))
    {
        rz7899_write_zero(motor, motor->channel_forward);
        rz7899_write_zero(motor, motor->channel_reverse);

        motor->direction = RZ7899_DIR_STOP;
        motor->speed = 0U;
        motor->duty = 0U;
        motor->target_duty = 0U;
        motor->ramp_start_duty = 0U;
        motor->ramp_total = 0U;
        motor->stopping = false;
        motor->soft_start = false;
        motor->state = RZ7899_STATE_IDLE;
        motor->cycles = 0U;
        return;
    }

    if (motor->state == RZ7899_STATE_RAMP)
    {
        /* 斜坡进行中: 复用剩余预算, 防止"先减速再停止"总时长翻倍 */
        uint32_t remaining = motor->ramp_total - motor->cycles;

        motor->ramp_total = (remaining == 0U) ? 1U : remaining;
    }
    else
    {
        /* KICK / RUN: 开一段完整的新预算 */
        motor->ramp_total = motor->ramp_stop_cycles;
    }

    motor->state = RZ7899_STATE_RAMP;
    motor->ramp_start_duty = motor->duty; /* 从当前占空比开始降 */
    motor->target_duty = 0U;
    motor->cycles = 0U;
    motor->stopping = true;
    motor->speed = 0U; /* 指令速度归 0, 方向保持到减速结束 */
    rz7899_write_duty(motor, rz7899_active_channel(motor), motor->duty);
}

void RZ7899_DeInit(RZ7899_Handle *motor)
{
    if ((motor == NULL) || (motor->htim == NULL))
    {
        return;
    }

    /* 先把占空比归零再关通道, 避免关断瞬间输出停留在有效电平 */
    rz7899_write_zero(motor, motor->channel_forward);
    rz7899_write_zero(motor, motor->channel_reverse);

    (void)HAL_TIM_PWM_Stop(motor->htim, motor->channel_forward);
    (void)HAL_TIM_PWM_Stop(motor->htim, motor->channel_reverse);

    motor->direction = RZ7899_DIR_STOP;
    motor->speed = 0U;
    motor->duty = 0U;
    motor->target_duty = 0U;
    motor->ramp_start_duty = 0U;
    motor->ramp_total = 0U;
    motor->stopping = false;
    motor->soft_start = false;
    motor->state = RZ7899_STATE_IDLE;
    motor->cycles = 0U;
    motor->pwm_freq_hz = 0U;
    motor->ramp_start_cycles = 0U;
    motor->ramp_stop_cycles = 0U;
    motor->htim = NULL;
}

RZ7899_Direction RZ7899_GetDirection(const RZ7899_Handle *motor)
{
    return (motor != NULL) ? motor->direction : RZ7899_DIR_STOP;
}

uint16_t RZ7899_GetSpeed(const RZ7899_Handle *motor)
{
    return (motor != NULL) ? motor->speed : 0U;
}
