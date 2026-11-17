/**
 * @file joint_service.h
 * @brief 关节服务：电位器采样滤波、关节闭环推进与标定的 Flash 存取。
 *
 * 调试终端和 Android 协议两种模式共用：本模块持有唯一的 joint_control_t，
 * 把它的硬件回调接到 bsp_motor，每 POT_SAMPLE_PERIOD_MS 过采样一次六路电位器
 * （截尾平均后再做时间上的三点中值），然后推进全部运动中的关节。
 * 命令解析、状态上报和 STOP 语义由各模式自己处理。
 */

#ifndef AITAID_JOINT_SERVICE_H
#define AITAID_JOINT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "joint_control.h"

/**
 * @brief 初始化 ADC 与关节控制，并从 Flash 恢复标定和控制参数。
 *
 * @param[in] reply 接收 JRESULT 等文本行（含 CRLF），不允许为 NULL。
 *
 * @return 恢复了标定的关节数。
 *
 * @note 调用前必须已完成 bsp_motor_init()。
 * @note 需要擦除 Flash 备用扇区时阻塞约 2 秒，必须在看门狗启动之前调用。
 * @note ADC 初始化失败不报错，此后采样一律无效，关节运动会因传感器失效被拒绝。
 */
unsigned joint_service_init(void (*reply)(const char *line));

/** @brief 关节控制状态；调用者可读，也可调用 joint_control 的接口修改。 */
joint_control_t *joint_service_joints(void);

/**
 * @brief 主循环反复调用：到时间就采样一次，然后推进关节闭环。
 *
 * @retval false 本次有关节产生了新故障（该关节已停止并锁存故障位）。
 */
bool joint_service_update(void);

/** @brief 把全部关节的标定和控制参数写入 Flash；返回是否成功。 */
bool joint_service_save(void);

/**
 * @brief 把 Flash 里的启动计数加一并保存，作为本次启动的标识。
 *
 * @param[out] boot_id 返回 true 时写入新的计数，不允许为 NULL。
 *
 * @retval true 新计数已写入 Flash，下次启动一定得到不同的值。
 * @retval false Flash 不可用或保存失败，boot_id 不变。
 *
 * @note 每次启动最多调用一次，在 joint_service_init() 之后、看门狗启动之前、
 *       电机静止时调用：写满一个扇区（四百多次启动）后的那一次保存要擦除，
 *       阻塞约 2 秒。
 * @note 启动计数和标定存在同一条记录里，整片擦除会让计数从头开始。
 */
bool joint_service_next_boot_id(uint32_t *boot_id);

/** @brief 标定的 Flash 存储是否可用。 */
bool joint_service_store_ok(void);

/** @brief ADC 是否初始化成功。 */
bool joint_service_adc_ok(void);

/** @brief 最近一次采样是否有效。 */
bool joint_service_pot_valid(void);

/** @brief 最近一次有效采样的六路滤波读数。 */
const uint16_t *joint_service_pot(void);

#endif /* AITAID_JOINT_SERVICE_H */
