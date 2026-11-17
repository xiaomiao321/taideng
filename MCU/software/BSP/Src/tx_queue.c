/**
 * @file tx_queue.c
 * @brief 发送用的字节环形队列实现。
 */

#include "tx_queue.h"

#include <stddef.h>
#include <string.h>

bool tx_queue_init(tx_queue_t *queue, uint8_t *buffer, uint32_t size)
{
    if ((queue == NULL) || (buffer == NULL) || (size < 2U) ||
        ((size & (size - 1U)) != 0U))
    {
        return false;
    }
    queue->buffer = buffer;
    queue->mask = size - 1U;
    queue->head = 0U;
    queue->tail = 0U;
    return true;
}

uint32_t tx_queue_free(const tx_queue_t *queue)
{
    /* 留一个字节不用，head 等于 tail 只表示空。 */
    return queue->mask - ((queue->head - queue->tail) & queue->mask);
}

bool tx_queue_is_empty(const tx_queue_t *queue)
{
    return queue->head == queue->tail;
}

bool tx_queue_put(tx_queue_t *queue, const uint8_t *data, uint32_t length,
                  uint32_t reserve)
{
    uint32_t space = tx_queue_free(queue);
    if ((length > space) || (space - length < reserve))
    {
        return false;
    }
    uint32_t head = queue->head;
    uint32_t first = queue->mask + 1U - head; /* 到存储区末尾的字节数 */
    if (first > length)
    {
        first = length;
    }
    memcpy(&queue->buffer[head], data, first);
    memcpy(queue->buffer, &data[first], length - first);
    /* 数据写完之后才发布新位置，读取者不会取到还没写好的字节。 */
    queue->head = (head + length) & queue->mask;
    return true;
}

uint32_t tx_queue_peek(const tx_queue_t *queue, const uint8_t **data)
{
    uint32_t head = queue->head;
    uint32_t tail = queue->tail;
    if (head == tail)
    {
        return 0U;
    }
    *data = &queue->buffer[tail];
    return (head > tail) ? (head - tail) : (queue->mask + 1U - tail);
}

void tx_queue_pop(tx_queue_t *queue, uint32_t length)
{
    queue->tail = (queue->tail + length) & queue->mask;
}
