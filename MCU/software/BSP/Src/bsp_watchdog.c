#include "bsp_watchdog.h"

#include "main.h"

/* IWDG_KR 键值，见 RM0090 IWDG 章节。 */
#define IWDG_KEY_RELOAD (0xAAAAU)
#define IWDG_KEY_WRITE_ACCESS (0x5555U)
#define IWDG_KEY_START (0xCCCCU)

/* 标称 LSI 频率 32kHz；预分频 /4 时每个计数 1/8 ms。 */
#define LSI_TICKS_PER_MS_DIV4 (8U)

/* 重装载值最大 4095，即每档最多 4096 个计数。 */
#define IWDG_RELOAD_COUNTS (4096U)

/* 预分频编码最大值：6 对应 /256。 */
#define IWDG_PRESCALER_MAX (6U)

/* 等待 PR/RLR 更新完成的上限；LSI 最慢时约需 6 个 LSI 周期乘以分频，远小于此值。 */
#define IWDG_UPDATE_TIMEOUT_MS (50U)

static bool s_is_running = false;
static uint32_t s_timeout_ms = 0U;

/** @brief 写入能覆盖 timeout_ms 的最小预分频和对应重装载值，并等待生效。 */
static bsp_status_t configure(uint32_t timeout_ms)
{
    /* /4 时每档 512ms，分频每加一级翻倍；选能放下 timeout_ms 的最小分频。 */
    uint32_t prescaler = 0U;
    uint32_t ms_per_count_x8 = 1U; /* 每个计数的毫秒数乘以 8 */
    while ((timeout_ms * LSI_TICKS_PER_MS_DIV4 / ms_per_count_x8 >
            IWDG_RELOAD_COUNTS) &&
           (prescaler < IWDG_PRESCALER_MAX))
    {
        ++prescaler;
        ms_per_count_x8 *= 2U;
    }
    uint32_t counts = timeout_ms * LSI_TICKS_PER_MS_DIV4 / ms_per_count_x8;
    if (counts == 0U)
    {
        counts = 1U;
    }

    IWDG->KR = IWDG_KEY_WRITE_ACCESS;
    IWDG->PR = prescaler;
    IWDG->RLR = counts - 1U;

    uint32_t start_ms = HAL_GetTick();
    while (IWDG->SR != 0U)
    {
        if ((uint32_t)(HAL_GetTick() - start_ms) >= IWDG_UPDATE_TIMEOUT_MS)
        {
            return BSP_ERR_TIMEOUT;
        }
    }
    IWDG->KR = IWDG_KEY_RELOAD;
    return BSP_OK;
}

bsp_status_t bsp_watchdog_start(uint32_t timeout_ms)
{
    if ((timeout_ms == 0U) || (timeout_ms > BSP_WATCHDOG_MAX_TIMEOUT_MS))
    {
        return BSP_ERR_PARAM;
    }

    /* 内核被调试器暂停时冻结 IWDG，断点停留不会触发复位。 */
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;

    IWDG->KR = IWDG_KEY_START;
    s_is_running = true;
    s_timeout_ms = timeout_ms;
    return configure(timeout_ms);
}

bsp_status_t bsp_watchdog_set_timeout(uint32_t timeout_ms)
{
    if (!s_is_running)
    {
        return BSP_IDLE;
    }
    if ((timeout_ms == 0U) || (timeout_ms > BSP_WATCHDOG_MAX_TIMEOUT_MS))
    {
        return BSP_ERR_PARAM;
    }
    s_timeout_ms = timeout_ms;
    return configure(timeout_ms);
}

uint32_t bsp_watchdog_timeout(void)
{
    return s_is_running ? s_timeout_ms : 0U;
}

void bsp_watchdog_feed(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}

bool bsp_watchdog_take_reset_flag(void)
{
    bool is_watchdog_reset = ((RCC->CSR & RCC_CSR_IWDGRSTF) != 0U);
    RCC->CSR |= RCC_CSR_RMVF;
    return is_watchdog_reset;
}
