#include "bsp_linux_uart.h"

#include <stdbool.h>

#include "usart.h"

static bool s_is_initialized = false;
static volatile bool s_is_rx_active = false;
static volatile bool s_has_rx_event = false;
static volatile bool s_has_rx_error = false;
static volatile uint16_t s_rx_length = 0U;
static uint16_t s_rx_capacity = 0U;
static uint32_t s_rx_start_ms = 0U;
static uint32_t s_rx_timeout_ms = 0U;
static bool s_has_cleanup_failed = false;
static bsp_linux_uart_status_t s_rx_result = BSP_LINUX_UART_STATUS_IDLE;

static bsp_linux_uart_status_t convert_hal_status(HAL_StatusTypeDef status)
{
    switch (status)
    {
        case HAL_OK:
            return BSP_LINUX_UART_STATUS_OK;
        case HAL_BUSY:
            return BSP_LINUX_UART_STATUS_BUSY;
        case HAL_TIMEOUT:
            return BSP_LINUX_UART_STATUS_TIMEOUT;
        default:
            return BSP_LINUX_UART_STATUS_UART_ERROR;
    }
}

/* Stop accepting notifications before cleaning up this module's reception. */
static bsp_linux_uart_status_t abort_receive(bsp_linux_uart_status_t result)
{
    s_is_rx_active = false;
    if (HAL_UART_AbortReceive(&huart1) != HAL_OK)
    {
        s_has_cleanup_failed = true;
        result = BSP_LINUX_UART_STATUS_UART_ERROR;
    }
    s_rx_length = 0U;
    s_has_rx_event = false;
    s_has_rx_error = false;
    s_rx_result = result;
    return s_rx_result;
}

bsp_linux_uart_status_t bsp_linux_uart_init(void)
{
    if (s_is_rx_active)
    {
        return BSP_LINUX_UART_STATUS_BUSY;
    }
    s_is_initialized = false;
    if ((huart1.Instance != USART1) || (huart1.gState == HAL_UART_STATE_RESET) ||
        (huart1.RxState == HAL_UART_STATE_RESET))
    {
        return BSP_LINUX_UART_STATUS_UART_ERROR;
    }
    if (huart1.gState != HAL_UART_STATE_READY ||
        huart1.RxState != HAL_UART_STATE_READY)
    {
        return BSP_LINUX_UART_STATUS_BUSY;
    }
    s_has_rx_event = false;
    s_has_rx_error = false;
    s_rx_length = 0U;
    s_rx_capacity = 0U;
    s_rx_start_ms = 0U;
    s_rx_timeout_ms = 0U;
    s_has_cleanup_failed = false;
    s_rx_result = BSP_LINUX_UART_STATUS_IDLE;
    s_is_initialized = true;
    return BSP_LINUX_UART_STATUS_OK;
}

bsp_linux_uart_status_t bsp_linux_uart_write(const uint8_t *data, size_t length,
                                             uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_LINUX_UART_STATUS_NOT_INITIALIZED;
    }
    if (data == NULL || length == 0U || length > UINT16_MAX || timeout_ms == 0U ||
        timeout_ms == UINT32_MAX)
    {
        return BSP_LINUX_UART_STATUS_INVALID_ARGUMENT;
    }
    if (huart1.gState != HAL_UART_STATE_READY)
    {
        return BSP_LINUX_UART_STATUS_BUSY;
    }
    HAL_StatusTypeDef status =
        HAL_UART_Transmit(&huart1, data, (uint16_t)length, timeout_ms);
    return convert_hal_status(status);
}

bsp_linux_uart_status_t bsp_linux_uart_start_receive(uint8_t *buffer,
                                                     size_t buffer_size,
                                                     uint32_t timeout_ms)
{
    if (s_is_initialized == false)
    {
        return BSP_LINUX_UART_STATUS_NOT_INITIALIZED;
    }
    if (buffer == NULL || buffer_size == 0U ||
        buffer_size > BSP_LINUX_UART_RX_MAX_SIZE || timeout_ms == 0U ||
        timeout_ms == UINT32_MAX)
    {
        return BSP_LINUX_UART_STATUS_INVALID_ARGUMENT;
    }
    if (s_has_cleanup_failed)
    {
        return BSP_LINUX_UART_STATUS_UART_ERROR;
    }
    if (s_is_rx_active || huart1.RxState != HAL_UART_STATE_READY)
    {
        return BSP_LINUX_UART_STATUS_BUSY;
    }
    s_has_rx_event = false;
    s_has_rx_error = false;
    s_rx_length = 0U;
    s_rx_capacity = (uint16_t)buffer_size;
    s_rx_start_ms = HAL_GetTick();
    s_rx_timeout_ms = timeout_ms;
    s_rx_result = BSP_LINUX_UART_STATUS_BUSY;
    s_is_rx_active = true;
    HAL_StatusTypeDef status =
        HAL_UARTEx_ReceiveToIdle_IT(&huart1, buffer, (uint16_t)buffer_size);
    if (status == HAL_BUSY)
    {
        s_is_rx_active = false;
        s_rx_result = BSP_LINUX_UART_STATUS_BUSY;
        return s_rx_result;
    }
    if (status != HAL_OK)
    {
        return abort_receive(BSP_LINUX_UART_STATUS_UART_ERROR);
    }
    return BSP_LINUX_UART_STATUS_OK;
}

bsp_linux_uart_status_t bsp_linux_uart_receive_update(size_t *received_length)
{
    if (received_length != NULL)
    {
        *received_length = 0U;
    }
    if (!s_is_initialized)
    {
        return BSP_LINUX_UART_STATUS_NOT_INITIALIZED;
    }
    if (received_length == NULL)
    {
        return BSP_LINUX_UART_STATUS_INVALID_ARGUMENT;
    }
    if (!s_is_rx_active)
    {
        if (s_rx_result == BSP_LINUX_UART_STATUS_OK)
        {
            *received_length = (size_t)s_rx_length;
        }
        return s_rx_result;
    }
    if (s_has_rx_error)
    {
        return abort_receive(BSP_LINUX_UART_STATUS_UART_ERROR);
    }
    if (s_has_rx_event)
    {
        s_is_rx_active = false;
        s_rx_result = BSP_LINUX_UART_STATUS_OK;
        *received_length = (size_t)s_rx_length;
        return s_rx_result;
    }
    if ((HAL_GetTick() - s_rx_start_ms) >= s_rx_timeout_ms)
    {
        return abort_receive(BSP_LINUX_UART_STATUS_TIMEOUT);
    }
    return BSP_LINUX_UART_STATUS_BUSY;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    if ((huart == &huart1) && s_is_initialized && s_is_rx_active)
    {
        if ((size == 0U) || (size > s_rx_capacity))
        {
            s_has_rx_error = true;
            return;
        }
        s_rx_length = size;
        s_has_rx_event = true;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if ((huart == &huart1) && s_is_initialized && s_is_rx_active)
    {
        s_has_rx_error = true;
    }
}
