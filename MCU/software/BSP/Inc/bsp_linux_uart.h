/**
 * @file bsp_linux_uart.h
 * @brief USART1 与 Android 主机通信的原始字节收发：双缓冲变长接收 + 非阻塞发送队列。
 *
 * 本版本修复《通讯与采样问题清单》的两条 P0：
 * - A1：接收改为**双缓冲乒乓**，一块收满或 IDLE 时在中断里立刻把另一块挂上去，
 *   主循环处理数据期间接收不会出现空窗；旧版本是"收到一批 -> 主循环处理 ->
 *   重新启动接收"，重新启动之前的字节直接丢失。
 * - A3：发送改为**队列 + 中断发送**，write() 只入队，不再用
 *   HAL_UART_Transmit() 阻塞主循环最多 100 ms。
 *
 * 不用 DMA 的原因：本工程的 CubeMX 配置里 USART1 没有绑定 DMA 流（DMA2_Stream0
 * 只给 ADC1 用），加 DMA 需要改 MSP/NVIC 并且在无硬件条件下无法验证；双缓冲中断
 * 接收在 115200 下每块 256 字节约 22 ms，足以覆盖主循环的处理时间。后续若要上
 * 921600 或更高，再改成循环 DMA。
 *
 * 模块固定使用 huart1，115200 bit/s、8N1、无硬件流控。
 * 公共接口由同一个主循环串行调用，不可重入，不从 ISR 调用。
 * HAL 回调由集成层（hal_callback_dispatch.c）在确认实例为 USART1 后转发到本模块的
 * _from_isr 接口；中断里只搬下标，不解析、不打印。
 */

#ifndef AITAID_BSP_LINUX_UART_H
#define AITAID_BSP_LINUX_UART_H

#include "bsp_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief 单次接收缓冲的字节数；HAL 一块最多收这么多就回调。 */
#define BSP_LINUX_UART_RX_BLOCK_SIZE (256U)

/** @brief 发送队列容量，必须是 2 的幂。 */
#define BSP_LINUX_UART_TX_RING_SIZE (512U)

/**
 * @brief 检查 USART1 是否可用、初始化模块并立即启动接收。
 *
 * @retval BSP_OK USART1 可用，接收已启动。
 * @retval BSP_BUSY USART1 正被其他代码使用。
 * @retval BSP_ERR_CONFIG USART1 句柄、配置或初始化状态不符合预期。
 * @retval BSP_ERR_HAL 启动接收失败。
 *
 * @note 调用前必须完成 MX_USART1_UART_Init()。
 * @note 成功后接收会一直挂着：主循环只要周期调用 bsp_linux_uart_read() 取数据，
 *       不需要再手动启动接收。
 */
bsp_status_t bsp_linux_uart_init(void);

/**
 * @brief 把数据放入发送队列，立即返回。
 *
 * @param[in] data 待发送数据，不允许为 NULL。
 * @param[in] length 字节数，1 至 UINT16_MAX。
 * @param[in] timeout_ms 保留参数，仅用于兼容旧调用点；不再阻塞等待。
 *
 * @retval BSP_OK 整条数据已入队。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_PARAM 参数无效。
 * @retval BSP_ERR_TIMEOUT 队列空间不足，整条丢弃并计入丢弃字节数。
 * @retval BSP_ERR_HAL 启动 HAL 中断发送失败，数据仍在队列里，下次 update() 重试。
 *
 * @note 成功只表示"已排队"。队列满时整条丢弃，绝不发送半条报文。
 */
bsp_status_t bsp_linux_uart_write(const uint8_t *data, size_t length,
                                  uint32_t timeout_ms);

/** @brief 发送队列剩余可用字节数。 */
size_t bsp_linux_uart_tx_free(void);

/**
 * @brief 取出已收到的字节。
 *
 * @param[out] dst 输出缓冲，不允许为 NULL。
 * @param[in] cap dst 的容量，字节数。
 *
 * @return 实际拷贝的字节数；0 表示当前没有新数据。
 *
 * @note 不会等待，也不会跨块拼接语义；调用者应循环调用直到返回 0。
 * @note 返回 0 不代表链路空闲，只代表这一瞬间没有可取的整块数据。
 */
size_t bsp_linux_uart_read(uint8_t *dst, size_t cap);

/**
 * @brief 主循环周期调用：推进发送队列，并从接收错误中恢复。
 *
 * @retval BSP_OK 正常，或错误已恢复。
 * @retval BSP_ERR_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_ERR_HAL 重新启动接收失败，下次调用再试。
 */
bsp_status_t bsp_linux_uart_update(void);

/** @brief 接收块来不及取走而被丢弃的次数。 */
uint32_t bsp_linux_uart_rx_overflow_count(void);

/** @brief 发送队列满而整条丢弃的字节累计数。 */
uint32_t bsp_linux_uart_tx_overflow_count(void);

/** @brief 收发错误（帧错误、噪声、HAL 启动失败）的累计次数。 */
uint32_t bsp_linux_uart_error_count(void);

/**
 * @brief 记录一次 USART1 接收事件（IDLE 或缓冲满），由 HAL 回调在集成层转发。
 *
 * @param[in] size 本次接收到的字节数，来自 HAL 回调。
 *
 * @note 中断里只做三件事：记录长度、切换乒乓下标、把另一块重新挂上。
 * @warning 只能在中断上下文调用，且调用前已确认对应外设为 USART1。
 */
void bsp_linux_uart_on_rx_event_from_isr(uint16_t size);

/**
 * @brief 记录一次 USART1 错误事件，由 HAL 回调在集成层转发。
 * @warning 只能在中断上下文调用，且调用前已确认对应外设为 USART1。
 */
void bsp_linux_uart_on_error_from_isr(void);

/**
 * @brief USART1 一块数据发送完成，由 HAL_UART_TxCpltCallback() 转发。
 * @warning 只能在中断上下文调用，且调用前已确认对应外设为 USART1。
 */
void bsp_linux_uart_on_tx_cplt_from_isr(void);

#endif /* AITAID_BSP_LINUX_UART_H */
