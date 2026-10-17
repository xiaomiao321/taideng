#include "bsp_debug_uart.h"

#include "bsp_uart_stream.h"
#include "usart.h"

/** @brief 接收环形缓冲容量，必须是 2 的幂。 */
#define BSP_DEBUG_UART_RX_RING_SIZE (256U)

static uint8_t s_rx_ring[BSP_DEBUG_UART_RX_RING_SIZE];
static bsp_uart_stream_t s_rx_stream;
static bool s_is_initialized = false;

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
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if ((data == NULL) || (length == 0U) || (length > UINT16_MAX) ||
        (timeout_ms == 0U))
    {
        return BSP_ERR_PARAM;
    }

    switch (HAL_UART_Transmit(&huart3, data, (uint16_t)length, timeout_ms))
    {
        case HAL_OK:
            return BSP_OK;
        case HAL_BUSY:
            return BSP_BUSY;
        case HAL_TIMEOUT:
            return BSP_ERR_TIMEOUT;
        default:
            return BSP_ERR_HAL;
    }
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

void bsp_debug_uart_on_error_from_isr(void)
{
    if (s_is_initialized)
    {
        bsp_uart_stream_on_error_from_isr(&s_rx_stream);
    }
}
