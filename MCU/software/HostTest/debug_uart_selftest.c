/**
 * @file debug_uart_selftest.c
 * @brief bsp_debug_uart 发送部分的 PC 端自测：入队、中断分段发送、丢弃和排空。
 *
 * 用 fake_hal/usart.h 里的假 HAL 代替真正的串口：HAL_UART_Transmit_IT() 只记下
 * 要发的一段，测试再调用发送完成回调，模拟"这一段已经发到线路上"。
 *
 * @note 本文件不属于固件，不参与 arm 构建；输出一律 ASCII。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "bsp_debug_uart.h"
#include "usart.h"

/* -------------------------------------------------------------------------- */
/* 假 HAL                                                                      */
/* -------------------------------------------------------------------------- */

USART_TypeDef fake_usart3;
UART_HandleTypeDef huart3;

static const uint8_t *pending_data; /**< 正在"发送"的一段，NULL 表示空闲。 */
static uint16_t pending_size;
static HAL_StatusTypeDef transmit_result = HAL_OK;
static unsigned init_calls;
static int irq_mask_depth;
static uint32_t tick;
static uint32_t primask;
static char wire[20000]; /**< 已经发到线路上的全部字节。 */
static size_t wire_length;
/** @brief 每次读时刻时自动"发完"当前这一段，模拟等待期间中断在工作。 */
static bool is_auto_complete;

static void complete_chunk(void)
{
    assert(pending_data != NULL);
    memcpy(&wire[wire_length], pending_data, pending_size);
    wire_length += pending_size;
    wire[wire_length] = '\0';
    pending_data = NULL;
    bsp_debug_uart_on_tx_cplt_from_isr();
}

HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *huart)
{
    (void)huart;
    ++init_calls;
    return HAL_OK;
}

HAL_UART_StateTypeDef HAL_UART_GetState(const UART_HandleTypeDef *huart)
{
    return huart->gState;
}

HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *huart,
                                       const uint8_t *data, uint16_t size)
{
    (void)huart;
    /* 主循环启动发送时必须已经屏蔽 USART3 中断；中断里启动时不需要。 */
    assert(pending_data == NULL && size > 0U);
    if (transmit_result != HAL_OK)
    {
        return transmit_result;
    }
    pending_data = data;
    pending_size = size;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *huart, uint8_t *data,
                                      uint16_t size)
{
    (void)huart;
    (void)data;
    (void)size;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *huart)
{
    (void)huart;
    return HAL_OK;
}

void HAL_NVIC_DisableIRQ(IRQn_Type irq)
{
    assert(irq == USART3_IRQn);
    ++irq_mask_depth;
}

void HAL_NVIC_EnableIRQ(IRQn_Type irq)
{
    assert(irq == USART3_IRQn && irq_mask_depth == 1);
    --irq_mask_depth;
}

uint32_t HAL_GetTick(void)
{
    if (is_auto_complete && pending_data != NULL && irq_mask_depth == 0)
    {
        complete_chunk();
    }
    return tick++;
}

uint32_t __get_PRIMASK(void)
{
    return primask;
}

uint32_t __get_IPSR(void)
{
    return 0U;
}

void __DMB(void)
{
}

/* -------------------------------------------------------------------------- */
/* 用例                                                                        */
/* -------------------------------------------------------------------------- */

static bsp_status_t reply(const char *text, uint32_t timeout_ms)
{
    return bsp_debug_uart_write((const uint8_t *)text, strlen(text),
                                timeout_ms);
}

static bsp_status_t status_line(const char *text, bool is_bulk)
{
    return bsp_debug_uart_write_droppable((const uint8_t *)text, strlen(text),
                                          is_bulk);
}

/** @brief 把队列里的全部数据"发完"。 */
static void complete_all(void)
{
    while (pending_data != NULL)
    {
        complete_chunk();
    }
}

static void reset(void)
{
    huart3.Instance = USART3;
    huart3.gState = HAL_UART_STATE_READY;
    huart3.RxState = HAL_UART_STATE_READY;
    huart3.Init.BaudRate = 115200U;
    pending_data = NULL;
    transmit_result = HAL_OK;
    init_calls = 0U;
    primask = 0U;
    wire_length = 0U;
    wire[0] = '\0';
    is_auto_complete = false;
    assert(bsp_debug_uart_tx_init() == BSP_OK);
}

int main(void)
{
    static char big[DEBUG_UART_TX_QUEUE_SIZE + 8U];
    memset(big, 'x', sizeof(big) - 1U);

    /* 初始化之前不能发；句柄没准备好时初始化失败。 */
    assert(reply("early\r\n", 0U) == BSP_ERR_NOT_INITIALIZED);
    assert(status_line("early\r\n", false) == BSP_ERR_NOT_INITIALIZED);
    assert(!bsp_debug_uart_drain(10U));
    huart3.Instance = USART3;
    huart3.gState = HAL_UART_STATE_RESET;
    assert(bsp_debug_uart_tx_init() == BSP_ERR_CONFIG);

    /* 初始化把波特率改成配置值，只在不同的时候重新初始化串口。 */
    reset();
    assert(huart3.Init.BaudRate == DEBUG_UART_BAUD && init_calls == 1U);
    assert(bsp_debug_uart_tx_init() == BSP_OK && init_calls == 1U);

    /* 参数检查。 */
    assert(reply("", 0U) == BSP_ERR_PARAM);
    assert(bsp_debug_uart_write(NULL, 3U, 0U) == BSP_ERR_PARAM);
    assert(bsp_debug_uart_write((const uint8_t *)big,
                                DEBUG_UART_TX_QUEUE_SIZE, 0U) == BSP_ERR_PARAM);
    assert(bsp_debug_uart_write_droppable(NULL, 3U, false) == BSP_ERR_PARAM);

    /* 入队立即返回并启动发送；发送期间入队的行排在后面，一行都不乱。 */
    assert(reply("ACK 1 PING\r\n", 0U) == BSP_OK);
    assert(pending_data != NULL && pending_size == 12U && wire_length == 0U);
    assert(status_line("STATE 100 0 0 0 0\r\n", false) == BSP_OK);
    assert(reply("ACK 2 STOP\r\n", 0U) == BSP_OK);
    assert(pending_size == 12U); /* 正在发的那一段不受后来入队的影响 */
    complete_chunk();
    assert(!strcmp(wire, "ACK 1 PING\r\n"));
    assert(pending_data != NULL && pending_size == 31U); /* 剩下两行合成一段 */
    complete_chunk();
    assert(pending_data == NULL);
    assert(!strcmp(wire, "ACK 1 PING\r\nSTATE 100 0 0 0 0\r\nACK 2 STOP\r\n"));
    assert(bsp_debug_uart_drain(0U) && irq_mask_depth == 0);

    /* 没有新数据时多出来的完成回调不做任何事。 */
    bsp_debug_uart_on_tx_cplt_from_isr();
    assert(pending_data == NULL && bsp_debug_uart_tx_drop_count() == 0U);

    /* 队列紧张时的先后：数据记录最先被拒，然后是周期状态，回复用到最后。 */
    reset();
    char line[101];
    memset(line, 'j', 98U);
    memcpy(&line[98], "\r\n", 3U);
    unsigned bulk_lines = 0U;
    while (status_line(line, true) == BSP_OK)
    {
        ++bulk_lines;
    }
    /* 数据记录最多用到"容量减去两段预留"。 */
    assert(bulk_lines == (DEBUG_UART_TX_QUEUE_SIZE - 1U -
                          DEBUG_UART_TX_REPLY_RESERVE -
                          DEBUG_UART_TX_STATUS_RESERVE) / 100U);
    assert(bsp_debug_uart_tx_drop_count() == 1U);
    unsigned status_lines = 0U;
    while (status_line(line, false) == BSP_OK)
    {
        ++status_lines;
    }
    assert(status_lines >= DEBUG_UART_TX_STATUS_RESERVE / 100U &&
           status_lines <= DEBUG_UART_TX_STATUS_RESERVE / 100U + 1U);
    assert(bsp_debug_uart_tx_drop_count() == 2U);
    unsigned reply_lines = 0U;
    while (reply(line, 0U) == BSP_OK)
    {
        ++reply_lines;
    }
    assert(reply_lines >= DEBUG_UART_TX_REPLY_RESERVE / 100U);
    assert(bsp_debug_uart_tx_drop_count() == 3U);
    assert(status_line(line, true) == BSP_BUSY &&
           status_line(line, false) == BSP_BUSY);
    assert(bsp_debug_uart_tx_drop_count() == 5U);
    /* 丢的都是整行：线路上的字节数是行长的整数倍，内容没有错位。 */
    complete_all();
    assert(wire_length == 100U * (bulk_lines + status_lines + reply_lines));
    for (size_t i = 0U; i < wire_length; i += 100U)
    {
        assert(!memcmp(&wire[i], line, 100U));
    }

    /* 回复放不下时在限定时间内等中断腾出位置；等待期间中断照常发送。 */
    reset();
    while (reply(line, 0U) == BSP_OK)
    {
    }
    uint32_t drops = bsp_debug_uart_tx_drop_count();
    is_auto_complete = true;
    assert(reply("ACK 9 JMOVE\r\n", 50U) == BSP_OK);
    assert(bsp_debug_uart_tx_drop_count() == drops);
    is_auto_complete = false;
    complete_all();
    assert(!strcmp(&wire[wire_length - 13U], "ACK 9 JMOVE\r\n"));

    /* 等不到（上位机没接、发送停住）就超时并计数；关中断时不等。 */
    reset();
    while (reply(line, 0U) == BSP_OK)
    {
    }
    drops = bsp_debug_uart_tx_drop_count();
    uint32_t before = tick;
    assert(reply(line, 30U) == BSP_ERR_TIMEOUT);
    assert(tick - before >= 30U && tick - before <= 40U);
    assert(bsp_debug_uart_tx_drop_count() == drops + 1U);
    primask = 1U;
    before = tick;
    assert(reply(line, 1000U) == BSP_ERR_TIMEOUT && tick - before <= 2U);
    assert(!bsp_debug_uart_drain(1000U));
    primask = 0U;
    assert(!bsp_debug_uart_drain(20U)); /* 没有人"发完"，排空超时 */
    is_auto_complete = true;
    assert(bsp_debug_uart_drain(1000U) && pending_data == NULL);
    is_auto_complete = false;

    /* 启动发送失败（HAL 忙）：数据留在队列里，下一次入队时重新发出。 */
    reset();
    transmit_result = HAL_BUSY;
    assert(reply("first\r\n", 0U) == BSP_OK && pending_data == NULL);
    transmit_result = HAL_OK;
    assert(reply("second\r\n", 0U) == BSP_OK && pending_data != NULL);
    complete_all();
    assert(!strcmp(wire, "first\r\nsecond\r\n"));

    /* 中断里启动下一段失败：同样留在队列里，由 update 重新发出。 */
    reset();
    assert(bsp_debug_uart_init() == BSP_OK);
    assert(reply("one\r\n", 0U) == BSP_OK);
    assert(reply("two\r\n", 0U) == BSP_OK);
    assert(pending_size == 5U);
    transmit_result = HAL_BUSY;
    complete_chunk();
    assert(pending_data == NULL && !strcmp(wire, "one\r\n"));
    transmit_result = HAL_OK;
    assert(bsp_debug_uart_update() == BSP_OK && pending_data != NULL);
    complete_all();
    assert(!strcmp(wire, "one\r\ntwo\r\n"));

    /* 长时间交替收发：跨过队列末尾许多次，线路上的内容与入队的完全一致。 */
    reset();
    static char expected[20000];
    size_t expected_length = 0U;
    for (unsigned i = 0U; i < 300U; ++i)
    {
        char text[64];
        int n = snprintf(text, sizeof(text), "JSTATE %u M%u 1 1 %u\r\n", i,
                         i % 6U + 1U, i * 7U);
        if (status_line(text, (i % 3U) == 0U) == BSP_OK)
        {
            memcpy(&expected[expected_length], text, (size_t)n);
            expected_length += (size_t)n;
        }
        if ((i % 2U) == 1U && pending_data != NULL)
        {
            complete_chunk();
        }
    }
    complete_all();
    assert(wire_length == expected_length && expected_length > 5000U);
    assert(!memcmp(wire, expected, expected_length));
    assert(irq_mask_depth == 0);

    puts("debug_uart tx: baud setup, queued order, chunking, drop priority, "
         "bounded wait, drain, restart after HAL busy and wrap-around passed");
    return 0;
}
