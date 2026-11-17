#include "bsp_debug_uart.h"

#include "app_config.h"
#include "bsp_uart_stream.h"
#include "tx_queue.h"
#include "usart.h"

_Static_assert(DEBUG_UART_TX_REPLY_RESERVE + DEBUG_UART_TX_STATUS_RESERVE <
                   DEBUG_UART_TX_QUEUE_SIZE,
               "reserves must leave room for trace data");

/** @brief 接收环形缓冲容量，必须是 2 的幂。 */
#define BSP_DEBUG_UART_RX_RING_SIZE (256U)

static uint8_t s_rx_ring[BSP_DEBUG_UART_RX_RING_SIZE];
static bsp_uart_stream_t s_rx_stream;
static bool s_is_initialized = false;

static uint8_t s_tx_storage[DEBUG_UART_TX_QUEUE_SIZE];
static tx_queue_t s_tx_queue;
/** @brief 发送队列是否可用。 */
static bool s_is_tx_ready = false;
/** @brief 是否有一段数据正在由中断发送；只在 USART3 中断被屏蔽时或中断里改。 */
static volatile bool s_is_tx_active = false;
/** @brief 正在发送的这一段的字节数，发完后从队列里移除。 */
static uint16_t s_tx_chunk = 0U;
/** @brief 因队列放不下而丢弃的数据段累计数。 */
static uint32_t s_tx_drop_count = 0U;
/** @brief 发送卡住后自恢复的次数，仅供调试器查看。 */
static uint32_t s_tx_stall_count = 0U;

/**
 * @brief 队列不空时启动下一段的中断发送。
 *
 * @note 调用时 USART3 中断必须不会打断本函数：要么在该中断里，要么已屏蔽它。
 * @note 一段是队列里连续的一块，发送期间主循环只会往它后面追加，不会改动它。
 */
static void start_next_chunk(void)
{
    const uint8_t *data = NULL;
    uint32_t length = tx_queue_peek(&s_tx_queue, &data);
    if (length == 0U)
    {
        s_is_tx_active = false;
        return;
    }
    if (length > UINT16_MAX)
    {
        length = UINT16_MAX;
    }
    s_tx_chunk = (uint16_t)length;
    /* 启动失败（HAL 忙等）时数据留在队列里，下一次入队或 update 再试。 */
    s_is_tx_active =
        HAL_UART_Transmit_IT(&huart3, data, s_tx_chunk) == HAL_OK;
}

/** @brief 发送空闲而队列不空时启动发送；主循环上下文调用。 */
static void kick_transmit(void)
{
    /* 只屏蔽 USART3 自己的中断：发送完成回调和 HAL 的收发状态都在它里面改，
     * 屏蔽期间它们不会和这里交错。屏蔽时间只有启动一次发送那么长。 */
    HAL_NVIC_DisableIRQ(USART3_IRQn);
    if (!s_is_tx_active)
    {
        start_next_chunk();
    }
    HAL_NVIC_EnableIRQ(USART3_IRQn);
}

/** @brief 当前上下文里发送中断能否运行；不能时等待队列腾空没有意义。 */
static bool can_wait(void)
{
    return (__get_PRIMASK() == 0U) && (__get_IPSR() == 0U);
}

bsp_status_t bsp_debug_uart_tx_init(void)
{
    s_is_tx_ready = false;
    if ((huart3.Instance != USART3) ||
        (HAL_UART_GetState(&huart3) == HAL_UART_STATE_RESET))
    {
        return BSP_ERR_CONFIG;
    }
    if (huart3.Init.BaudRate != DEBUG_UART_BAUD)
    {
        /* CubeMX 生成的初始化固定为 115200，在这里按配置重新设一次，
         * 不改生成的文件。 */
        huart3.Init.BaudRate = DEBUG_UART_BAUD;
        if (HAL_UART_Init(&huart3) != HAL_OK)
        {
            return BSP_ERR_HAL;
        }
    }
    (void)tx_queue_init(&s_tx_queue, s_tx_storage, sizeof(s_tx_storage));
    s_is_tx_active = false;
    s_tx_chunk = 0U;
    s_tx_drop_count = 0U;
    s_is_tx_ready = true;
    return BSP_OK;
}

bsp_status_t bsp_debug_uart_init(void)
{
    s_is_initialized = false;
    if (huart3.Instance != USART3)
    {
        return BSP_ERR_CONFIG;
    }

    bsp_status_t status = bsp_uart_stream_init(&s_rx_stream, &huart3, s_rx_ring,
                                               sizeof(s_rx_ring));
    if (status != BSP_OK)
    {
        return status;
    }
    /* 先置位再启动：否则第一个字节的回调被忽略，接收不会重新挂起。 */
    s_is_initialized = true;
    status = bsp_uart_stream_start(&s_rx_stream);
    if (status != BSP_OK)
    {
        s_is_initialized = false;
    }
    return status;
}

bsp_status_t bsp_debug_uart_write(const uint8_t *data, size_t length,
                                  uint32_t timeout_ms)
{
    if (!s_is_tx_ready)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if ((data == NULL) || (length == 0U) ||
        (length >= DEBUG_UART_TX_QUEUE_SIZE))
    {
        return BSP_ERR_PARAM;
    }

    uint32_t start_ms = HAL_GetTick();
    while (!tx_queue_put(&s_tx_queue, data, (uint32_t)length, 0U))
    {
        /* 放不下：让中断继续发，等它腾出位置。正常情况下走不到这里，
         * 队列里有留给回复的空间。 */
        kick_transmit();
        if (!can_wait() || ((uint32_t)(HAL_GetTick() - start_ms) >= timeout_ms))
        {
            ++s_tx_drop_count;
            return BSP_ERR_TIMEOUT;
        }
    }
    kick_transmit();
    return BSP_OK;
}

bsp_status_t bsp_debug_uart_write_droppable(const uint8_t *data, size_t length,
                                            bool is_bulk)
{
    if (!s_is_tx_ready)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if ((data == NULL) || (length == 0U) ||
        (length >= DEBUG_UART_TX_QUEUE_SIZE))
    {
        return BSP_ERR_PARAM;
    }
    uint32_t reserve = DEBUG_UART_TX_REPLY_RESERVE +
                       (is_bulk ? DEBUG_UART_TX_STATUS_RESERVE : 0U);
    if (!tx_queue_put(&s_tx_queue, data, (uint32_t)length, reserve))
    {
        ++s_tx_drop_count;
        kick_transmit();
        return BSP_BUSY;
    }
    kick_transmit();
    return BSP_OK;
}

bool bsp_debug_uart_drain(uint32_t timeout_ms)
{
    if (!s_is_tx_ready)
    {
        return false;
    }
    uint32_t start_ms = HAL_GetTick();
    for (;;)
    {
        kick_transmit();
        if (tx_queue_is_empty(&s_tx_queue) && !s_is_tx_active)
        {
            return true;
        }
        if (!can_wait() || ((uint32_t)(HAL_GetTick() - start_ms) >= timeout_ms))
        {
            return false;
        }
    }
}

uint32_t bsp_debug_uart_tx_drop_count(void)
{
    return s_tx_drop_count;
}

bool bsp_debug_uart_read(uint8_t *byte)
{
    if (!s_is_initialized || (byte == NULL))
    {
        return false;
    }
    return bsp_uart_stream_read(&s_rx_stream, byte);
}

void bsp_debug_uart_flush(void)
{
    if (s_is_initialized)
    {
        bsp_uart_stream_flush(&s_rx_stream);
    }
}

bsp_status_t bsp_debug_uart_update(void)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    /* 上一次启动发送失败时，队列里的数据在这里重新发出。 */
    if (s_is_tx_ready && !s_is_tx_active && !tx_queue_is_empty(&s_tx_queue))
    {
        kick_transmit();
    }
    /* 自恢复：标记着"正在发送"，HAL 却已经空闲，说明丢了一次发送完成回调。
     * 不处理的话队列再也发不出去，板子就此不出声。屏蔽中断后再确认一次，
     * 这时回调不可能正执行到一半；把这一段重新发一遍，宁可重复也不卡死。 */
    if (s_is_tx_ready && s_is_tx_active &&
        (huart3.gState == HAL_UART_STATE_READY))
    {
        HAL_NVIC_DisableIRQ(USART3_IRQn);
        if (s_is_tx_active && (huart3.gState == HAL_UART_STATE_READY))
        {
            ++s_tx_stall_count;
            s_is_tx_active = false;
            start_next_chunk();
        }
        HAL_NVIC_EnableIRQ(USART3_IRQn);
    }
    return bsp_uart_stream_update(&s_rx_stream);
}

uint32_t bsp_debug_uart_rx_error_count(void)
{
    return bsp_uart_stream_error_count(&s_rx_stream);
}

uint32_t bsp_debug_uart_rx_overflow_count(void)
{
    return bsp_uart_stream_overflow_count(&s_rx_stream);
}

void bsp_debug_uart_on_rx_cplt_from_isr(void)
{
    if (s_is_initialized)
    {
        bsp_uart_stream_on_rx_cplt_from_isr(&s_rx_stream);
    }
}

void bsp_debug_uart_on_tx_cplt_from_isr(void)
{
    if (!s_is_tx_ready || !s_is_tx_active)
    {
        return;
    }
    /* 这一段已经全部移出，现在才从队列里移除，再接着发下一段。 */
    tx_queue_pop(&s_tx_queue, s_tx_chunk);
    start_next_chunk();
}

void bsp_debug_uart_on_error_from_isr(void)
{
    if (s_is_initialized)
    {
        bsp_uart_stream_on_error_from_isr(&s_rx_stream);
    }
}
