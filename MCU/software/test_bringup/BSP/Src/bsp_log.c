#include "bsp_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "usart.h"

/** @brief 单次发送超时，单位为毫秒。 */
#define BSP_LOG_UART_TIMEOUT_MS (1000U)
static const char *const s_level_names[] = {"NONE", "ERROR", "WARN", "INFO",
                                            "DEBUG"};
static bool s_is_log_initialized = false;
bsp_status_t bsp_log_init(void)
{
    s_is_log_initialized = false;
    /* 确保句柄指向 USART3 且已经初始化 */
    if ((huart3.Instance != USART3) ||
        (HAL_UART_GetState(&huart3) == HAL_UART_STATE_RESET))
    {
        return BSP_ERR_CONFIG;
    }
    s_is_log_initialized = true;
    return BSP_OK;
}

bsp_status_t bsp_log_write(const uint8_t *data, size_t length)
{
    HAL_StatusTypeDef write_status;
    if (!s_is_log_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if ((data == NULL) || (length == 0U) || (length > UINT16_MAX))
    {
        return BSP_ERR_PARAM;
    }
    write_status = HAL_UART_Transmit(&huart3, data, (uint16_t)length,
                                     BSP_LOG_UART_TIMEOUT_MS);
    if (write_status == HAL_OK)
    {
        return BSP_OK;
    }
    else if (write_status == HAL_BUSY)
    {
        return BSP_BUSY;
    }
    else if (write_status == HAL_TIMEOUT)
    {
        return BSP_ERR_TIMEOUT;
    }
    else
    {
        return BSP_ERR_HAL;
    }
}

bsp_status_t bsp_log_write_string(const char *message)
{
    if (message == NULL || message[0] == '\0')
    {
        return BSP_ERR_PARAM;
    }
    size_t length = strlen(message);
    return bsp_log_write((const uint8_t *)message, length);
}

bsp_status_t bsp_log_printf(const char *format, ...)
{
    char buffer[BSP_LOG_BUFFER_SIZE];
    va_list args;
    int length;

    if (s_is_log_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    /* 检查格式字符串 */
    if (format == NULL || format[0] == '\0')
    {
        return BSP_ERR_PARAM;
    }

    /* 获取可变参数并格式化到buffer */
    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    /* 检查格式化结果 */
    if (length < 0)
    {
        return BSP_ERR_PARAM;
    }

    if ((size_t)length >= sizeof(buffer))
    {
        return BSP_ERR_BUFFER_TOO_SMALL;
    }
    if (length == 0)
    {
        return BSP_OK; /* 格式化结果为空，不发送 */
    }

    /* 发送字符串 */
    return bsp_log_write((const uint8_t *)buffer, (size_t)length);
}

bsp_status_t bsp_log_print(uint32_t level, const char *tag, const char *format,
                           ...)
{
    if (s_is_log_initialized == false)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    /* 检查参数 */
    if (tag == NULL || tag[0] == '\0')
    {
        return BSP_ERR_PARAM;
    }
    if (format == NULL || format[0] == '\0')
    {
        return BSP_ERR_PARAM;
    }
    if ((level < LOG_LEVEL_ERROR) || (level > LOG_LEVEL_DEBUG))
    {
        return BSP_ERR_PARAM;
    }
    /* 等级过滤 */
    if (level > AITAID_LOG_LEVEL)
    {
        return BSP_OK; /* 等级被过滤，返回成功 */
    }
    /* 把前缀加入格式化字符串 */
    char buffer[BSP_LOG_BUFFER_SIZE];
    int prefix_length =
        snprintf(buffer, sizeof(buffer), "[%lu][%s][%s] ",
                 (unsigned long)HAL_GetTick(), s_level_names[level], tag);

    if (prefix_length < 0)
    {
        return BSP_ERR_PARAM;
    }
    if ((size_t)prefix_length > sizeof(buffer) - 3U)
    {
        return BSP_ERR_BUFFER_TOO_SMALL;
    }

    size_t used_length = (size_t)prefix_length;
    size_t remaining_length =
        sizeof(buffer) - used_length - 2U; /* 留出 CRLF 和空字符 */
    va_list args;
    va_start(args, format);
    int formatted_length =
        vsnprintf(buffer + used_length, remaining_length, format, args);
    va_end(args);
    if (formatted_length < 0)
    {
        return BSP_ERR_PARAM;
    }
    if ((size_t)formatted_length >= remaining_length)
    {
        return BSP_ERR_BUFFER_TOO_SMALL;
    }
    used_length += (size_t)formatted_length;

    /* 添加 CRLF */
    buffer[used_length++] = '\r';
    buffer[used_length++] = '\n';
    buffer[used_length] = '\0'; /* 确保空字符 */

    /* 发送字符串 */
    return bsp_log_write((const uint8_t *)buffer, used_length);
}
