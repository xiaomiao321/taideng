/**
 * @file app_main.h
 * @brief 应用层入口：把 USART1 上的二进制协议接到命令层与实际硬件。
 *
 * 数据流：
 *   收到的字节 -> mcu_protocol_feed()  -> 解析出完整请求
 *              -> mcu_command_handle() -> 组出回复
 *              -> mcu_protocol_encode() -> 经 USART1 发出
 *
 * 协议层和命令层都不认识 HAL；真正碰灯和电机的动作由本文件里的 hw_* 回调
 * 绑定到 bsp_light / bsp_motor。这样换板子或换驱动时，只需要改这一处。
 */

#ifndef AITAID_APP_MAIN_H
#define AITAID_APP_MAIN_H

/**
 * @brief 应用层初始化。
 *
 * @note 调用前必须完成各 MX_TIMx_Init() 与 MX_USART1_UART_Init()。
 * @note 内部会初始化灯板与电机驱动、注册命令层回调并启动 USART1 接收。
 * @note 任一初始化失败时按项目约定点亮错误灯并进入 Error_Handler()。
 */
void app_init(void);

/**
 * @brief 应用层的一次周期性任务，由主循环反复调用。
 *
 * @note 不阻塞、不返回错误码；内部错误统一走错误灯加 Error_Handler()。
 * @note 单次调用最多处理一次接收结果和队列里所有已完成的请求。
 */
void app_run_once(void);

#endif /* AITAID_APP_MAIN_H */
