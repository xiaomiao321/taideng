/**
 * @file bsp_uart_stream.h
 * @brief UART 单字节中断接收加环形缓冲：把串口变成一个可轮询读取的字节流。
 *
 * 每个实例绑定一个 UART 句柄和一块调用者提供的环形缓冲。ISR 每收到一个字节
 * 就写入缓冲并重新启动单字节接收；主循环用 read() 取字节，用 update() 处理
 * 接收错误。本模块只负责接收，发送由持有实例的 BSP 模块自己实现。
 *
 * 并发约定：ISR 只写 head 和各错误标志，主循环只写 tail，单生产者单消费者，
 * 对齐的 32 位读写在 Cortex-M4 上是原子的，不需要关中断。
 *
 * @note 本模块不定义 HAL 回调；HAL_UART_RxCpltCallback() 和
 *       HAL_UART_ErrorCallback() 由集成层（hal_callback_dispatch.c）按实例
 *       转发给持有实例的 BSP 模块，再由它调用这里的 _from_isr 接口。
 * @note 同一个 UART 同一时刻只能被一个接收者使用；与 HAL 的 ReceiveToIdle
 *       等其他接收方式互斥由持有实例的模块负责。
 */

#ifndef AITAID_BSP_UART_STREAM_H
#define AITAID_BSP_UART_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_common.h"
#include "stm32f4xx_hal.h"

/**
 * @brief 字节流实例。字段由本模块维护，调用者不得直接读写。
 */
typedef struct
{
    UART_HandleTypeDef *huart; /**< 绑定的 UART 句柄。 */
    uint8_t *ring;             /**< 环形缓冲，容量为 mask + 1。 */
    uint32_t mask;             /**< 容量减一，容量必须是 2 的幂。 */
    volatile uint32_t head;    /**< 写入位置，只由 ISR 修改。 */
    volatile uint32_t tail;    /**< 读出位置，只由主循环修改。 */
    uint8_t rx_byte;           /**< HAL 单字节接收的目标。 */
    bool is_enabled;         /**< 调用者要求接收：start() 置位，stop() 清除。 */
    volatile bool is_active; /**< HAL 单字节接收当前是否已挂起。 */
    volatile bool has_error; /**< ISR 报告了接收错误，等待 update() 处理。 */
    volatile uint32_t overflow_count; /**< 缓冲满而丢弃的字节数，只增不减。 */
    uint32_t error_count;             /**< 已处理的接收错误次数，只增不减。 */
} bsp_uart_stream_t;

/**
 * @brief 绑定 UART 句柄和环形缓冲，不启动接收。
 *
 * @param[out] self 待初始化的实例，不允许为 NULL。
 * @param[in] huart UART 句柄，不允许为 NULL，必须已完成 MX_USARTx_UART_Init()。
 * @param[in] ring 环形缓冲，不允许为 NULL，生命周期不短于实例。
 * @param[in] size 缓冲容量，必须是 2 的幂且不小于 2。
 *
 * @retval BSP_OK 已初始化。
 * @retval BSP_ERR_PARAM 参数无效。
 * @retval BSP_ERR_CONFIG UART 句柄尚未初始化。
 */
bsp_status_t bsp_uart_stream_init(bsp_uart_stream_t *self,
                                  UART_HandleTypeDef *huart, uint8_t *ring,
                                  uint32_t size);

/**
 * @brief 清空缓冲和错误标志，启动单字节中断接收。
 *
 * @retval BSP_OK 已启动，或本来就在接收。
 * @retval BSP_BUSY UART 正被其他接收方式占用。
 * @retval BSP_ERR_HAL HAL 启动接收失败。
 */
bsp_status_t bsp_uart_stream_start(bsp_uart_stream_t *self);

/**
 * @brief 停止接收并清空缓冲；未启动时什么都不做。
 *
 * @retval BSP_OK 已停止。
 * @retval BSP_ERR_HAL HAL 终止接收失败；实例仍标记为停止。
 */
bsp_status_t bsp_uart_stream_stop(bsp_uart_stream_t *self);

/** @brief 是否处于 start() 之后、stop() 之前；出错恢复期间也算启动。 */
bool bsp_uart_stream_is_active(const bsp_uart_stream_t *self);

/**
 * @brief 取出一个已收到的字节。
 *
 * @param[out] byte 输出字节，不允许为 NULL。
 *
 * @retval true 已取出一个字节。
 * @retval false 缓冲为空。
 */
bool bsp_uart_stream_read(bsp_uart_stream_t *self, uint8_t *byte);

/** @brief 丢弃缓冲中所有尚未读取的字节。 */
void bsp_uart_stream_flush(bsp_uart_stream_t *self);

/**
 * @brief 主循环周期调用：处理 ISR 报告的接收错误。
 *
 * 有错误时终止并重新启动接收，error_count 加一；缓冲中已收到的字节保留，
 * 需要丢弃时由调用者调用 bsp_uart_stream_flush()。stop() 之后不做任何事。
 *
 * @retval BSP_OK 未启动、没有错误，或错误已恢复。
 * @retval BSP_ERR_HAL 重新启动接收失败；实例仍处于启动要求下，下次调用再试。
 */
bsp_status_t bsp_uart_stream_update(bsp_uart_stream_t *self);

/** @brief 已处理的接收错误次数；与上次读到的值比较即可发现新错误。 */
uint32_t bsp_uart_stream_error_count(const bsp_uart_stream_t *self);

/** @brief 缓冲满而丢弃的字节数；与上次读到的值比较即可发现新溢出。 */
uint32_t bsp_uart_stream_overflow_count(const bsp_uart_stream_t *self);

/**
 * @brief 单字节接收完成：存入缓冲并重新启动接收。
 * @warning 只能在中断上下文调用，且调用前已确认 UART 实例。
 */
void bsp_uart_stream_on_rx_cplt_from_isr(bsp_uart_stream_t *self);

/**
 * @brief 记录一次接收错误，由 update() 在主循环中恢复。
 * @warning 只能在中断上下文调用，且调用前已确认 UART 实例。
 */
void bsp_uart_stream_on_error_from_isr(bsp_uart_stream_t *self);

#endif /* AITAID_BSP_UART_STREAM_H */
