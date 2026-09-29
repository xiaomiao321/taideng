/**
 * @file bsp_log.h
 * @brief 通过板载调试串口发送 Bring-up 日志的基础接口。
 */

#ifndef AITAID_BSP_LOG_H
#define AITAID_BSP_LOG_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief 日志发送操作的返回状态。
 */
typedef enum
{
    BSP_LOG_STATUS_OK = 0,
    BSP_LOG_STATUS_NOT_INITIALIZED,
    BSP_LOG_STATUS_INVALID_ARGUMENT,
    BSP_LOG_STATUS_BUSY,
    BSP_LOG_STATUS_TIMEOUT,
    BSP_LOG_STATUS_TRANSMIT_ERROR
} bsp_log_status_t;

/**
 * @brief 初始化 Bring-up 日志模块。
 *
 * @retval BSP_LOG_STATUS_OK 初始化成功。
 * @retval BSP_LOG_STATUS_TRANSMIT_ERROR 调试串口未处于可用状态。
 *
 * @note 调用本函数前必须已经执行 MX_USART3_UART_Init()。
 * @note 本模块固定使用 USART3，115200 bit/s、8N1、无硬件流控。
 */
bsp_log_status_t bsp_log_init(void);

/**
 * @brief 通过调试串口阻塞发送一段原始字节数据。
 *
 * @param[in] data 待发送数据的首地址，不允许为 NULL。
 * @param[in] length 待发送数据长度，单位为字节，必须大于 0。
 *
 * @retval BSP_LOG_STATUS_OK 发送成功。
 * @retval BSP_LOG_STATUS_NOT_INITIALIZED 日志模块尚未初始化。
 * @retval BSP_LOG_STATUS_INVALID_ARGUMENT 参数无效。
 * @retval BSP_LOG_STATUS_BUSY UART 当前忙。
 * @retval BSP_LOG_STATUS_TIMEOUT UART 发送超时。
 * @retval BSP_LOG_STATUS_TRANSMIT_ERROR UART 发生其他发送错误。
 *
 * @warning 本函数为阻塞接口，不能从中断服务程序调用。
 */
bsp_log_status_t bsp_log_write(const uint8_t *data, size_t length);

/**
 * @brief 通过调试串口阻塞发送以空字符结尾的字符串。
 *
 * @param[in] message 待发送字符串，不允许为 NULL。
 *
 * @retval BSP_LOG_STATUS_OK 发送成功。
 * @retval BSP_LOG_STATUS_NOT_INITIALIZED 日志模块尚未初始化。
 * @retval BSP_LOG_STATUS_INVALID_ARGUMENT 参数无效。
 * @retval BSP_LOG_STATUS_BUSY UART 当前忙。
 * @retval BSP_LOG_STATUS_TIMEOUT UART 发送超时。
 * @retval BSP_LOG_STATUS_TRANSMIT_ERROR UART 发生其他发送错误。
 *
 * @warning 本函数为阻塞接口，不能从中断服务程序调用。
 */
bsp_log_status_t bsp_log_write_string(const char *message);

#endif /* AITAID_BSP_LOG_H */
