#include "bsp_param_flash.h"

#include <stdbool.h>
#include <stddef.h>

#include "bsp_watchdog.h"
#include "flash_journal.h"
#include "main.h"

/** @brief 参数扇区：Flash 最后两个 128KB 扇区。 */
#define PARAM_AREA_SIZE (128U * 1024U)
#define PARAM_AREA0_ADDRESS (0x080C0000U)
#define PARAM_AREA1_ADDRESS (0x080E0000U)

/** @brief 擦除期间放宽到的看门狗超时；128KB 扇区擦除最长约 2 秒。 */
#define ERASE_WATCHDOG_TIMEOUT_MS (8000U)

/* 链接脚本提供：.data 的加载地址和长度，用来算出固件映像在 Flash 中的结尾。 */
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;

static const uint32_t k_sectors[FLASH_JOURNAL_AREA_COUNT] = {FLASH_SECTOR_10,
                                                             FLASH_SECTOR_11};

static flash_journal_t s_journal;
static bool s_is_initialized = false;

static void clear_flash_errors(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

/** @brief 编程后清空数据缓存，避免回读到编程前缓存的旧内容。 */
static void flush_data_cache(void)
{
    if (READ_BIT(FLASH->ACR, FLASH_ACR_DCEN) != 0U)
    {
        __HAL_FLASH_DATA_CACHE_DISABLE();
        __HAL_FLASH_DATA_CACHE_RESET();
        __HAL_FLASH_DATA_CACHE_ENABLE();
    }
}

static bool erase_area(void *ctx, unsigned area)
{
    (void)ctx;
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_SECTORS,
        .Sector = k_sectors[area],
        .NbSectors = 1U,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
    };
    uint32_t bad_sector = 0U;

    /* 擦除期间 CPU 停顿、无法喂狗：临时放宽看门狗，结束后恢复原值。 */
    uint32_t saved_timeout = bsp_watchdog_timeout();
    if (saved_timeout != 0U)
    {
        (void)bsp_watchdog_set_timeout(ERASE_WATCHDOG_TIMEOUT_MS);
    }

    bool ok = (HAL_FLASH_Unlock() == HAL_OK);
    if (ok)
    {
        clear_flash_errors();
        ok = (HAL_FLASHEx_Erase(&erase, &bad_sector) == HAL_OK);
        (void)HAL_FLASH_Lock();
    }

    if (saved_timeout != 0U)
    {
        (void)bsp_watchdog_set_timeout(saved_timeout);
    }
    return ok;
}

static bool program_area(void *ctx, unsigned area, uint32_t offset,
                         const uint32_t *words, uint32_t count)
{
    const uint32_t base =
        (area == 0U) ? PARAM_AREA0_ADDRESS : PARAM_AREA1_ADDRESS;
    (void)ctx;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return false;
    }
    clear_flash_errors();
    bool ok = true;
    for (uint32_t i = 0; ok && (i < count); ++i)
    {
        ok = (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, base + offset + 4U * i,
                                words[i]) == HAL_OK);
    }
    (void)HAL_FLASH_Lock();
    flush_data_cache();
    return ok;
}

bsp_status_t bsp_param_flash_init(uint32_t payload_size)
{
    s_is_initialized = false;

    /* 固件映像结尾 = .data 初值的加载地址 + .data 长度。万一程序长到参数
     * 扇区（链接脚本被改回 1024K 时），擦除会毁掉程序本身，必须拒绝。 */
    uint32_t image_end =
        (uint32_t)&_sidata + ((uint32_t)&_edata - (uint32_t)&_sdata);
    if (image_end > PARAM_AREA0_ADDRESS)
    {
        return BSP_ERR_CONFIG;
    }

    const uint8_t *areas[FLASH_JOURNAL_AREA_COUNT] = {
        (const uint8_t *)PARAM_AREA0_ADDRESS,
        (const uint8_t *)PARAM_AREA1_ADDRESS};
    flash_journal_ops_t ops = {erase_area, program_area, NULL};
    bsp_status_t status = flash_journal_init(&s_journal, ops, areas,
                                             PARAM_AREA_SIZE, payload_size);
    if (status != BSP_OK)
    {
        return status;
    }
    s_is_initialized = true;
    return flash_journal_prepare(&s_journal);
}

bsp_status_t bsp_param_flash_load(void *payload)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    return flash_journal_load(&s_journal, payload);
}

bsp_status_t bsp_param_flash_save(const void *payload)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    return flash_journal_save(&s_journal, payload);
}
