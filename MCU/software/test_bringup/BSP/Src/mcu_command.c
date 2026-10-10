/**
 * @file mcu_command.c
 * @brief 协议命令层实现：请求分派、状态回读、去重与失联安全停止。
 *
 * 本文件不包含任何 HAL 或 BSP 头，所有硬件动作都通过 mcu_command_hw_t 回调
 * 交出去，因此可以在电脑上配合假硬件完整测试。
 */

#include "mcu_command.h"

/* -------------------------------------------------------------------------- */
/* 内部状态                                                                    */
/* -------------------------------------------------------------------------- */

/** @brief 上层注册的硬件回调表。 */
static const mcu_command_hw_t *s_hw = NULL;
/** @brief 初始化时刻，用于计算运行秒数。 */
static uint32_t s_start_ms = 0U;
/** @brief 最近一次收到合法请求的时刻。 */
static uint32_t s_last_rx_ms = 0U;
/** @brief 是否已经收到过至少一条合法请求。 */
static bool s_has_link = false;
/** @brief 本次失联是否已经执行过安全停止，避免重复停。 */
static bool s_has_stopped = true;
/** @brief 是否记有上一次请求，用于 (SEQ, CMD) 去重。 */
static bool s_has_last = false;
/** @brief 上一次请求的序号与命令字。 */
static uint8_t s_last_seq = 0U;
static uint8_t s_last_cmd = 0U;
/** @brief 上一次请求对应的回复，重复请求时原样重发。 */
static mcu_frame_t s_last_reply;

/* -------------------------------------------------------------------------- */
/* 内部工具                                                                    */
/* -------------------------------------------------------------------------- */

/** @brief 判断宏运动动作码是否在协议允许的范围内。 */
static bool is_valid_motion(uint8_t motion)
{
    return (motion == MCU_PROTOCOL_MOTION_STOP) ||
           (motion == MCU_PROTOCOL_MOTION_FORWARD) ||
           (motion == MCU_PROTOCOL_MOTION_BACKWARD) ||
           (motion == MCU_PROTOCOL_MOTION_TURN_LEFT) ||
           (motion == MCU_PROTOCOL_MOTION_TURN_RIGHT);
}

/**
 * @brief 该命令是否会改变硬件状态。
 *
 * @note 只有这类命令才需要按 (SEQ, CMD) 去重：协议要求重复帧不能重复产生
 *       非幂等副作用。查询类命令（PING、GET_STATUS）没有副作用，必须每次都
 *       重新处理，否则重复查询会一直返回缓存的旧状态、运行秒数看起来不涨。
 */
static bool has_side_effect(uint8_t cmd)
{
    return (cmd == MCU_PROTOCOL_CMD_SET_LIGHT) ||
           (cmd == MCU_PROTOCOL_CMD_SET_MOTION);
}

/** @brief 组装一条 STATUS 回复。 */
static void build_status(mcu_frame_t *reply, uint8_t seq, uint32_t now_ms)
{
    mcu_status_t status = {0};

    s_hw->read_status(&status);

    /* 协议规定这些字段由 MCU 自己维护，不看回调填了什么。 */
    status.status_flags = 0U;
    status.uptime_seconds = (now_ms - s_start_ms) / 1000U;
    status.protocol_errors = mcu_protocol_error_count();

    mcu_protocol_make_status(reply, seq, &status);
}

/** @brief 组装一条错误 ACK。 */
static void build_error_ack(mcu_frame_t *reply, const mcu_frame_t *request,
                            uint8_t result)
{
    mcu_protocol_make_ack(reply, request->seq, request->cmd, result, 0U);
}

/** @brief 记录一次链路活动，刷新失联计时。 */
static void note_link_activity(uint32_t now_ms)
{
    s_last_rx_ms = now_ms;
    s_has_link = true;
    s_has_stopped = false;
}

/** @brief 刷新链路计时；对会改变状态的命令，额外记下回复用于重发。 */
static void remember_request(const mcu_frame_t *request,
                             const mcu_frame_t *reply, uint32_t now_ms)
{
    note_link_activity(now_ms);

    if (!has_side_effect(request->cmd))
    {
        return;
    }

    s_last_seq = request->seq;
    s_last_cmd = request->cmd;
    s_last_reply = *reply;
    s_has_last = true;
}

/* -------------------------------------------------------------------------- */
/* 对外接口                                                                    */
/* -------------------------------------------------------------------------- */

void mcu_command_init(const mcu_command_hw_t *hw, uint32_t now_ms)
{
    if ((hw == NULL) || (hw->read_status == NULL) ||
        (hw->apply_light == NULL) || (hw->apply_motion == NULL) ||
        (hw->safe_stop == NULL))
    {
        return;
    }

    s_hw = hw;
    s_start_ms = now_ms;
    s_last_rx_ms = now_ms;
    s_has_link = false;
    s_has_stopped = true;
    s_has_last = false;
    s_last_seq = 0U;
    s_last_cmd = 0U;
}

bool mcu_command_handle(const mcu_frame_t *request, mcu_frame_t *reply,
                        uint32_t now_ms)
{
    if ((s_hw == NULL) || (request == NULL) || (reply == NULL))
    {
        return false;
    }

    /* 带 RESPONSE 位的是回复，主机不该发给我们；忽略以免互相回环。 */
    if ((request->flags & MCU_PROTOCOL_FLAG_RESPONSE) != 0U)
    {
        return false;
    }

    /* 重复的控制帧：只重发上次的回复，绝不重复执行副作用。
     * 注意这里也要刷新链路计时，否则主机一直重发同一条帧会被误判成失联。 */
    if (s_has_last && has_side_effect(request->cmd) &&
        (request->seq == s_last_seq) && (request->cmd == s_last_cmd))
    {
        note_link_activity(now_ms);
        *reply = s_last_reply;
        return true;
    }

    /* 版本不符：整帧不可信，不碰硬件，只回一个错误 ACK。 */
    if (request->version != MCU_PROTOCOL_VERSION)
    {
        build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_COMMAND);
        remember_request(request, reply, now_ms);
        return true;
    }

    switch (request->cmd)
    {
        case MCU_PROTOCOL_CMD_PING:
            /* Payload 预留，当前不校验内容。 */
            mcu_protocol_make_ack(reply, request->seq, request->cmd,
                                  MCU_PROTOCOL_RESULT_OK, 0U);
            break;

        case MCU_PROTOCOL_CMD_GET_STATUS:
            if (request->payload_length != 0U)
            {
                build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_LENGTH);
                break;
            }
            build_status(reply, request->seq, now_ms);
            break;

        case MCU_PROTOCOL_CMD_SET_LIGHT:
        {
            if (request->payload_length != 4U)
            {
                build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_LENGTH);
                break;
            }

            uint8_t light_flags = request->payload[0];
            uint8_t brightness = request->payload[1];
            uint8_t reserved_flags = request->payload[2];
            uint8_t reserved = request->payload[3];

            if (((light_flags & 0xFEU) != 0U) || (brightness > 100U) ||
                (reserved_flags != 0U) || (reserved != 0U))
            {
                build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_VALUE);
                break;
            }

            if (!s_hw->apply_light(light_flags, brightness))
            {
                build_error_ack(reply, request, MCU_PROTOCOL_RESULT_HARDWARE);
                break;
            }

            mcu_protocol_make_ack(reply, request->seq, request->cmd,
                                  MCU_PROTOCOL_RESULT_OK, 0U);
            break;
        }

        case MCU_PROTOCOL_CMD_SET_MOTION:
        {
            if (request->payload_length != 4U)
            {
                build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_LENGTH);
                break;
            }

            uint8_t motion = request->payload[0];
            uint8_t speed = request->payload[1];
            uint8_t control_flags = request->payload[2];
            uint8_t reserved = request->payload[3];

            if (!is_valid_motion(motion) || (speed > 100U) ||
                (control_flags != 0U) || (reserved != 0U) ||
                ((motion == MCU_PROTOCOL_MOTION_STOP) && (speed != 0U)))
            {
                build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_VALUE);
                break;
            }

            if (motion != MCU_PROTOCOL_MOTION_STOP)
            {
                /* 底盘动作映射还没定，先明确拒绝，不假装接受。 */
                build_error_ack(reply, request,
                                MCU_PROTOCOL_RESULT_INVALID_COMMAND);
                break;
            }

            if (!s_hw->apply_motion(motion, speed))
            {
                build_error_ack(reply, request, MCU_PROTOCOL_RESULT_HARDWARE);
                break;
            }

            mcu_protocol_make_ack(reply, request->seq, request->cmd,
                                  MCU_PROTOCOL_RESULT_OK, 0U);
            break;
        }

        default:
            /* 尚未实现的命令：要求 ACK 才回，否则静默丢弃。 */
            if ((request->flags & MCU_PROTOCOL_FLAG_ACK_REQUIRED) == 0U)
            {
                return false;
            }
            build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_COMMAND);
            break;
    }

    remember_request(request, reply, now_ms);
    return true;
}

bool mcu_command_poll(uint32_t now_ms)
{
    if ((s_hw == NULL) || (!s_has_link) || (s_has_stopped))
    {
        return false;
    }

    if ((now_ms - s_last_rx_ms) < MCU_COMMAND_LINK_TIMEOUT_MS)
    {
        return false;
    }

    s_hw->safe_stop();
    s_has_stopped = true;
    return true;
}
