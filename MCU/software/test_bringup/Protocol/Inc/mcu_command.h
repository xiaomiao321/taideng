/**
 * @file mcu_command.h
 * @brief 协议命令层：把解析出来的请求变成回复，并维护设备级状态。
 *
 * 本模块不认识 HAL，也不直接操作灯或电机。需要硬件动作时，通过初始化时注册的
 * mcu_command_hw_t 回调交给上层（main.c 里绑定到 bsp_light / bsp_motor）。这样
 * 命令逻辑可以在电脑上用假硬件完整单测，不需要板子和串口。
 *
 * 当前实现范围：
 *   - GET_STATUS        -> STATUS，回读实际灯光、运动和故障状态；
 *   - SET_LIGHT         -> 七路灯板一起开关与调亮度，回成功 ACK；
 *   - SET_MOTION STOP   -> 安全停止 + 成功 ACK；
 *   - PING              -> 成功 ACK；
 *   - 其余命令（前进/后退/左右转、扩展命令）-> 带"未知命令"的 ACK，不改输出。
 */

#ifndef AITAID_MCU_COMMAND_H
#define AITAID_MCU_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "mcu_protocol.h"

/**
 * @brief 命令层需要的硬件动作，由上层实现并在初始化时注册。
 *
 * @note 三个函数指针都必须有效，且都由主循环串行调用，不需要考虑重入。
 */
typedef struct
{
    /**
     * @brief 读取当前实际状态，填好 LED 与运动的显示字段。
     *
     * @param[out] status 待填充的状态；调用前已清零。
     *
     * @note 只需填 light_enabled、brightness、motion、target_motion、speed、
     *       target_speed、fault_flags。运行秒数、协议错误数和保留字节由命令层
     *       自己填，回调不要管。
     */
    void (*read_status)(mcu_status_t *status);

    /**
     * @brief 应用灯光总开关和亮度。
     *
     * @param[in] light_flags bit0 为 1 表示开灯，其他位已由命令层校验为 0。
     * @param[in] brightness 亮度 0～100。
     *
     * @retval true 已应用。
     * @retval false 硬件失败，命令层会回一个硬件故障 ACK。
     *
     * @note 关灯时保留 brightness，下一次开灯继续使用。
     * @note apply_light 必须真正作用到硬件；如果只改了几个灯就失败，应当把
     *       全部灯恢复为关闭状态再返回 false，避免出现"只亮一半"的中间态。
     */
    bool (*apply_light)(uint8_t light_flags, uint8_t brightness);

    /**
     * @brief 应用一个宏观运动动作。
     *
     * @param[in] motion 动作码，见 MCU_PROTOCOL_MOTION_*。
     * @param[in] speed 速度 0～100。
     *
     * @retval true 已应用。
     * @retval false 硬件拒绝，命令层会回一个硬件故障 ACK。
     */
    bool (*apply_motion)(uint8_t motion, uint8_t speed);

    /** @brief 失联时的安全停止：立即停止全部运动输出。 */
    void (*safe_stop)(void);
} mcu_command_hw_t;

/**
 * @brief 初始化命令层并注册硬件回调。
 *
 * @param[in] hw 硬件回调表，不允许为 NULL，三个函数指针也必须全部有效。
 * @param[in] now_ms 当前时刻，作为运行秒数的起点和失联计时的基准。
 *
 * @note 本函数只清状态，不主动驱动任何硬件；上电安全状态由各 BSP 的 init 保证。
 */
void mcu_command_init(const mcu_command_hw_t *hw, uint32_t now_ms);

/**
 * @brief 处理一条已解析的请求报文。
 *
 * @param[in] request 已解析的请求，不允许为 NULL。
 * @param[out] reply 需要回复时填好的报文，不允许为 NULL。
 * @param[in] now_ms 当前时刻，用于刷新失联计时。
 *
 * @retval true 需要把 reply 编码后发给主机。
 * @retval false 本条报文按协议不需要回复，reply 内容未定义。
 *
 * @note 收到任何合法请求都会刷新失联计时，重复帧也算。
 * @note 只有会改变硬件状态的命令（SET_LIGHT、SET_MOTION）参与去重：与上一条
 *       控制帧的 SEQ、CMD、VERSION、FLAGS 和 PAYLOAD 全部相同才算重发，
 *       重发帧不会重复执行，但会重发上一次的 ACK。
 * @note 查询类命令（PING、GET_STATUS）不去重，每次查询都返回当前状态，
 *       否则重复查询会一直读到缓存的旧状态、运行秒数看起来不涨。
 */
bool mcu_command_handle(const mcu_frame_t *request, mcu_frame_t *reply,
                        uint32_t now_ms);

/**
 * @brief 周期调用：检查与 Android 是否失联。
 *
 * @param[in] now_ms 当前时刻。
 *
 * @retval true 本次调用刚刚触发了安全停止。
 * @retval false 链路正常，或已经停过一次、不需要重复停。
 *
 * @note 连续 MCU_COMMAND_LINK_TIMEOUT_MS 没有收到合法请求时调用 safe_stop()，
 *       灯光保持最近状态；一旦重新收到合法请求，计时重新开始。
 */
bool mcu_command_poll(uint32_t now_ms);

#endif /* AITAID_MCU_COMMAND_H */
