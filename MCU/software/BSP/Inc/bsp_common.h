/**
 * @file bsp_common.h
 * @brief 所有 BSP 模块共用的返回状态定义。
 *
 * 统一 BSP 模块的初始化、读写、查询接口都返回 bsp_status_t，使调用者能用
 * 同一套判断处理所有模块，并便于 bring-up 阶段把“配置错误”“硬件无响应”
 * “通信错误”“数据异常”等失败类型区分开。
 *
 * 本枚举只表达 BSP 层的通用结果，不承载具体业务语义；业务错误由上层自行定义。
 */

#ifndef AITAID_BSP_COMMON_H
#define AITAID_BSP_COMMON_H

/**
 * @brief BSP 操作的返回状态。
 *
 * @note 约定：BSP_OK 为成功；BSP_IDLE、BSP_BUSY 表示非错误的过程状态；
 *       带 ERR_ 前缀的为错误，调用者应据此选择重试、报错或返回错误。
 */
typedef enum
{
    BSP_OK = 0,              /**< 操作成功完成。 */
    BSP_IDLE,                /**< 尚未启动该操作，当前无结果。 */
    BSP_BUSY,                /**< 上一次操作仍在进行中。 */
    BSP_ERR_NOT_INITIALIZED, /**< 模块尚未初始化，调用顺序不正确。 */
    BSP_ERR_PARAM,           /**< 参数为空指针、越界或格式无效。 */
    BSP_ERR_CONFIG,          /**< 句柄或外设配置与驱动的预期不符。 */
    BSP_ERR_TIMEOUT,         /**< 等待硬件或数据超时。 */
    BSP_ERR_HAL,             /**< 底层 HAL/SDK 返回错误。 */
    BSP_ERR_DATA,            /**< 操作已完成，但数据内容异常或不匹配。 */
    BSP_ERR_BUFFER_TOO_SMALL /**< 目标缓冲区不足，整条数据未写入或未发送。 */
} bsp_status_t;

/**
 * @brief 返回状态码的固定短名称，用于日志和 bring-up 输出。
 *
 * @param[in] status 待转换的状态码。
 *
 * @return 指向静态字符串常量的指针，调用者不得修改，也不需要释放。
 *         传入未定义的状态码时返回 "UNKNOWN"。
 */
const char *bsp_status_str(bsp_status_t status);

#endif /* AITAID_BSP_COMMON_H */
