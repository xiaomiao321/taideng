/**
 * @file bsp_log.h
 * @brief 板载调试串口的原始输出、格式化输出和分级日志接口。
 */

#ifndef AITAID_BSP_LOG_H
#define AITAID_BSP_LOG_H

#include "bsp_common.h"

#include <stddef.h>
#include <stdint.h>

/** @brief 格式化缓冲区容量，包含末尾空字符，单位为字节。 */
#define BSP_LOG_BUFFER_SIZE (256U)

/* 使用宏定义等级，供预处理器 #if 判断。数值越大，输出越详细。 */
#define LOG_LEVEL_NONE  (0U)
#define LOG_LEVEL_ERROR (1U)
#define LOG_LEVEL_WARN  (2U)
#define LOG_LEVEL_INFO  (3U)
#define LOG_LEVEL_DEBUG (4U)

/* 构建系统可以通过 -DAITAID_LOG_LEVEL=4 等方式覆盖默认等级。 */
#ifndef AITAID_LOG_LEVEL
#define AITAID_LOG_LEVEL LOG_LEVEL_INFO
#endif

#if (AITAID_LOG_LEVEL < LOG_LEVEL_NONE) || \
    (AITAID_LOG_LEVEL > LOG_LEVEL_DEBUG)
#error "AITAID_LOG_LEVEL must be between 0 and 4"
#endif

/**
 * @brief 初始化 Bring-up 日志模块。
 *
 * @retval BSP_OK 初始化成功。
 * @retval BSP_ERR_CONFIG 调试串口句柄未绑定 USART3，或未处于可用状态。
 *
 * @note 调用本函数前必须已经执行 MX_USART3_UART_Init()。
 * @note 本模块固定使用 USART3，115200 bit/s、8N1、无硬件流控。
 */
bsp_status_t bsp_log_init(void);

/**
 * @brief 通过调试串口阻塞发送一段原始字节数据。
 *
 * @param[in] data 待发送数据的首地址，不允许为 NULL。
 * @param[in] length 待发送数据长度，单位为字节，必须大于 0。
 *
 * @retval BSP_OK 发送成功。
 * @retval BSP_ERR_NOT_INITIALIZED 日志模块尚未初始化。
 * @retval BSP_ERR_PARAM 参数无效。
 * @retval BSP_BUSY UART 当前忙。
 * @retval BSP_ERR_TIMEOUT UART 发送超时。
 * @retval BSP_ERR_HAL UART 发生其他发送错误。
 *
 * @warning 本函数为阻塞接口，不能从中断服务程序调用。
 */
bsp_status_t bsp_log_write(const uint8_t *data, size_t length);

/**
 * @brief 通过调试串口阻塞发送以空字符结尾的字符串。
 *
 * @param[in] message 待发送字符串，不允许为 NULL。
 *
 * @retval BSP_OK 发送成功。
 * @retval BSP_ERR_NOT_INITIALIZED 日志模块尚未初始化。
 * @retval BSP_ERR_PARAM 参数无效。
 * @retval BSP_BUSY UART 当前忙。
 * @retval BSP_ERR_TIMEOUT UART 发送超时。
 * @retval BSP_ERR_HAL UART 发生其他发送错误。
 *
 * @warning 本函数为阻塞接口，不能从中断服务程序调用。
 */
bsp_status_t bsp_log_write_string(const char *message);

/**
 * @brief 将 printf 风格的消息格式化后，通过调试串口阻塞发送。
 *
 * @param[in] format 格式字符串，不允许为 NULL 或空字符串。
 * @param[in] ... 与格式字符串匹配的可变参数。
 *
 * @retval BSP_OK 发送成功，或格式化结果为空且无须发送。
 * @retval BSP_ERR_NOT_INITIALIZED 日志模块尚未初始化。
 * @retval BSP_ERR_PARAM 格式字符串无效，或格式化函数返回错误。
 * @retval BSP_ERR_BUFFER_TOO_SMALL 格式化结果超长，整条消息不发送。
 * @retval BSP_BUSY UART 当前忙。
 * @retval BSP_ERR_TIMEOUT UART 发送超时。
 * @retval BSP_ERR_HAL UART 发生其他发送错误。
 *
 * @note 使用 256 字节缓冲区，最多发送 255 字节，不发送末尾的空字符。
 * @note 本函数不自动添加换行，调用者按需在格式字符串中添加换行。
 * @warning 本函数为阻塞接口，不能从中断服务程序调用。
 * @warning 当前版本不保证多任务并发调用安全。
 */
bsp_status_t bsp_log_printf(const char *format, ...);

/**
 * @brief 输出带时间戳、等级和模块标签的单行格式化日志。
 *
 * @param[in] level 日志等级，仅接受 LOG_LEVEL_ERROR 至 LOG_LEVEL_DEBUG。
 * @param[in] tag 模块标签，例如 "BOOT"，不允许为 NULL 或空字符串。
 * @param[in] format printf 风格的格式字符串，不允许为 NULL 或空字符串。
 * @param[in] ... 与格式字符串匹配的可变参数。
 *
 * @retval BSP_OK 发送成功，或日志被等级过滤而未发送。
 * @retval BSP_ERR_NOT_INITIALIZED 日志模块尚未初始化。
 * @retval BSP_ERR_PARAM 等级、标签或格式字符串无效，或格式化失败。
 * @retval BSP_ERR_BUFFER_TOO_SMALL 完整日志超长，整条消息不发送。
 * @retval BSP_BUSY UART 当前忙。
 * @retval BSP_ERR_TIMEOUT UART 发送超时。
 * @retval BSP_ERR_HAL UART 发生其他发送错误。
 *
 * @note 先检查初始化和参数，再过滤等级；level > AITAID_LOG_LEVEL 时
 *       返回成功，不进行格式化或发送。
 * @note 输出格式为 "[1250][INFO][BOOT] system started\r\n"，
 *       时间戳使用 HAL_GetTick() 的毫秒值，并随其计数器回绕。
 * @note 自动添加 CRLF 换行，调用者应避免在标签或正文中包含换行。
 * @note 前缀、正文和 CRLF 共用 BSP_LOG_BUFFER_SIZE 字节缓冲区，
 *       必须为末尾空字符预留空间；发送时不包含该空字符。
 * @note 正文格式化结果为空时，仍输出前缀和 CRLF。
 * @note 等级过滤仅作用于分级日志；bsp_log_write()、
 *       bsp_log_write_string() 和 bsp_log_printf() 本身不受等级限制。
 * @warning 本函数为阻塞接口，不能从中断服务程序调用。
 * @warning 当前版本不保证多任务并发调用安全。
 */
bsp_status_t bsp_log_print(uint32_t level,
                           const char *tag,
                           const char *format, ...);

/*
 * 应用层使用 LOG_*(tag, format, ...) 宏。
 * 启用的宏忽略发送返回值；需要检查结果时，直接调用 bsp_log_print()。
 * 关闭的宏不求值任何参数，日志参数中不得包含业务必需的副作用。
 */
#if AITAID_LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_ERROR(...) \
    do \
    { \
        (void)bsp_log_print(LOG_LEVEL_ERROR, __VA_ARGS__); \
    } while (0)
#else
#define LOG_ERROR(...) ((void)0)
#endif

#if AITAID_LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_WARN(...) \
    do \
    { \
        (void)bsp_log_print(LOG_LEVEL_WARN, __VA_ARGS__); \
    } while (0)
#else
#define LOG_WARN(...) ((void)0)
#endif

#if AITAID_LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_INFO(...) \
    do \
    { \
        (void)bsp_log_print(LOG_LEVEL_INFO, __VA_ARGS__); \
    } while (0)
#else
#define LOG_INFO(...) ((void)0)
#endif

#if AITAID_LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DEBUG(...) \
    do \
    { \
        (void)bsp_log_print(LOG_LEVEL_DEBUG, __VA_ARGS__); \
    } while (0)
#else
#define LOG_DEBUG(...) ((void)0)
#endif

#endif /* AITAID_BSP_LOG_H */
