#include "bsp_uart_loopback.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "usart.h"

static bool s_is_initialized = false;
static volatile bool s_is_rx_complete = false;
static volatile bool s_is_rx_error = false;
static volatile bool s_is_test_active = false;
static bool s_has_cleanup_failed = false;
static uint32_t s_start_tick_ms = 0U;
static bsp_uart_loopback_status_t s_result = BSP_UART_LOOPBACK_STATUS_IDLE;
static uint8_t s_tx_buffer[BSP_UART_LOOPBACK_LENGTH] = {0};
static uint8_t s_rx_buffer[BSP_UART_LOOPBACK_LENGTH] = {0};

/* Stop notifications before aborting; the result remains available to main. */
static bsp_uart_loopback_status_t abort_test(bsp_uart_loopback_status_t result)
{
    s_is_test_active = false;
    if (HAL_UART_AbortReceive(&huart1) != HAL_OK)
    {
        s_has_cleanup_failed = true;
        result = BSP_UART_LOOPBACK_STATUS_UART_ERROR;
    }
    s_is_rx_complete = false;
    s_is_rx_error = false;
    s_result = result;
    return s_result;
}

void bsp_uart_loopback_init(void)
{
    memcpy(s_tx_buffer, "ABCDEFGH", sizeof(s_tx_buffer));
    memset(s_rx_buffer, 0, sizeof(s_rx_buffer));
    s_is_rx_complete = false;
    s_is_rx_error = false;
    s_is_test_active = false;
    s_has_cleanup_failed = false;
    s_start_tick_ms = 0U;
    s_result = BSP_UART_LOOPBACK_STATUS_IDLE;
    s_is_initialized = true;
}

bsp_uart_loopback_status_t bsp_uart_loopback_start(void)
{
    if (s_is_initialized == false)
    {
        return BSP_UART_LOOPBACK_STATUS_NOT_INITIALIZED;
    }
    if (s_has_cleanup_failed)
    {
        return BSP_UART_LOOPBACK_STATUS_UART_ERROR;
    }
    if (s_is_test_active || (huart1.gState != HAL_UART_STATE_READY) ||
        (huart1.RxState != HAL_UART_STATE_READY))
    {
        return BSP_UART_LOOPBACK_STATUS_BUSY;
    }
    s_is_rx_complete = false;
    s_is_rx_error = false;
    memset(s_rx_buffer, 0, sizeof(s_rx_buffer));
    s_is_test_active = true;
    s_start_tick_ms = HAL_GetTick();
    s_result = BSP_UART_LOOPBACK_STATUS_BUSY;
    HAL_StatusTypeDef status =
        HAL_UART_Receive_IT(&huart1, s_rx_buffer, BSP_UART_LOOPBACK_LENGTH);
    if (status != HAL_OK)
    {
        s_is_test_active = false;
        s_result = (status == HAL_BUSY) ? BSP_UART_LOOPBACK_STATUS_BUSY :
                                         BSP_UART_LOOPBACK_STATUS_UART_ERROR;
        return s_result;
    }
    status = HAL_UART_Transmit(&huart1, s_tx_buffer, BSP_UART_LOOPBACK_LENGTH,
                              BSP_UART_LOOPBACK_TX_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        bsp_uart_loopback_status_t result = BSP_UART_LOOPBACK_STATUS_UART_ERROR;
        if (status == HAL_TIMEOUT)
        {
            result = BSP_UART_LOOPBACK_STATUS_TIMEOUT;
        }
        else if (status == HAL_BUSY)
        {
            result = BSP_UART_LOOPBACK_STATUS_BUSY;
        }
        return abort_test(result);
    }
    return BSP_UART_LOOPBACK_STATUS_OK;
}

bsp_uart_loopback_status_t bsp_uart_loopback_update(void)
{
    if (s_is_initialized == false)
    {
        return BSP_UART_LOOPBACK_STATUS_NOT_INITIALIZED;
    }
    if (!s_is_test_active)
    {
        return s_result;
    }
    if (s_is_rx_error)
    {
        return abort_test(BSP_UART_LOOPBACK_STATUS_UART_ERROR);
    }
    if (s_is_rx_complete)
    {
        s_is_test_active = false;
        if (memcmp(s_tx_buffer, s_rx_buffer, BSP_UART_LOOPBACK_LENGTH) == 0)
        {
            s_result = BSP_UART_LOOPBACK_STATUS_OK;
        }
        else
        {
            s_result = BSP_UART_LOOPBACK_STATUS_DATA_MISMATCH;
        }
        return s_result;
    }
    if ((HAL_GetTick() - s_start_tick_ms) >= BSP_UART_LOOPBACK_RX_TIMEOUT_MS)
    {
        return abort_test(BSP_UART_LOOPBACK_STATUS_TIMEOUT);
    }
    return BSP_UART_LOOPBACK_STATUS_BUSY;
}

void bsp_uart_loopback_on_rx_complete_from_isr(void)
{
    if (s_is_initialized && s_is_test_active)
    {
        s_is_rx_complete = true;
    }
}

void bsp_uart_loopback_on_error_from_isr(void)
{
    if (s_is_initialized && s_is_test_active)
    {
        s_is_rx_error = true;
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1)
    {
        bsp_uart_loopback_on_rx_complete_from_isr();
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1)
    {
        bsp_uart_loopback_on_error_from_isr();
    }
}
