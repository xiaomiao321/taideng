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
 *   - SET_MOTION        -> STOP 安全停止，其余动作交给底盘，回 ACK；
 *   - PING              -> 成功 ACK；
 *   - SET_ARM_POSITION、HOME_ARM -> 校验后交给 arm_move / arm_home；
 *   - SET_LIGHT_ZONE、SET_BUZZER、SET_INDICATOR -> 校验后交给对应回调；
 *   - GET_EXTENDED_STATUS（关节类和上面三种输出类）-> EXTENDED_STATUS；
 *   - 其余命令（SET_LIGHT_MODE、SOFT_SHUTDOWN），以及没有注册对应回调的
 *     扩展命令 -> 带"未知命令"的 ACK，不改输出。
 *
 * 全局 fault_flags 非 0 时拒绝 SET_ARM_POSITION、HOME_ARM 和非 STOP 的
 * SET_MOTION（硬件故障 ACK，detail 为故障位），STOP、查询和灯光、蜂鸣器、
 * 指示灯不受限制。
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
 * @note 前四个函数指针必须有效，其余扩展回调可为 NULL（表示不支持），
 *       都由主循环串行调用，不需要考虑重入。
 * @note 扩展回调收到的分区、图案、指示灯 ID 是原样转发的，由固件自己判断是否
 *       支持，不支持时返回 MCU_PROTOCOL_RESULT_INVALID_VALUE。
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

    /**
     * @brief 启动一个关节的定位（SET_ARM_POSITION）。
     *
     * @param[in] joint_id 关节 ID 1～6，已校验。
     * @param[in] target 目标位置，毫度。
     * @param[in] speed 本次运动的速度上限，毫度/秒，已校验为正数。
     *
     * @return 结果码 MCU_PROTOCOL_RESULT_*，原样放进 ACK。
     */
    uint8_t (*arm_move)(uint8_t joint_id, int32_t target, int32_t speed);

    /** @brief 关节回原点（HOME_ARM）；参数和返回值同 arm_move。 */
    uint8_t (*arm_home)(uint8_t joint_id);

    /**
     * @brief 读取一个关节的扩展状态，供 GET_EXTENDED_STATUS 回读。
     *
     * @param[in] command 被查询的控制命令，SET_ARM_POSITION 或 HOME_ARM。
     * @param[in] joint_id 关节 ID 1～6，已校验。
     * @param[out] status 填 faults、actual、target、actual_speed、target_speed；
     *             command 和 selector 由命令层填。
     *
     * @return MCU_PROTOCOL_RESULT_OK 时回 EXTENDED_STATUS，否则回该结果码的 ACK。
     */
    uint8_t (*arm_status)(uint8_t command, uint8_t joint_id,
                          mcu_extended_status_t *status);

    /**
     * @brief 设置一个灯光分区（SET_LIGHT_ZONE）。
     *
     * @param[in] zone_id 分区 ID，未校验。
     * @param[in] is_on 开关。
     * @param[in] brightness 亮度 0～100，已校验；关灯时也要记住，供回读。
     *
     * @return 结果码 MCU_PROTOCOL_RESULT_*，原样放进 ACK。
     */
    uint8_t (*light_zone)(uint8_t zone_id, bool is_on, uint8_t brightness);

    /**
     * @brief 设置蜂鸣器（SET_BUZZER）。
     *
     * @param[in] pattern_id 图案 ID，未校验。
     * @param[in] volume 音量 0～100，已校验。
     *
     * @return 结果码 MCU_PROTOCOL_RESULT_*，原样放进 ACK。
     */
    uint8_t (*buzzer)(uint8_t pattern_id, uint8_t volume);

    /**
     * @brief 设置一个指示灯（SET_INDICATOR）。
     *
     * @param[in] indicator_id 指示灯 ID，未校验。
     * @param[in] is_on 开关。
     *
     * @return 结果码 MCU_PROTOCOL_RESULT_*，原样放进 ACK。
     */
    uint8_t (*indicator)(uint8_t indicator_id, bool is_on);

    /**
     * @brief 读取分区灯、蜂鸣器或指示灯的扩展状态，供 GET_EXTENDED_STATUS 回读。
     *
     * @param[in] command 被查询的控制命令，SET_LIGHT_ZONE、SET_BUZZER 或
     *            SET_INDICATOR。
     * @param[in] selector 分区、图案或指示灯 ID，未校验。
     * @param[out] status 字段含义见 mcu_extended_status_t；command 和 selector
     *             由命令层填。
     *
     * @return MCU_PROTOCOL_RESULT_OK 时回 EXTENDED_STATUS，否则回该结果码的 ACK。
     */
    uint8_t (*output_status)(uint8_t command, uint8_t selector,
                             mcu_extended_status_t *status);
} mcu_command_hw_t;

/**
 * @brief 初始化命令层并注册硬件回调。
 *
 * @param[in] hw 硬件回调表，不允许为 NULL，前四个函数指针必须有效。
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
 * @note 只有会改变硬件状态的命令（SET_LIGHT、SET_MOTION、SET_ARM_POSITION、
 *       HOME_ARM、SET_LIGHT_ZONE、SET_BUZZER、SET_INDICATOR）参与去重：与上一条
 *       控制帧的 SEQ、CMD、VERSION、FLAGS 和 PAYLOAD 全部相同才算重发，
 *       重发帧不会重复执行，但会重发上一次的 ACK。
 * @note 查询类命令（PING、GET_STATUS、GET_EXTENDED_STATUS）不去重，每次查询都返回当前状态，
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
