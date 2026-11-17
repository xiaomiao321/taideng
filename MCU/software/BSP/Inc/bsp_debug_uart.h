/**
 * @file bsp_debug_uart.h
 * @brief USART3 调试终端串口：排队后台发送与按字节轮询接收。
 *
 * 接收由内部的 bsp_uart_stream 实例完成（单字节中断加环形缓冲）。
 *
 * 发送不占用主循环：调用者把整行数据放进发送队列就返回，由 USART3 中断在后台
 * 发出。数据分两类：
 *   - 可丢弃的（周期状态、数据记录）：队列快满时整行丢弃并计数，从不等待；
 *   - 不可丢弃的（命令回复、日志）：队列留有专用空间，万一仍放不下，最多等待
 *     调用者给的时间，等发送中断腾出位置。
 *
 * bsp_log 也经由本模块发送，日志和终端文本按入队顺序发出，不会互相打断。
 *
 * @note USART3 的 HAL 回调由集成层（hal_callback_dispatch.c）转发到本模块的
 *       _from_isr 接口；不得有其他代码直接使用 USART3。
 * @note 发送接口只能在主循环上下文调用，不能从中断里调用。
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
 * @brief 准备发送：把 USART3 设成 DEBUG_UART_BAUD 并清空发送队列。
 *
 * @retval BSP_OK 可以发送。
 * @retval BSP_ERR_CONFIG USART3 句柄未初始化。
 * @retval BSP_ERR_HAL 改波特率失败。
 *
 * @note 调用前必须完成 MX_USART3_UART_Init()。由 bsp_log_init() 调用，调试
 *       模式和安卓模式都会执行；必须先于 bsp_debug_uart_init()。
 * @note 不得在发送或接收进行中调用。
 */
bsp_status_t bsp_debug_uart_tx_init(void);

/**
 * @brief 把一段不可丢弃的数据放进发送队列，放不下时限时等待。
 *
 * @param[in] data 待发送数据，不允许为 NULL；整段要么全部入队，要么都不入队。
 * @param[in] length 字节数，1 至发送队列容量减 1。
 * @param[in] timeout_ms 队列放不下时最多等待的毫秒数，0 表示不等待。
 *
 * @retval BSP_OK 已入队，随后由中断发出。
 * @retval BSP_ERR_NOT_INITIALIZED 尚未调用 bsp_debug_uart_tx_init()。
 * @retval BSP_ERR_PARAM 参数无效。
 * @retval BSP_ERR_TIMEOUT 等待后仍放不下，数据已丢弃并计入丢弃数。
 *
 * @note 返回 BSP_OK 只说明已入队，不说明已经发到线路上。
 * @note 全局中断关闭或处于中断上下文时不等待，放不下就立即返回超时。
 */
bsp_status_t bsp_debug_uart_write(const uint8_t *data, size_t length,
                                  uint32_t timeout_ms);

/**
 * @brief 把一段可丢弃的数据放进发送队列，从不等待。
 *
 * @param[in] data 待发送数据，不允许为 NULL。
 * @param[in] length 字节数，1 至发送队列容量减 1。
 * @param[in] is_bulk true 表示量大的数据记录，队列紧张时比周期状态先丢。
 *
 * @retval BSP_OK 已入队。
 * @retval BSP_ERR_NOT_INITIALIZED 尚未调用 bsp_debug_uart_tx_init()。
 * @retval BSP_ERR_PARAM 参数无效。
 * @retval BSP_BUSY 队列里留给更重要数据之外的空间不够，数据已丢弃并计入丢弃数。
 */
bsp_status_t bsp_debug_uart_write_droppable(const uint8_t *data, size_t length,
                                            bool is_bulk);

/**
 * @brief 等发送队列发空，用于马上要停机或复位之前。
 *
 * @param[in] timeout_ms 最多等待的毫秒数。
 *
 * @retval true 队列已发空。
 * @retval false 超时，或当前上下文中断关闭、无法发送。
 */
bool bsp_debug_uart_drain(uint32_t timeout_ms);

/** @brief 因队列放不下而丢弃的数据段累计数，用于发现新的丢弃。 */
uint32_t bsp_debug_uart_tx_drop_count(void);

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
 * @brief 主循环周期调用：从接收错误中恢复，并重新启动意外停下的发送。
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
 * @brief USART3 一段数据发送完成，由 HAL_UART_TxCpltCallback() 转发。
 * @warning 只能在中断上下文调用，且调用前已确认实例为 USART3。
 */
void bsp_debug_uart_on_tx_cplt_from_isr(void);

/**
 * @brief USART3 错误，由 HAL_UART_ErrorCallback() 转发。
 * @warning 只能在中断上下文调用，且调用前已确认实例为 USART3。
 */
void bsp_debug_uart_on_error_from_isr(void);

#endif /* AITAID_BSP_DEBUG_UART_H */
