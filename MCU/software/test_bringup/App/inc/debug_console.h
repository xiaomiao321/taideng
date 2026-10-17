/**
 * @file debug_console.h
 * @brief 调试终端入口（APP_MODE_DEBUG）：USART3 文本命令、状态上报与全板测试。
 */

#ifndef AITAID_DEBUG_CONSOLE_H
#define AITAID_DEBUG_CONSOLE_H

/**
 * @brief 初始化各 BSP 模块与调试状态机，开始接收 USART3 命令。
 *
 * @note 调用前必须完成 MX_*_Init() 和 bsp_log_init()。
 * @note 电机、按键或 USART3 初始化失败时进入 Error_Handler()；
 *       灯板、蜂鸣器、Linux 电源与 ADC 失败时只禁用相应命令。
 * @note 会从 Flash 恢复上次保存的关节标定，需要擦除备用扇区时阻塞约 2 秒，
 *       因此必须在看门狗启动之前调用。
 */
void debug_console_init(void);

/** @brief 主循环反复调用：处理命令、推进控制、周期上报状态。 */
void debug_console_update(void);

#endif /* AITAID_DEBUG_CONSOLE_H */
