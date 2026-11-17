/**
 * @file tx_queue_selftest.c
 * @brief tx_queue 的 PC 端自测：整行写入、预留空间、跨末尾回绕和取出顺序。
 *
 * @note 本文件不属于固件，不参与 arm 构建；输出一律 ASCII。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tx_queue.h"

static uint8_t storage[16];
static tx_queue_t queue;

/** @brief 像发送中断那样取空队列，把内容接到 out 后面；返回取走的字节数。 */
static uint32_t drain(char *out)
{
    uint32_t total = 0U;
    const uint8_t *data = NULL;
    uint32_t length;
    while ((length = tx_queue_peek(&queue, &data)) != 0U)
    {
        memcpy(&out[total], data, length);
        total += length;
        tx_queue_pop(&queue, length);
    }
    out[total] = '\0';
    return total;
}

int main(void)
{
    char out[64];
    const uint8_t *data = NULL;

    /* 容量必须是 2 的幂；空队列取不出东西。 */
    assert(!tx_queue_init(&queue, storage, 12U));
    assert(!tx_queue_init(&queue, storage, 1U));
    assert(!tx_queue_init(&queue, NULL, 16U));
    assert(tx_queue_init(&queue, storage, sizeof(storage)));
    assert(tx_queue_is_empty(&queue) && tx_queue_free(&queue) == 15U);
    assert(tx_queue_peek(&queue, &data) == 0U && data == NULL);

    /* 按写入顺序取出。 */
    assert(tx_queue_put(&queue, (const uint8_t *)"abc", 3U, 0U));
    assert(tx_queue_put(&queue, (const uint8_t *)"defg", 4U, 0U));
    assert(tx_queue_free(&queue) == 8U && !tx_queue_is_empty(&queue));
    assert(drain(out) == 7U && !strcmp(out, "abcdefg"));
    assert(tx_queue_is_empty(&queue) && tx_queue_free(&queue) == 15U);

    /* 跨过存储区末尾：peek 分两段给出，内容连续。 */
    assert(tx_queue_put(&queue, (const uint8_t *)"0123456789AB", 12U, 0U));
    assert(tx_queue_peek(&queue, &data) == 9U && data == &storage[7]);
    tx_queue_pop(&queue, 9U);
    assert(tx_queue_peek(&queue, &data) == 3U && data == &storage[0]);
    assert(drain(out) == 3U && !strcmp(out, "9AB"));

    /* 整行写入：放不下就一个字节都不写。 */
    assert(tx_queue_put(&queue, (const uint8_t *)"0123456789", 10U, 0U));
    assert(!tx_queue_put(&queue, (const uint8_t *)"abcdef", 6U, 0U));
    assert(tx_queue_free(&queue) == 5U);
    assert(tx_queue_put(&queue, (const uint8_t *)"abcde", 5U, 0U));
    assert(tx_queue_free(&queue) == 0U);
    assert(!tx_queue_put(&queue, (const uint8_t *)"x", 1U, 0U));
    assert(drain(out) == 15U && !strcmp(out, "0123456789abcde"));

    /* 预留空间：状态行要给回复留出 reserve 字节，回复自己可以用到最后。 */
    assert(tx_queue_put(&queue, (const uint8_t *)"status1", 7U, 4U));
    assert(!tx_queue_put(&queue, (const uint8_t *)"status2", 7U, 4U));
    assert(tx_queue_put(&queue, (const uint8_t *)"s3", 2U, 6U));
    assert(!tx_queue_put(&queue, (const uint8_t *)"s", 1U, 6U));
    assert(tx_queue_put(&queue, (const uint8_t *)"reply!", 6U, 0U));
    assert(tx_queue_free(&queue) == 0U);
    assert(drain(out) == 15U && !strcmp(out, "status1s3reply!"));

    /* 取走一部分后再写：读写交替许多圈，内容和顺序都不乱。 */
    unsigned written = 0U;
    unsigned read = 0U;
    for (unsigned round = 0U; round < 1000U; ++round)
    {
        uint8_t line[5];
        for (unsigned i = 0U; i < sizeof(line); ++i)
        {
            line[i] = (uint8_t)((written + i) & 0xFFU);
        }
        if (tx_queue_put(&queue, line, sizeof(line), 0U))
        {
            written += sizeof(line);
        }
        uint32_t length = tx_queue_peek(&queue, &data);
        if (length > 3U)
        {
            length = 3U; /* 发送中断一次只取走一小段 */
        }
        for (uint32_t i = 0U; i < length; ++i)
        {
            assert(data[i] == (uint8_t)((read + i) & 0xFFU));
        }
        tx_queue_pop(&queue, length);
        read += length;
    }
    assert(written > 2500U && written - read <= 15U);

    /* 长度为 0 的写入总是成功，不改变队列。 */
    assert(tx_queue_put(&queue, (const uint8_t *)"", 0U, 0U));

    puts("tx_queue: order, wrap-around, all-or-nothing lines, reserve for "
         "replies and interleaved read/write passed");
    return 0;
}
