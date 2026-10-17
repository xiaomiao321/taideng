#include "flash_journal.h"

#include <stddef.h>
#include <string.h>

/** @brief 有效记录最后一个字的魔数（"JRNL"）。 */
#define JOURNAL_MAGIC (0x4C4E524AU)

/** @brief 擦除后的字。 */
#define ERASED_WORD (0xFFFFFFFFU)

/** @brief 一条记录的最大字数：固定开销加参数，参数上限 1KB。 */
#define MAX_PAYLOAD_SIZE (1024U)
#define MAX_RECORD_WORDS ((FLASH_JOURNAL_OVERHEAD + MAX_PAYLOAD_SIZE) / 4U)

/** @brief CRC-32（IEEE 802.3，反射多项式 0xEDB88320），逐位计算。 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8U; ++bit)
        {
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
        }
    }
    return crc;
}

static uint32_t read_word(const uint8_t *address)
{
    uint32_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}

static const uint8_t *slot_address(const flash_journal_t *j, unsigned area,
                                   uint32_t slot)
{
    return j->areas[area] + slot * j->slot_size;
}

/** @brief 记录的 CRC 覆盖序号、长度和参数。 */
static uint32_t record_crc(uint32_t seq, uint32_t size, const uint8_t *payload)
{
    uint32_t crc = 0xFFFFFFFFU;
    crc = crc32_update(crc, (const uint8_t *)&seq, sizeof(seq));
    crc = crc32_update(crc, (const uint8_t *)&size, sizeof(size));
    crc = crc32_update(crc, payload, size);
    return crc ^ 0xFFFFFFFFU;
}

static bool is_range_erased(const uint8_t *address, uint32_t size)
{
    for (uint32_t i = 0; i < size; i += 4U)
    {
        if (read_word(&address[i]) != ERASED_WORD)
        {
            return false;
        }
    }
    return true;
}

/** @brief 槽内是否是一条完整有效的记录，有效时输出序号。 */
static bool is_slot_valid(const flash_journal_t *j, unsigned area,
                          uint32_t slot, uint32_t *seq)
{
    const uint8_t *p = slot_address(j, area, slot);
    uint32_t size = read_word(&p[4]);
    if ((read_word(&p[j->slot_size - 4U]) != JOURNAL_MAGIC) ||
        (size != j->payload_size))
    {
        return false;
    }
    uint32_t crc = read_word(&p[8U + size]);
    if (crc != record_crc(read_word(&p[0]), size, &p[8]))
    {
        return false;
    }
    *seq = read_word(&p[0]);
    return true;
}

/**
 * @brief 扫描一块区：找出其中最新的有效记录和下一个可写槽。
 *
 * @return 该区是否含有效记录。
 */
static bool scan_area(const flash_journal_t *j, unsigned area,
                      uint32_t *latest_slot, uint32_t *latest_seq,
                      uint32_t *next_slot)
{
    bool found = false;
    *next_slot = 0U;
    for (uint32_t slot = 0; slot < j->slot_count; ++slot)
    {
        if (is_range_erased(slot_address(j, area, slot), j->slot_size))
        {
            continue;
        }
        /* 已用槽（有效记录，或写到一半的残缺记录）之后才能追加。 */
        *next_slot = slot + 1U;
        uint32_t seq;
        if (is_slot_valid(j, area, slot, &seq) &&
            (!found || (int32_t)(seq - *latest_seq) > 0))
        {
            found = true;
            *latest_slot = slot;
            *latest_seq = seq;
        }
    }
    return found;
}

bsp_status_t flash_journal_init(flash_journal_t *j, flash_journal_ops_t ops,
                                const uint8_t *const areas[2],
                                uint32_t area_size, uint32_t payload_size)
{
    if ((j == NULL) || (areas == NULL) || (areas[0] == NULL) ||
        (areas[1] == NULL) || (ops.erase == NULL) || (ops.program == NULL) ||
        (payload_size == 0U) || (payload_size > MAX_PAYLOAD_SIZE) ||
        ((payload_size % 4U) != 0U) || ((area_size % 4U) != 0U) ||
        (area_size < payload_size + FLASH_JOURNAL_OVERHEAD))
    {
        return BSP_ERR_PARAM;
    }

    j->ops = ops;
    j->areas[0] = areas[0];
    j->areas[1] = areas[1];
    j->area_size = area_size;
    j->payload_size = payload_size;
    j->slot_size = payload_size + FLASH_JOURNAL_OVERHEAD;
    j->slot_count = area_size / j->slot_size;
    j->has_record = false;
    j->latest_area = 0U;
    j->latest_slot = 0U;
    j->latest_seq = 0U;

    uint32_t next[FLASH_JOURNAL_AREA_COUNT];
    for (unsigned area = 0; area < FLASH_JOURNAL_AREA_COUNT; ++area)
    {
        uint32_t slot;
        uint32_t seq;
        if (scan_area(j, area, &slot, &seq, &next[area]) &&
            (!j->has_record || (int32_t)(seq - j->latest_seq) > 0))
        {
            j->has_record = true;
            j->latest_area = area;
            j->latest_slot = slot;
            j->latest_seq = seq;
        }
    }
    j->write_area = j->latest_area;
    j->next_slot = next[j->write_area];
    return BSP_OK;
}

bsp_status_t flash_journal_prepare(flash_journal_t *j)
{
    unsigned spare = 1U - j->latest_area;
    if (is_range_erased(j->areas[spare], j->area_size))
    {
        return BSP_OK;
    }
    return j->ops.erase(j->ops.ctx, spare) ? BSP_OK : BSP_ERR_HAL;
}

bsp_status_t flash_journal_load(const flash_journal_t *j, void *payload)
{
    if (!j->has_record)
    {
        return BSP_IDLE;
    }
    memcpy(payload, slot_address(j, j->latest_area, j->latest_slot) + 8U,
           j->payload_size);
    return BSP_OK;
}

bsp_status_t flash_journal_save(flash_journal_t *j, const void *payload)
{
    unsigned area = j->write_area;
    uint32_t slot = j->next_slot;
    if (slot >= j->slot_count)
    {
        /* 写入区已满：换到另一块区，那里若有旧数据先擦除。 */
        area = 1U - j->write_area;
        slot = 0U;
        if (!is_range_erased(j->areas[area], j->area_size) &&
            !j->ops.erase(j->ops.ctx, area))
        {
            return BSP_ERR_HAL;
        }
    }

    uint32_t seq = j->has_record ? j->latest_seq + 1U : 1U;
    uint32_t words[MAX_RECORD_WORDS];
    uint32_t count = j->slot_size / 4U;
    words[0] = seq;
    words[1] = j->payload_size;
    memcpy(&words[2], payload, j->payload_size);
    words[count - 2U] = record_crc(seq, j->payload_size, payload);
    words[count - 1U] = JOURNAL_MAGIC; /* 最后写魔数，写一半掉电即视为无效 */

    /* 无论成败，这个槽都已被占用；下次从后一个槽开始写。
     * 最新记录的位置只在校验通过后才更新，失败时上一条仍可加载。 */
    j->write_area = area;
    j->next_slot = slot + 1U;
    bool is_programmed =
        j->ops.program(j->ops.ctx, area, slot * j->slot_size, words, count);

    uint32_t written_seq;
    if (!is_programmed)
    {
        return BSP_ERR_HAL;
    }
    if (!is_slot_valid(j, area, slot, &written_seq) || (written_seq != seq) ||
        (memcmp(slot_address(j, area, slot) + 8U, payload, j->payload_size) !=
         0))
    {
        return BSP_ERR_DATA;
    }

    j->has_record = true;
    j->latest_area = area;
    j->latest_slot = slot;
    j->latest_seq = seq;
    return BSP_OK;
}
