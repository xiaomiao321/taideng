/**
 * @file tx_queue.h
 * @brief 发送用的字节环形队列：主循环整行写入，发送中断按连续的一段取走。
 *
 * 一个写入者（主循环）加一个读取者（发送完成中断）时不需要关中断：写入者只改
 * head，读取者只改 tail。写入以整行为单位，放不下就一个字节都不写，所以线路上
 * 不会出现半行。
 *
 * 写入时可以要求"写完后至少还剩 reserve 字节"。周期性的状态行带着这个要求
 * 写入，命令的回复不带，于是队列快满时先丢状态行，最后一段空间留给回复。
 *
 * @note 不依赖 HAL，可在 PC 上单测（HostTest/tx_queue_selftest.c）。
 */

#ifndef AITAID_TX_QUEUE_H
#define AITAID_TX_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

/** @brief 队列实例。字段由本模块维护，调用者不得直接修改。 */
typedef struct
{
    uint8_t *buffer;        /**< 存储区。 */
    uint32_t mask;          /**< 容量减 1；容量是 2 的幂。 */
    volatile uint32_t head; /**< 下一个写入位置，只由写入者修改。 */
    volatile uint32_t tail; /**< 下一个取出位置，只由读取者修改。 */
} tx_queue_t;

/**
 * @brief 绑定存储区并清空。
 *
 * @param[in] buffer 存储区，不允许为 NULL。
 * @param[in] size 字节数，必须是 2 的幂且不小于 2；最多能存 size - 1 字节。
 *
 * @retval false 参数无效，队列不可用。
 */
bool tx_queue_init(tx_queue_t *queue, uint8_t *buffer, uint32_t size);

/** @brief 还能写入的字节数。 */
uint32_t tx_queue_free(const tx_queue_t *queue);

/** @brief 队列是否为空。 */
bool tx_queue_is_empty(const tx_queue_t *queue);

/**
 * @brief 整段写入，放不下就不写。
 *
 * @param[in] data 待写入数据，不允许为 NULL。
 * @param[in] length 字节数，0 视为成功。
 * @param[in] reserve 写完后至少要剩下的字节数；回复类数据传 0。
 *
 * @retval true 已全部写入。
 * @retval false 空间不够，队列不变。
 *
 * @warning 只能由写入者调用。
 */
bool tx_queue_put(tx_queue_t *queue, const uint8_t *data, uint32_t length,
                  uint32_t reserve);

/**
 * @brief 取队首连续的一段，不移除。
 *
 * @param[out] data 指向这一段的首字节；返回 0 时不变。
 *
 * @return 这一段的字节数；数据跨过存储区末尾时只返回到末尾为止的部分，
 *         取走后再调用一次得到剩下的。队列为空时返回 0。
 *
 * @warning 只能由读取者调用。
 */
uint32_t tx_queue_peek(const tx_queue_t *queue, const uint8_t **data);

/**
 * @brief 移除队首的 length 字节，length 不得超过上一次 peek 的返回值。
 *
 * @warning 只能由读取者调用。
 */
void tx_queue_pop(tx_queue_t *queue, uint32_t length);

#endif /* AITAID_TX_QUEUE_H */
