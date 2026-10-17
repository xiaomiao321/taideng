/**
 * @file bsp_param_flash.h
 * @brief 片内 Flash 参数存储：用最后两个 128KB 扇区保存一份定长参数，断电保留。
 *
 * 存储格式与掉电安全由 flash_journal 实现；本模块只负责选定扇区、擦除和编程。
 * 扇区 10（0x080C0000）与扇区 11（0x080E0000）在链接脚本中已从程序区划出。
 *
 * 耗时：
 * - 保存通常只编程一条记录（一百多字节），不到 1ms；
 * - 擦除一个扇区约 1～2 秒，期间 CPU 停顿、中断无法响应。初始化时会预先
 *   擦好备用扇区，保存时只有在一次运行内连续保存几百次、写满两个扇区后
 *   才会擦除；擦除时会临时放宽看门狗。
 *
 * @warning 调用初始化和保存前，执行器（电机等）必须已经停止。
 * @note 用调试器下载固件时只擦除程序所在扇区，参数会保留；
 *       整片擦除（mass erase）会清除参数。
 */

#ifndef AITAID_BSP_PARAM_FLASH_H
#define AITAID_BSP_PARAM_FLASH_H

#include <stdint.h>

#include "bsp_common.h"

/**
 * @brief 扫描参数扇区，找出最新记录，并预先擦好备用扇区。
 *
 * @param[in] payload_size 参数字节数，4 的倍数，1 至 1024。
 *
 * @retval BSP_OK 可以加载和保存（不论是否已有记录）。
 * @retval BSP_ERR_PARAM payload_size 无效。
 * @retval BSP_ERR_CONFIG 固件映像已延伸到参数扇区，拒绝擦写。
 * @retval BSP_ERR_HAL 擦除备用扇区失败；已有记录仍可加载，保存可能失败。
 *
 * @note 可能阻塞约 2 秒（需要擦除时）。应在看门狗启动前、初始化阶段调用。
 */
bsp_status_t bsp_param_flash_init(uint32_t payload_size);

/**
 * @brief 读出最新保存的参数。
 *
 * @retval BSP_OK 已读出。
 * @retval BSP_IDLE 还没有保存过参数。
 * @retval BSP_ERR_NOT_INITIALIZED 尚未成功初始化。
 */
bsp_status_t bsp_param_flash_load(void *payload);

/**
 * @brief 保存一份新参数。
 *
 * @retval BSP_OK 已写入并回读校验通过。
 * @retval BSP_ERR_NOT_INITIALIZED 尚未成功初始化。
 * @retval BSP_ERR_HAL 擦除或编程失败；之前保存的参数仍然有效。
 * @retval BSP_ERR_DATA 回读校验不符；之前保存的参数仍然有效。
 */
bsp_status_t bsp_param_flash_save(const void *payload);

#endif /* AITAID_BSP_PARAM_FLASH_H */
