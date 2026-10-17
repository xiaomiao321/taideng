#include "bsp_linux_uart.h"

#include <stdbool.h>

#include "bsp_uart_stream.h"
#include "usart.h"

/** @brief 字节流（透传）模式的接收环形缓冲容量，必须是 2 的幂。 */
#define BSP_LINUX_UART_STREAM_RING_SIZE (1024U)

static bool s_is_initialized = false;
static volatile bool s_is_rx_active = false;
static volatile bool s_has_rx_event = false;
static volatile bool s_has_rx_error = false;
static volatile uint16_t s_rx_length = 0U;
static uint16_t s_rx_capacity = 0U;
static uint32_t s_rx_start_ms = 0U;
static uint32_t s_rx_timeout_ms = 0U;
static bool s_has_cleanup_failed = false;
static bsp_status_t s_rx_result = BSP_IDLE;
static uint8_t s_stream_ring[BSP_LINUX_UART_STREAM_RING_SIZE];
static bsp_uart_stream_t s_stream;

static bsp_status_t convert_hal_status(HAL_StatusTypeDef status)
{
    switch (status)
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

/* Stop accepting notifications before cleaning up this module's reception. */
static bsp_status_t abort_receive(bsp_status_t result)
{
    s_is_rx_active = false;
    if (HAL_UART_AbortReceive(&huart1) != HAL_OK)
    {
        s_has_cleanup_failed = true;
        result = BSP_ERR_HAL;
    }
    s_rx_length = 0U;
    s_has_rx_event = false;
    s_has_rx_error = false;
    s_rx_result = result;
    return s_rx_result;
}

bsp_status_t bsp_linux_uart_init(void)
{
    if (s_is_rx_active ||
        (s_is_initialized && bsp_uart_stream_is_active(&s_stream)))
    {
        return BSP_BUSY;
    }
    s_is_initialized = false;
    if ((huart1.Instance != USART1) ||
        (huart1.gState == HAL_UART_STATE_RESET) ||
        (huart1.RxState == HAL_UART_STATE_RESET))
    {
        return BSP_ERR_CONFIG;
    }
    if ((huart1.Init.BaudRate != 115200U) ||
        (huart1.Init.WordLength != UART_WORDLENGTH_8B) ||
        (huart1.Init.StopBits != UART_STOPBITS_1) ||
        (huart1.Init.Parity != UART_PARITY_NONE) ||
        (huart1.Init.HwFlowCtl != UART_HWCONTROL_NONE))
    {
        return BSP_ERR_CONFIG;
    }
    if (huart1.gState != HAL_UART_STATE_READY ||
        huart1.RxState != HAL_UART_STATE_READY)
    {
        return BSP_BUSY;
    }
    s_has_rx_event = false;
    s_has_rx_error = false;
    s_rx_length = 0U;
    s_rx_capacity = 0U;
    s_rx_start_ms = 0U;
    s_rx_timeout_ms = 0U;
    s_has_cleanup_failed = false;
    s_rx_result = BSP_IDLE;
    bsp_status_t status = bsp_uart_stream_init(
        &s_stream, &huart1, s_stream_ring, sizeof(s_stream_ring));
    if (status != BSP_OK)
    {
        return status;
    }
    s_is_initialized = true;
    return BSP_OK;
}

bsp_status_t bsp_linux_uart_write(const uint8_t *data, size_t length,
                                  uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (data == NULL || length == 0U || length > UINT16_MAX ||
        timeout_ms == 0U || timeout_ms == UINT32_MAX)
    {
        return BSP_ERR_PARAM;
    }
    if (huart1.gState != HAL_UART_STATE_READY)
    {
        return BSP_BUSY;
    }
    HAL_StatusTypeDef status =
        HAL_UART_Transmit(&huart1, data, (uint16_t)length, timeout_ms);
    return convert_hal_status(status);
}

bsp_status_t bsp_linux_uart_start_receive(uint8_t *buffer, size_t buffer_size,
                                          uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (buffer == NULL || buffer_size == 0U ||
        buffer_size > BSP_LINUX_UART_RX_MAX_SIZE || timeout_ms == 0U ||
        timeout_ms == UINT32_MAX)
    {
        return BSP_ERR_PARAM;
    }
    if (s_has_cleanup_failed)
    {
        /* 上次终止接收失败：重试一次，成功即可恢复，不必复位整机。 */
        if (HAL_UART_AbortReceive(&huart1) != HAL_OK)
        {
            return BSP_ERR_HAL;
        }
        s_has_cleanup_failed = false;
    }
    if (bsp_uart_stream_is_active(&s_stream))
    {
        return BSP_BUSY;
    }
    if (s_is_rx_active || huart1.RxState != HAL_UART_STATE_READY)
    {
        return BSP_BUSY;
    }
    s_has_rx_event = false;
    s_has_rx_error = false;
    s_rx_length = 0U;
    s_rx_capacity = (uint16_t)buffer_size;
    s_rx_start_ms = HAL_GetTick();
    s_rx_timeout_ms = timeout_ms;
    s_rx_result = BSP_BUSY;
    s_is_rx_active = true;
    HAL_StatusTypeDef status =
        HAL_UARTEx_ReceiveToIdle_IT(&huart1, buffer, (uint16_t)buffer_size);
    if (status == HAL_BUSY)
    {
        s_is_rx_active = false;
        s_rx_result = BSP_BUSY;
        return s_rx_result;
    }
    if (status != HAL_OK)
    {
        return abort_receive(BSP_ERR_HAL);
    }
    return BSP_OK;
}

bsp_status_t bsp_linux_uart_receive_update(size_t *received_length)
{
    if (received_length != NULL)
    {
        *received_length = 0U;
    }
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (received_length == NULL)
    {
        return BSP_ERR_PARAM;
    }
    if (!s_is_rx_active)
    {
        if (s_rx_result == BSP_OK)
        {
            *received_length = (size_t)s_rx_length;
        }
        return s_rx_result;
    }
    if (s_has_rx_error)
    {
        return abort_receive(BSP_ERR_HAL);
    }
    if (s_has_rx_event)
    {
        s_is_rx_active = false;
        s_rx_result = BSP_OK;
        *received_length = (size_t)s_rx_length;
        return s_rx_result;
    }
    if ((HAL_GetTick() - s_rx_start_ms) >= s_rx_timeout_ms)
    {
        return abort_receive(BSP_ERR_TIMEOUT);
    }
    return BSP_BUSY;
}

void bsp_linux_uart_on_rx_event_from_isr(uint16_t size)
{
    if (!s_is_initialized || !s_is_rx_active)
    {
        return;
    }
    if ((size == 0U) || (size > s_rx_capacity))
    {
        s_has_rx_error = true;
        return;
    }
    s_rx_length = size;
    s_has_rx_event = true;
}

void bsp_linux_uart_on_error_from_isr(void)
{
    if (!s_is_initialized)
    {
        return;
    }
    if (s_is_rx_active)
    {
        s_has_rx_error = true;
    }
    bsp_uart_stream_on_error_from_isr(&s_stream);
}

bsp_status_t bsp_linux_uart_stream_start(void)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if (s_is_rx_active)
    {
        return BSP_BUSY;
    }
    return bsp_uart_stream_start(&s_stream);
}

bsp_status_t bsp_linux_uart_stream_stop(void)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    return bsp_uart_stream_stop(&s_stream);
}

bool bsp_linux_uart_stream_is_active(void)
{
    return s_is_initialized && bsp_uart_stream_is_active(&s_stream);
}

bool bsp_linux_uart_stream_read(uint8_t *byte)
{
    if (!s_is_initialized || (byte == NULL))
    {
        return false;
    }
    return bsp_uart_stream_read(&s_stream, byte);
}

bsp_status_t bsp_linux_uart_stream_update(void)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    return bsp_uart_stream_update(&s_stream);
}

uint32_t bsp_linux_uart_stream_error_count(void)
{
    return bsp_uart_stream_error_count(&s_stream);
}

uint32_t bsp_linux_uart_stream_overflow_count(void)
{
    return bsp_uart_stream_overflow_count(&s_stream);
}

void bsp_linux_uart_on_rx_cplt_from_isr(void)
{
    if (s_is_initialized)
    {
        bsp_uart_stream_on_rx_cplt_from_isr(&s_stream);
    }
}
