#include "bsp_watchdog.h"

#include "main.h"

/* IWDG_KR 键值，见 RM0090 IWDG 章节。 */
#define IWDG_KEY_RELOAD (0xAAAAU)
#define IWDG_KEY_WRITE_ACCESS (0x5555U)
#define IWDG_KEY_START (0xCCCCU)

/* 预分频 /32：标称 32kHz LSI 下每个计数约 1ms。 */
#define IWDG_PRESCALER_DIV32 (0x3U)

/* 等待 PR/RLR 更新完成的上限；LSI 最慢时约需 6 个 LSI 周期乘以 /32，远小于此值。 */
#define IWDG_UPDATE_TIMEOUT_MS (50U)

bsp_status_t bsp_watchdog_start(uint32_t timeout_ms)
{
    if ((timeout_ms == 0U) || (timeout_ms > BSP_WATCHDOG_MAX_TIMEOUT_MS))
    {
        return BSP_ERR_PARAM;
    }

    /* 内核被调试器暂停时冻结 IWDG，断点停留不会触发复位。 */
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;

    IWDG->KR = IWDG_KEY_START;
    IWDG->KR = IWDG_KEY_WRITE_ACCESS;
    IWDG->PR = IWDG_PRESCALER_DIV32;
    IWDG->RLR = timeout_ms - 1U;

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
