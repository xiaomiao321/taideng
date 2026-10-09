/**
 * @file bsp_uart_loopback.h
 * @brief USART1 固定长度回环测试接口，暂不作为正式通信驱动。
 *
 * 测试前断开 Linux 板，在 USART1 接口短接 TX 和 RX。
 * 模块独占 huart1，使用中断接收和阻塞发送；日志由 main 通过 USART3 输出。
 * 除 _from_isr 接口外，所有接口由同一主循环串行调用，不从 ISR 调用。
 * 当前版本不支持多任务并发，不使用 DMA，不解析协议。
 */

#ifndef AITAID_BSP_UART_LOOPBACK_H
#define AITAID_BSP_UART_LOOPBACK_H

/** @brief 一次测试的固定字节数，不包含字符串末尾的空字符。 */
#define BSP_UART_LOOPBACK_LENGTH (8U)

/** @brief 阻塞发送的最大等待时间，单位为毫秒。 */
#define BSP_UART_LOOPBACK_TX_TIMEOUT_MS (20U)

/** @brief 从启动接收起计算的接收等待时间，单位为毫秒。 */
#define BSP_UART_LOOPBACK_RX_TIMEOUT_MS (100U)

/**
 * @brief 回环测试操作或结果状态。
 */
typedef enum
{
    BSP_UART_LOOPBACK_STATUS_OK = 0,
    BSP_UART_LOOPBACK_STATUS_NOT_INITIALIZED,
    BSP_UART_LOOPBACK_STATUS_IDLE,
    BSP_UART_LOOPBACK_STATUS_BUSY,
    BSP_UART_LOOPBACK_STATUS_TIMEOUT,
    BSP_UART_LOOPBACK_STATUS_UART_ERROR,
    BSP_UART_LOOPBACK_STATUS_DATA_MISMATCH
} bsp_uart_loopback_status_t;

/**
 * @brief 初始化回环测试的软件状态，初始结果为 IDLE。
 *
 * @note 调用前必须完成 MX_USART1_UART_Init()，并启用 USART1 全局中断。
 * @note 本函数不配置 UART，不启动收发，不输出日志。
 * @warning 只在启动阶段、UART 没有正在收发时调用，不用于中途复位测试。
 */
void bsp_uart_loopback_init(void);

/**
 * @brief 启动一次固定 8 字节的回环测试。
 *
 * @retval BSP_UART_LOOPBACK_STATUS_OK 已启动测试，不表示回环验证通过。
 * @retval BSP_UART_LOOPBACK_STATUS_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_UART_LOOPBACK_STATUS_BUSY 测试尚未结束或 UART 已被占用。
 * @retval BSP_UART_LOOPBACK_STATUS_TIMEOUT 阻塞发送超时。
 * @retval BSP_UART_LOOPBACK_STATUS_UART_ERROR 启动接收或发送失败，或此前清理失败。
 *
 * @note 固定发送内容由 .c 中的私有数组定义，接收缓冲区也由模块持有。
 *       两个数组均须在本次异步接收结束前有效，不使用函数栈上的接收数组。
 * @note 顺序为：检查状态、清除通知标志、记录开始时间及进行状态、
 *       HAL_UART_Receive_IT() 启动接收、HAL_UART_Transmit() 发送。
 * @note 中断接收可能在阻塞发送期间完成，不得在发送结束后清除完成标志。
 * @note 发送失败时必须终止本次已启动的接收并检查清理结果，避免遗留忙状态。
 *       接收启动失败时不得发送；若 UART 原本被占用，不得终止他人的收发。
 * @note 正在测试时重复调用不会重置缓冲区、通知标志或超时时间。
 * @note 清理失败后禁止再次启动；确认 UART 已恢复空闲后才能重新初始化模块。
 * @warning 发送最多阻塞 BSP_UART_LOOPBACK_TX_TIMEOUT_MS；必须保持中断开启。
 * @warning 成功启动后持续调用 bsp_uart_loopback_update() 处理结果和超时。
 */
bsp_uart_loopback_status_t bsp_uart_loopback_start(void);

/**
 * @brief 在主循环检查通知、比较数据并处理接收超时。
 *
 * @retval BSP_UART_LOOPBACK_STATUS_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_UART_LOOPBACK_STATUS_IDLE 初始化后尚未启动测试。
 * @retval BSP_UART_LOOPBACK_STATUS_BUSY 本次测试仍在等待接收。
 * @retval BSP_UART_LOOPBACK_STATUS_OK 已接收 8 字节且逐字节匹配。
 * @retval BSP_UART_LOOPBACK_STATUS_DATA_MISMATCH 接收完成，但内容不匹配。
 * @retval BSP_UART_LOOPBACK_STATUS_TIMEOUT 等待接收超时。
 * @retval BSP_UART_LOOPBACK_STATUS_UART_ERROR UART 错误或终止接收失败。
 *
 * @note 优先处理错误通知，再处理完成通知，最后判断超时。
 * @note 比较使用固定长度的字节比较，不使用 strcmp()，不依赖空字符结尾。
 * @note 用 HAL_GetTick() 的 uint32_t 无符号时间差判断接收超时。
 * @note 超时或 UART 错误时，在主循环调用 HAL_UART_AbortReceive() 清理本次
 *       接收并检查返回值；清理失败返回 UART_ERROR，不继续启动下一次测试。
 * @note 终态保留至下一次 start；重复 update 返回同一结果。main 应只打印一次。
 * @note 本函数不等待接收完成，不使用 HAL_Delay()，不输出日志。
 */
bsp_uart_loopback_status_t bsp_uart_loopback_update(void);

/**
 * @brief 从 USART1 接收完成回调记录完成通知。
 *
 * @note 由 HAL_UART_RxCpltCallback() 确认句柄为 huart1 后转发。
 * @note 只在测试进行中设置完成标志；未初始化或没有测试时忽略通知。
 * @warning 本函数在 ISR 中执行，只设置通知，不比较数据、不打印、不延时。
 */
void bsp_uart_loopback_on_rx_complete_from_isr(void);

/**
 * @brief 从 USART1 错误回调记录错误通知。
 *
 * @note 由 HAL_UART_ErrorCallback() 确认句柄为 huart1 后转发。
 * @note 只在测试进行中设置错误标志，具体清理由 update 在主循环中执行。
 * @warning 本函数在 ISR 中执行，不打印、不延时、不启动新的接收。
 */
void bsp_uart_loopback_on_error_from_isr(void);

#endif /* AITAID_BSP_UART_LOOPBACK_H */
