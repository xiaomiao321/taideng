/**
 * @file bsp_debug_uart.h
 * @brief USART3 调试终端串口：限时发送与按字节轮询接收。
 *
 * 接收由内部的 bsp_uart_stream 实例完成（单字节中断加环形缓冲）；发送为阻塞
 * 限时发送。bsp_log 也使用 USART3 发送，两者都在主循环中串行调用，不会冲突。
 *
 * @note USART3 的 HAL 接收回调由集成层（hal_callback_dispatch.c）转发到本模块
 *       的 _from_isr 接口；除 bsp_log 的发送外，不得有其他代码使用 USART3。
 */

#ifndef AITAID_BSP_DEBUG_UART_H
#define AITAID_BSP_DEBUG_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp_common.h"

/**
 * @brief 检查 USART3 配置并启动接收。
 *
 * @retval BSP_OK 已开始接收。
 * @retval BSP_ERR_CONFIG USART3 句柄未初始化。
 * @retval BSP_BUSY USART3 接收被其他代码占用。
 * @retval BSP_ERR_HAL HAL 启动接收失败。
 *
 * @note 调用前必须完成 MX_USART3_UART_Init()。
 */
bsp_status_t bsp_debug_uart_init(void);

/**
 * @brief 限时阻塞发送。
 *
 * @param[in] data 待发送数据，不允许为 NULL。
 * @param[in] length 字节数，1 至 UINT16_MAX。
 * @param[in] timeout_ms 超时，单位毫秒，不允许为 0。
 *
 * @retval BSP_OK 已发送。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_PARAM 参数无效。
 * @retval BSP_BUSY HAL 发送状态忙。
 * @retval BSP_ERR_TIMEOUT 发送超时，可能已发出部分字节。
 * @retval BSP_ERR_HAL HAL 返回其他错误。
 */
bsp_status_t bsp_debug_uart_write(const uint8_t *data, size_t length,
                                  uint32_t timeout_ms);

/**
 * @brief 取出一个已收到的字节。
 *
 * @retval true 已取出，写入 byte。
 * @retval false 没有新字节，或模块尚未初始化。
 */
bool bsp_debug_uart_read(uint8_t *byte);

/** @brief 丢弃尚未读取的接收字节。 */
void bsp_debug_uart_flush(void);

/**
 * @brief 主循环周期调用：从接收错误中恢复。
 *
 * @retval BSP_OK 正常，或错误已恢复。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_HAL 无法重新启动接收，下次调用再试。
 */
bsp_status_t bsp_debug_uart_update(void);

/** @brief 接收错误累计次数，用于发现新错误。 */
uint32_t bsp_debug_uart_rx_error_count(void);

/** @brief 接收缓冲满而丢弃的字节累计数，用于发现新溢出。 */
uint32_t bsp_debug_uart_rx_overflow_count(void);

/**
 * @brief USART3 单字节接收完成，由 HAL_UART_RxCpltCallback() 转发。
 * @warning 只能在中断上下文调用，且调用前已确认实例为 USART3。
 */
void bsp_debug_uart_on_rx_cplt_from_isr(void);

/**
 * @brief USART3 错误，由 HAL_UART_ErrorCallback() 转发。
 * @warning 只能在中断上下文调用，且调用前已确认实例为 USART3。
 */
void bsp_debug_uart_on_error_from_isr(void);

#endif /* AITAID_BSP_DEBUG_UART_H */
