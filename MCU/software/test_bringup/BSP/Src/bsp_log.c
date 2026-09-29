#include "bsp_log.h"

#include <stdbool.h>
#include <string.h>

#include "main.h"
#include "usart.h"

#define BSP_LOG_UART_TIMEOUT_MS (1000U)  // 发送超时，单位为毫秒
static bool s_is_log_initialized = false;
bsp_log_status_t bsp_log_init(void)
{
    s_is_log_initialized = false;
    // 确保 USART3 已经初始化
    if (HAL_UART_GetState(&huart3) == HAL_UART_STATE_RESET)
    {
        return BSP_LOG_STATUS_TRANSMIT_ERROR;
    }
    s_is_log_initialized = true;
    return BSP_LOG_STATUS_OK;
}

bsp_log_status_t bsp_log_write(const uint8_t *data, size_t length)
{
    HAL_StatusTypeDef write_status;
    if (!s_is_log_initialized)
    {
        return BSP_LOG_STATUS_NOT_INITIALIZED;
    }
    if ((data == NULL) || (length == 0U) || (length > UINT16_MAX))
    {
        return BSP_LOG_STATUS_INVALID_ARGUMENT;
    }
    write_status = HAL_UART_Transmit(&huart3, (uint8_t *)data, (uint16_t)length,
                                     BSP_LOG_UART_TIMEOUT_MS);
    if (write_status == HAL_OK)
    {
        return BSP_LOG_STATUS_OK;
    }
    else if (write_status == HAL_BUSY)
    {
        return BSP_LOG_STATUS_BUSY;
    }
    else if (write_status == HAL_TIMEOUT)
    {
        return BSP_LOG_STATUS_TIMEOUT;
    }
    else
    {
        return BSP_LOG_STATUS_TRANSMIT_ERROR;
    }
}

bsp_log_status_t bsp_log_write_string(const char *message)
{
    if (message == NULL || message[0] == '\0')
    {
        return BSP_LOG_STATUS_INVALID_ARGUMENT;
    }
    size_t length = strlen(message);
    return bsp_log_write((const uint8_t *)message, length);
}