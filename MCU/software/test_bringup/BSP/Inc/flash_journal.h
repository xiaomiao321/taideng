/**
 * @file flash_journal.h
 * @brief 两块 Flash 区轮换的追加式记录存储：保存一份定长参数，掉电安全。
 *
 * 每块区划分为等长的槽，每次保存把一条新记录写入当前区的下一个空槽：
 *
 *   [序号 u32][长度 u32][参数 payload_size 字节][CRC32 u32][魔数 u32]
 *
 * 魔数最后写入，写到一半掉电的记录魔数不对，加载时自动跳过并退回上一条。
 * 加载时在两块区中找序号最大、校验正确的记录。当前区写满后换到另一块区，
 * 因此只有换区时才需要擦除，平时保存只是几十个字的编程。
 *
 * 本模块不依赖 HAL：Flash 按内存映射直接读取，擦除和编程通过回调完成，
 * 可在 PC 上用内存模拟 Flash 测试（HostTest/flash_journal_selftest.c）。
 *
 * @note 编程语义按 NOR Flash：擦除后全为 0xFF，编程只能把 1 变成 0。
 */

#ifndef AITAID_FLASH_JOURNAL_H
#define AITAID_FLASH_JOURNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_common.h"

/** @brief 存储区数量。 */
#define FLASH_JOURNAL_AREA_COUNT (2U)

/** @brief 每条记录除参数外的固定开销：序号、长度、CRC、魔数。 */
#define FLASH_JOURNAL_OVERHEAD (16U)

/** @brief 擦除和编程回调。 */
typedef struct
{
    /** @brief 擦除整块区；返回 false 表示失败。 */
    bool (*erase)(void *ctx, unsigned area);
    /**
     * @brief 从区内偏移 offset 起依次编程 count 个 32 位字。
     * @return false 表示失败；部分字可能已写入。
     */
    bool (*program)(void *ctx, unsigned area, uint32_t offset,
                    const uint32_t *words, uint32_t count);
    void *ctx; /**< 原样传给回调。 */
} flash_journal_ops_t;

/** @brief 存储实例。字段由本模块维护，调用者不得直接修改。 */
typedef struct
{
    flash_journal_ops_t ops;
    const uint8_t *areas[FLASH_JOURNAL_AREA_COUNT]; /**< 各区的可读地址。 */
    uint32_t area_size;                             /**< 每块区的字节数。 */
    uint32_t payload_size; /**< 参数字节数，4 的倍数。 */
    uint32_t slot_size;    /**< 每条记录占用的字节数。 */
    uint32_t slot_count;   /**< 每块区的槽数。 */
    bool has_record;       /**< 是否找到过有效记录。 */
    unsigned latest_area;  /**< 最新记录所在区。 */
    uint32_t latest_slot;  /**< 最新记录所在槽。 */
    uint32_t latest_seq;   /**< 最新记录的序号。 */
    unsigned write_area;   /**< 下一次保存写入的区。 */
    uint32_t next_slot; /**< 写入区下一个可写槽；等于 slot_count 表示已满。 */
} flash_journal_t;

/**
 * @brief 绑定存储区并扫描，找出最新的有效记录。
 *
 * @param[in] areas 两块区的可读地址，不允许为 NULL，4 字节对齐。
 * @param[in] area_size 每块区的字节数，4 的倍数，至少能放下一条记录。
 * @param[in] payload_size 参数字节数，4 的倍数且不为 0。
 *
 * @retval BSP_OK 已初始化（不论是否找到记录）。
 * @retval BSP_ERR_PARAM 参数无效。
 */
bsp_status_t flash_journal_init(flash_journal_t *j, flash_journal_ops_t ops,
                                const uint8_t *const areas[2],
                                uint32_t area_size, uint32_t payload_size);

/**
 * @brief 预先擦除备用区（不含最新记录的那一块），使之后的保存不必擦除。
 *
 * @retval BSP_OK 备用区本来就是空的，或已擦除。
 * @retval BSP_ERR_HAL 擦除失败。
 *
 * @note 擦除可能耗时数秒，应在初始化阶段、执行器停止时调用。
 */
bsp_status_t flash_journal_prepare(flash_journal_t *j);

/**
 * @brief 读出最新的参数。
 *
 * @param[out] payload 输出缓冲，至少 payload_size 字节。
 *
 * @retval BSP_OK 已读出。
 * @retval BSP_IDLE 没有任何有效记录，payload 不变。
 */
bsp_status_t flash_journal_load(const flash_journal_t *j, void *payload);

/**
 * @brief 保存一份新参数，成为最新记录。
 *
 * @param[in] payload 参数，payload_size 字节。
 *
 * @retval BSP_OK 已写入并校验通过。
 * @retval BSP_ERR_HAL 擦除或编程失败；之前的最新记录仍然有效。
 * @retval BSP_ERR_DATA 写入后回读校验不符；之前的最新记录仍然有效。
 *
 * @note 当前区写满时换到另一块区，若该区不是空的会先擦除（可能耗时数秒）。
 */
bsp_status_t flash_journal_save(flash_journal_t *j, const void *payload);

#endif /* AITAID_FLASH_JOURNAL_H */
