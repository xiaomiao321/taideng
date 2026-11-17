#include "bsp_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bsp_debug_uart.h"
#include "main.h"

/** @brief 发送队列放不下时最多等待的时间，单位为毫秒。 */
#define BSP_LOG_QUEUE_WAIT_MS (20U)

/** @brief ERROR 级日志等待发完的最长时间，单位为毫秒。 */
#define BSP_LOG_ERROR_DRAIN_MS (100U)
static const char *const s_level_names[] = {"NONE", "ERROR", "WARN", "INFO",
                                            "DEBUG"};
static bool s_is_log_initialized = false;
bsp_status_t bsp_log_init(void)
{
    s_is_log_initialized = false;
    /* 日志和调试终端共用 USART3 的发送队列，波特率也在这里按配置设好。 */
    bsp_status_t status = bsp_debug_uart_tx_init();
    if (status != BSP_OK)
    {
        return status;
    }
    s_is_log_initialized = true;
    return BSP_OK;
}

bsp_status_t bsp_log_write(const uint8_t *data, size_t length)
{
    if (!s_is_log_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if ((data == NULL) || (length == 0U) || (length > UINT16_MAX))
    {
        return BSP_ERR_PARAM;
    }
    /* 日志不可丢弃：入队后由中断发出，队列放不下时才短暂等待。 */
    return bsp_debug_uart_write(data, length, BSP_LOG_QUEUE_WAIT_MS);
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
    bsp_status_t status = bsp_log_write((const uint8_t *)buffer, used_length);
    if (level == LOG_LEVEL_ERROR)
    {
        /* ERROR 之后往往紧跟着停机或复位，等它真正发到线路上再返回。 */
        (void)bsp_debug_uart_drain(BSP_LOG_ERROR_DRAIN_MS);
    }
    return status;
}
