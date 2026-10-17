/**
 * @file bsp_linux_uart.h
 * @brief USART1 原始字节阻塞发送及中断变长接收接口。
 *
 * 当前版本只负责传输字节，不进行格式化、字符串处理或协议解析。
 * 模块固定使用 huart1，115200 bit/s、8N1、无硬件流控。
 * 公共接口由同一个主循环或任务串行调用，不可重入，不从 ISR 调用。
 * USART1 全局中断必须开启；HAL 回调由集成层（hal_callback_dispatch.c）
 * 在确认实例为 USART1 后转发到本模块的 _from_isr 接口，只保存长度和通知。
 * 不得与其他代码同时使用 USART1。
 */

#ifndef AITAID_BSP_LINUX_UART_H
#define AITAID_BSP_LINUX_UART_H

#include "bsp_common.h"

#include <stddef.h>
#include <stdint.h>

/** @brief 当前 Bring-up 版本一次接收的最大字节数。 */
#define BSP_LINUX_UART_RX_MAX_SIZE (256U)

/**
 * @brief 检查 USART1 是否可用并初始化模块的软件状态。
 *
 * @retval BSP_OK USART1 可用，模块已初始化。
 * @retval BSP_BUSY USART1 正在收发，初始化未成功。
 * @retval BSP_ERR_CONFIG USART1 句柄、配置或初始化状态不符合预期。
 *
 * @note 调用前必须完成 MX_USART1_UART_Init()。
 * @note 检查 huart1.Instance 为 USART1、配置为 115200 bit/s、8N1、无硬件流控，
 *       且发送和接收状态均为 READY。
 * @note 本函数不重新配置 UART，不发送数据，也不清除接收数据。
 * @note 接收进行中调用返回 BUSY，不改变当前接收；其他情况下重新检查 UART。
 * @note 成功后清除软件接收状态，初始结果为 IDLE。不用于中途复位接收。
 */
bsp_status_t bsp_linux_uart_init(void);

/**
 * @brief 阻塞发送指定数量的原始字节。
 *
 * @param[in] data 待发送数据，不允许为 NULL，至少包含 length 个有效字节。
 * @param[in] length 发送字节数，范围为 1 至 UINT16_MAX。
 * @param[in] timeout_ms 整次 HAL 发送调用的超时，单位为毫秒，
 *                       范围为 1 至 UINT32_MAX - 1；不允许无限等待。
 *
 * @retval BSP_OK 指定字节已发送，不代表 Linux 已接收或处理。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_PARAM 指针、长度或超时参数无效。
 * @retval BSP_BUSY HAL UART 发送状态忙。
 * @retval BSP_ERR_TIMEOUT 发送超时，部分字节可能已经发送。
 * @retval BSP_ERR_HAL HAL UART 返回其他错误。
 *
 * @note 先检查初始化，再检查参数；校验通过后才转换 length 为 uint16_t。
 * @note 使用 HAL_UART_Transmit()，不添加前缀、换行或末尾空字符。
 * @note 数据由调用者持有，本函数不保存指针；调用期间数据必须保持有效不变。
 * @warning 本函数阻塞主循环；超时或出错后不能假定整条消息完全没有发送。
 */
bsp_status_t bsp_linux_uart_write(const uint8_t *data, size_t length,
                                  uint32_t timeout_ms);

/**
 * @brief 启动一次中断变长接收，立即返回。
 *
 * @param[out] buffer 接收缓冲区，不允许为 NULL，容量至少为 buffer_size 字节。
 * @param[in] buffer_size 本次可用容量，范围为 1 至 BSP_LINUX_UART_RX_MAX_SIZE，
 *                        不是期望收到的长度。
 * @param[in] timeout_ms 从启动接收起计算的超时，单位为毫秒，
 *                       范围为 1 至 UINT32_MAX - 1；不允许无限等待。
 *
 * @retval BSP_OK 已启动接收，不表示数据已经到达。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化，不修改缓冲区。
 * @retval BSP_ERR_PARAM 参数无效，不修改缓冲区。
 * @retval BSP_BUSY 上次接收尚未处理完，或 HAL 接收状态忙。
 * @retval BSP_ERR_HAL 启动失败或此前终止接收失败。
 *
 * @note 使用 HAL_UARTEx_ReceiveToIdle_IT()。缓冲区由调用者持有，HAL 会保存指针；
 *       直到 receive_update 返回终态前，缓冲区必须有效，不得由调用者读写。
 * @note 启动前先清除通知并设置进行状态；启动后不清除可能已到达的中断通知。
 * @note 失败或 BUSY 时不重置正在进行的接收，不终止他人的接收。
 * @note 不自动追加空字符；完成后按实际长度打印，不直接使用 %s。
 * @note IDLE 表示至少收到一个字节后，RX 线上空闲约一个字符时间；不保证一批数据
 *       就是 Linux 一次 write 的全部内容，也不保证是完整协议消息。
 * @note 长度等于 buffer_size 表示容量已满，不能据此确认发送方是否还有更多数据。
 * @warning 先启动接收，再发送，可在阻塞发送期间通过中断接收即时回传数据。
 * @warning 单缓冲区一次接收结束后不会自动重启，打印和重新启动之间仍有接收空窗。
 */
bsp_status_t bsp_linux_uart_start_receive(uint8_t *buffer, size_t buffer_size,
                                          uint32_t timeout_ms);

/**
 * @brief 在主循环处理接收通知或超时，返回本次实际长度。
 *
 * @param[out] received_length 实际接收长度，不允许为 NULL，不与接收缓冲区重叠。
 *
 * @retval BSP_OK 收到 IDLE 事件或缓冲区满，返回实际长度。
 * @retval BSP_BUSY 接收仍在进行，长度为零。
 * @retval BSP_IDLE 初始化后尚未启动接收，长度为零。
 * @retval BSP_ERR_TIMEOUT 接收超时，已终止接收，部分数据丢弃，长度为零。
 * @retval BSP_ERR_HAL UART 错误或清理失败，长度为零。
 * @retval BSP_ERR_NOT_INITIALIZED 模块未初始化，长度为零。
 * @retval BSP_ERR_PARAM received_length 为 NULL。
 *
 * @note 不等待数据；错误优先于完成，完成优先于超时。终态保留至下次启动。
 * @note 只有 OK 时才允许读取本次缓冲区；读取完后可以启动下一次接收。
 * @note 超时或出错在主循环调用 HAL_UART_AbortReceive()，清理失败后禁止再次启动。
 * @warning 必须周期调用本函数才能处理软件超时；ISR 内不打印、不比较数据。
 */
bsp_status_t bsp_linux_uart_receive_update(size_t *received_length);

/**
 * @brief 记录一次 USART1 接收事件（IDLE 或缓冲区满），由 HAL 回调在集成层转发。
 *
 * @param[in] size 本次接收到的字节数，来自 HAL 回调。
 *
 * @note 只在初始化且接收进行中时记录长度和完成通知，否则忽略。
 * @note size 为 0 或超过启动接收时声明的容量时，改记为错误通知。
 * @warning 只能在中断上下文调用，且调用前已确认对应外设为 USART1。
 */
void bsp_linux_uart_on_rx_event_from_isr(uint16_t size);

/**
 * @brief 记录一次 USART1 错误事件，由 HAL 回调在集成层转发。
 *
 * @note 只在初始化且接收进行中时记录错误通知，否则忽略。
 *       具体清理由 bsp_linux_uart_receive_update() 在主循环执行。
 * @warning 只能在中断上下文调用，且调用前已确认对应外设为 USART1。
 */
void bsp_linux_uart_on_error_from_isr(void);

#endif /* AITAID_BSP_LINUX_UART_H */
