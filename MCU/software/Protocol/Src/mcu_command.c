/**
 * @file mcu_command.c
 * @brief 协议命令层实现：请求分派、状态回读、去重与失联安全停止。
 *
 * 本文件不包含任何 HAL 或 BSP 头，所有硬件动作都通过 mcu_command_hw_t 回调
 * 交出去，因此可以在电脑上配合假硬件完整测试。
 */

#include "mcu_command.h"

#include <string.h>

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
/** @brief STATUS.status_flags 里当前挂着、等主机读走的扩展位。 */
static uint8_t s_status_flags = 0U;
/** @brief 底盘是否正按 SET_MOTION 运动，用来选失联时限。 */
static bool s_is_chassis_active = false;
/** @brief 是否下发过关节动作、且还没到它自己的期限。 */
static bool s_is_arm_active = false;
/** @brief 关节动作的固件内部期限，见 MCU_ARM_MOVE_TIMEOUT_MS。 */
static uint32_t s_arm_deadline_ms = 0U;

/** @brief 重发识别环的深度：足够覆盖“丢 ACK 后继续发新指令再回头重发”。 */
#define MCU_COMMAND_DEDUP_SLOTS (4U)

/**
 * @brief 重发识别环里的一项：控制帧摘要 + 当时的回复。
 *
 * @note 只留摘要（SEQ、CMD、VERSION、FLAGS、LENGTH、PAYLOAD）而不留整帧是为了
 *       省 RAM；回复必须整帧保留，重发时要逐字节原样回给主机。一项约 80 字节，
 *       四项约 320 字节。
 */
typedef struct
{
    bool is_valid;                             /**< 该项是否已经写过。 */
    uint8_t seq;                               /**< 请求的 SEQ。 */
    uint8_t cmd;                               /**< 请求的 CMD。 */
    uint8_t version;                           /**< 请求的协议版本。 */
    uint8_t flags;                             /**< 请求的 FLAGS。 */
    uint8_t payload_length;                    /**< 请求的 PAYLOAD 长度。 */
    uint8_t payload[MCU_PROTOCOL_MAX_PAYLOAD]; /**< 请求的 PAYLOAD 原文。 */
    mcu_frame_t reply;                         /**< 当时的回复，重发时原样返回。 */
} dedup_entry_t;

/** @brief 最近若干条带副作用控制帧，环形使用，最旧的被覆盖。 */
static dedup_entry_t s_dedup[MCU_COMMAND_DEDUP_SLOTS];
/** @brief 下一条写入的下标。 */
static uint8_t s_dedup_next = 0U;
/** @brief 已经写过的项数；未写满环时只扫这么多项。 */
static uint8_t s_dedup_used = 0U;

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
 * @note 只有这类命令才需要去重：协议要求重复帧不能重复产生
 *       非幂等副作用。查询类命令（PING、GET_STATUS）没有副作用，必须每次都
 *       重新处理，否则重复查询会一直返回缓存的旧状态、运行秒数看起来不涨。
 */
static bool has_side_effect(uint8_t cmd)
{
    return (cmd == MCU_PROTOCOL_CMD_SET_LIGHT) ||
           (cmd == MCU_PROTOCOL_CMD_SET_MOTION) ||
           (cmd == MCU_PROTOCOL_CMD_SET_ARM_POSITION) ||
           (cmd == MCU_PROTOCOL_CMD_HOME_ARM) ||
           (cmd == MCU_PROTOCOL_CMD_SET_LIGHT_ZONE) ||
           (cmd == MCU_PROTOCOL_CMD_SET_BUZZER) ||
           (cmd == MCU_PROTOCOL_CMD_SET_INDICATOR);
}

/** @brief 是否为分区灯、蜂鸣器、指示灯这三种输出类扩展命令。 */
static bool is_output_command(uint8_t cmd)
{
    return (cmd == MCU_PROTOCOL_CMD_SET_LIGHT_ZONE) ||
           (cmd == MCU_PROTOCOL_CMD_SET_BUZZER) ||
           (cmd == MCU_PROTOCOL_CMD_SET_INDICATOR);
}

/** @brief 关节 ID 是否在 extension-v2 的 1～6 内。 */
static bool is_valid_joint(uint8_t joint_id)
{
    return (joint_id >= MCU_PROTOCOL_JOINT_ID_MIN) &&
           (joint_id <= MCU_PROTOCOL_JOINT_ID_MAX);
}

/** @brief 按小端读一个 i32。 */
static int32_t get_i32(const uint8_t *in)
{
    return (int32_t)((uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                     ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24));
}

/** @brief 当前全局故障位；关节动作和非 STOP 底盘动作在非 0 时被拒绝。 */
static uint8_t current_faults(void)
{
    mcu_status_t status = {0};
    s_hw->read_status(&status);
    return status.fault_flags;
}

/**
 * @brief 环项与请求是否完全一致，即是否为同一条控制帧的原样重发。
 *
 * @note 只比 (SEQ, CMD) 不够：主机重启后 SEQ 从头开始，新命令可能恰好和
 *       MCU 记住的上一条撞上 SEQ 与 CMD，被误当成重发而不执行。原样重发的
 *       报文逐字节相同，所以把版本、标志和 PAYLOAD 一起比较。
 */
static bool dedup_matches(const dedup_entry_t *entry, const mcu_frame_t *request)
{
    return entry->is_valid && (request->seq == entry->seq) &&
           (request->cmd == entry->cmd) &&
           (request->version == entry->version) &&
           (request->flags == entry->flags) &&
           (request->payload_length == entry->payload_length) &&
           (memcmp(request->payload, entry->payload, request->payload_length) == 0);
}

/**
 * @brief 在环里找原样重发；命中时把当时的回复原样写进 reply。
 *
 * @retval true 是重发，reply 已填好，不应再执行一次副作用。
 * @retval false 不是重发。
 *
 * @note 用环而不是只记上一条（问题清单 A5）：主机丢了一个 ACK 后继续发新指令，
 *       回头再重发旧指令时，单槽实现已经把它挤掉了，于是 X→Y→X 会把第二个 X
 *       重新执行一遍。
 */
static bool dedup_lookup(const mcu_frame_t *request, mcu_frame_t *reply)
{
    for (uint8_t i = 0U; i < s_dedup_used; ++i)
    {
        if (dedup_matches(&s_dedup[i], request))
        {
            *reply = s_dedup[i].reply;
            return true;
        }
    }
    return false;
}

/** @brief 记一条带副作用控制帧及其回复，写满后覆盖最旧的一项。 */
static void dedup_store(const mcu_frame_t *request, const mcu_frame_t *reply)
{
    /* 解析层已经保证长度不超限，这里只做防御性检查。 */
    if (request->payload_length > MCU_PROTOCOL_MAX_PAYLOAD)
    {
        return;
    }

    dedup_entry_t *entry = &s_dedup[s_dedup_next];
    entry->seq = request->seq;
    entry->cmd = request->cmd;
    entry->version = request->version;
    entry->flags = request->flags;
    entry->payload_length = request->payload_length;
    (void)memcpy(entry->payload, request->payload, request->payload_length);
    entry->reply = *reply;
    entry->is_valid = true;

    s_dedup_next = (uint8_t)((s_dedup_next + 1U) % MCU_COMMAND_DEDUP_SLOTS);
    if (s_dedup_used < MCU_COMMAND_DEDUP_SLOTS)
    {
        ++s_dedup_used;
    }
}

/** @brief 清空重发识别环，初始化时调用。 */
static void dedup_reset(void)
{
    (void)memset(s_dedup, 0, sizeof(s_dedup));
    s_dedup_next = 0U;
    s_dedup_used = 0U;
}

/** @brief 组装一条 STATUS 回复。 */
static void build_status(mcu_frame_t *reply, uint8_t seq, uint32_t now_ms)
{
    mcu_status_t status = {0};

    s_hw->read_status(&status);

    /* 协议规定这些字段由 MCU 自己维护，不看回调填了什么。
     * status_flags 平时为 0，只有失联停机后挂一个 LINK_LOST 位；读到即清，
     * 所以主机下一次 GET_STATUS 就能看到这次停机的真实原因。 */
    status.status_flags = s_status_flags;
    s_status_flags = 0U;
    status.uptime_seconds = (now_ms - s_start_ms) / 1000U;
    status.protocol_errors = (uint16_t)mcu_protocol_error_count();

    mcu_protocol_make_status(reply, seq, &status);
}

/** @brief 组装一条错误 ACK。 */
static void build_error_ack(mcu_frame_t *reply, const mcu_frame_t *request,
                            uint8_t result)
{
    mcu_protocol_make_ack(reply, request->seq, request->cmd, result, 0U);
}

/**
 * @brief 处理关节动作（SET_ARM_POSITION、HOME_ARM）：校验参数、故障门控，
 *        再交给回调。
 */
static void handle_arm_action(const mcu_frame_t *request, mcu_frame_t *reply,
                              uint32_t now_ms)
{
    bool is_move = request->cmd == MCU_PROTOCOL_CMD_SET_ARM_POSITION;
    uint8_t length = is_move ? MCU_PROTOCOL_ARM_POSITION_PAYLOAD_SIZE : 4U;
    if (request->payload_length != length)
    {
        build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_LENGTH);
        return;
    }

    const uint8_t *p = request->payload;
    int32_t target = is_move ? get_i32(&p[4]) : 0;
    int32_t speed = is_move ? get_i32(&p[8]) : 0;
    if (!is_valid_joint(p[0]) || (p[1] != 0U) || (p[2] != 0U) ||
        (p[3] != 0U) || (is_move && (speed <= 0)))
    {
        build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_VALUE);
        return;
    }

    /* 有故障时固件自己也要拒绝危险动作，不能只靠主机门控。 */
    uint8_t faults = current_faults();
    if (faults != 0U)
    {
        mcu_protocol_make_ack(reply, request->seq, request->cmd,
                              MCU_PROTOCOL_RESULT_HARDWARE, faults);
        return;
    }

    uint8_t result = is_move ? s_hw->arm_move(p[0], target, speed)
                             : s_hw->arm_home(p[0]);
    mcu_protocol_make_ack(reply, request->seq, request->cmd, result, 0U);

    /* 真正动起来的关节才按“运动中”时限计时；忙、参数错、未标定都不算。 */
    if (result == MCU_PROTOCOL_RESULT_OK)
    {
        s_is_arm_active = true;
        s_arm_deadline_ms = now_ms + MCU_ARM_MOVE_TIMEOUT_MS;
    }
}

/**
 * @brief 处理输出类扩展命令（SET_LIGHT_ZONE、SET_BUZZER、SET_INDICATOR）：
 *        校验通用字段，再交给回调判断 ID 是否支持。
 *
 * @note 不做故障门控：灯光、蜂鸣器和指示灯不会造成机械危险，有故障时
 *       还可能正需要它们提示用户。
 */
static void handle_output_action(const mcu_frame_t *request, mcu_frame_t *reply)
{
    if (request->payload_length != 4U)
    {
        build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_LENGTH);
        return;
    }

    const uint8_t *p = request->payload;
    bool is_valid = false;
    switch (request->cmd)
    {
        case MCU_PROTOCOL_CMD_SET_LIGHT_ZONE:
            /* 分区 ID，开关 0/1，亮度 0～100，保留。 */
            is_valid = (p[1] <= 1U) && (p[2] <= 100U) && (p[3] == 0U);
            break;
        case MCU_PROTOCOL_CMD_SET_BUZZER:
            /* 图案 ID，音量 0～100，两个保留。 */
            is_valid = (p[1] <= 100U) && (p[2] == 0U) && (p[3] == 0U);
            break;
        default: /* SET_INDICATOR：指示灯 ID，开关 0/1，两个保留。 */
            is_valid = (p[1] <= 1U) && (p[2] == 0U) && (p[3] == 0U);
            break;
    }
    if (!is_valid)
    {
        build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_VALUE);
        return;
    }

    uint8_t result = MCU_PROTOCOL_RESULT_INTERNAL;
    switch (request->cmd)
    {
        case MCU_PROTOCOL_CMD_SET_LIGHT_ZONE:
            result = s_hw->light_zone(p[0], p[1] != 0U, p[2]);
            break;
        case MCU_PROTOCOL_CMD_SET_BUZZER:
            result = s_hw->buzzer(p[0], p[1]);
            break;
        default:
            result = s_hw->indicator(p[0], p[1] != 0U);
            break;
    }
    mcu_protocol_make_ack(reply, request->seq, request->cmd, result, 0U);
}

/**
 * @brief 处理 GET_EXTENDED_STATUS：支持关节类和输出类查询，对应回调没有
 *        注册的查询类型回 INVALID_VALUE。
 */
static void handle_extended_query(const mcu_frame_t *request,
                                  mcu_frame_t *reply)
{
    if (request->payload_length != 4U)
    {
        build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_LENGTH);
        return;
    }

    const uint8_t *p = request->payload;
    bool is_joint_query = (p[0] == MCU_PROTOCOL_CMD_SET_ARM_POSITION) ||
                          (p[0] == MCU_PROTOCOL_CMD_HOME_ARM);
    bool is_valid = (p[2] == 0U) && (p[3] == 0U);
    if (is_joint_query)
    {
        is_valid = is_valid && (s_hw->arm_status != NULL) &&
                   is_valid_joint(p[1]);
    }
    else
    {
        is_valid = is_valid && is_output_command(p[0]) &&
                   (s_hw->output_status != NULL);
    }
    if (!is_valid)
    {
        build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_VALUE);
        return;
    }

    mcu_extended_status_t status = {0};
    uint8_t result = is_joint_query
                         ? s_hw->arm_status(p[0], p[1], &status)
                         : s_hw->output_status(p[0], p[1], &status);
    if (result != MCU_PROTOCOL_RESULT_OK)
    {
        build_error_ack(reply, request, result);
        return;
    }
    status.command = p[0];
    status.selector = p[1];
    mcu_protocol_make_extended_status(reply, request->seq, &status);
}

/** @brief 该命令是否有已注册的处理；关节命令没有回调时按未知命令处理。 */
static bool is_supported(uint8_t cmd)
{
    switch (cmd)
    {
        case MCU_PROTOCOL_CMD_PING:
        case MCU_PROTOCOL_CMD_GET_STATUS:
        case MCU_PROTOCOL_CMD_SET_LIGHT:
        case MCU_PROTOCOL_CMD_SET_MOTION:
            return true;
        case MCU_PROTOCOL_CMD_SET_ARM_POSITION:
            return s_hw->arm_move != NULL;
        case MCU_PROTOCOL_CMD_HOME_ARM:
            return s_hw->arm_home != NULL;
        case MCU_PROTOCOL_CMD_GET_EXTENDED_STATUS:
            return (s_hw->arm_status != NULL) || (s_hw->output_status != NULL);
        case MCU_PROTOCOL_CMD_SET_LIGHT_ZONE:
            return s_hw->light_zone != NULL;
        case MCU_PROTOCOL_CMD_SET_BUZZER:
            return s_hw->buzzer != NULL;
        case MCU_PROTOCOL_CMD_SET_INDICATOR:
            return s_hw->indicator != NULL;
        default:
            return false;
    }
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

    dedup_store(request, reply);
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
    s_status_flags = 0U;
    s_is_chassis_active = false;
    s_is_arm_active = false;
    s_arm_deadline_ms = 0U;
    dedup_reset();
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
    /* 环里任一历史控制帧的原样重发：只重发当时的回复，绝不重复执行副作用。 */
    if (has_side_effect(request->cmd) && dedup_lookup(request, reply))
    {
        note_link_activity(now_ms);
        return true;
    }

    /* 版本不符：整帧不可信，不碰硬件，只回一个错误 ACK。 */
    if (request->version != MCU_PROTOCOL_VERSION)
    {
        build_error_ack(reply, request, MCU_PROTOCOL_RESULT_INVALID_COMMAND);
        remember_request(request, reply, now_ms);
        return true;
    }

    /* 没有对应处理的命令一律走 default；0x00 不是任何命令。 */
    switch (is_supported(request->cmd) ? request->cmd : 0x00U)
    {
        case MCU_PROTOCOL_CMD_PING:
            /* Payload 预留，当前不校验内容。 */
            mcu_protocol_make_ack(reply, request->seq, request->cmd,
                                  MCU_PROTOCOL_RESULT_OK, 0U);
            break;

        case MCU_PROTOCOL_CMD_GET_STATUS:
            if (request->payload_length != 0U)
            {
                build_error_ack(reply, request,
                                MCU_PROTOCOL_RESULT_INVALID_LENGTH);
                break;
            }
            build_status(reply, request->seq, now_ms);
            break;

        case MCU_PROTOCOL_CMD_SET_LIGHT:
        {
            if (request->payload_length != 4U)
            {
                build_error_ack(reply, request,
                                MCU_PROTOCOL_RESULT_INVALID_LENGTH);
                break;
            }

            uint8_t light_flags = request->payload[0];
            uint8_t brightness = request->payload[1];
            uint8_t reserved_flags = request->payload[2];
            uint8_t reserved = request->payload[3];

            if (((light_flags & 0xFEU) != 0U) || (brightness > 100U) ||
                (reserved_flags != 0U) || (reserved != 0U))
            {
                build_error_ack(reply, request,
                                MCU_PROTOCOL_RESULT_INVALID_VALUE);
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
                build_error_ack(reply, request,
                                MCU_PROTOCOL_RESULT_INVALID_LENGTH);
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
                build_error_ack(reply, request,
                                MCU_PROTOCOL_RESULT_INVALID_VALUE);
                break;
            }

            /* 有故障时只放行 STOP，与关节动作同样由固件自己门控。 */
            uint8_t faults = (motion != MCU_PROTOCOL_MOTION_STOP)
                                 ? current_faults()
                                 : 0U;
            if (faults != 0U)
            {
                mcu_protocol_make_ack(reply, request->seq, request->cmd,
                                      MCU_PROTOCOL_RESULT_HARDWARE, faults);
                break;
            }

            if (!s_hw->apply_motion(motion, speed))
            {
                build_error_ack(reply, request, MCU_PROTOCOL_RESULT_HARDWARE);
                break;
            }

            /* 底盘只要有动作就按“运动中”的短时限保护，STOP 后恢复空闲时限。 */
            s_is_chassis_active = (motion != MCU_PROTOCOL_MOTION_STOP);

            mcu_protocol_make_ack(reply, request->seq, request->cmd,
                                  MCU_PROTOCOL_RESULT_OK, 0U);
            break;
        }

        case MCU_PROTOCOL_CMD_SET_ARM_POSITION:
        case MCU_PROTOCOL_CMD_HOME_ARM:
            handle_arm_action(request, reply, now_ms);
            break;

        case MCU_PROTOCOL_CMD_GET_EXTENDED_STATUS:
            handle_extended_query(request, reply);
            break;

        case MCU_PROTOCOL_CMD_SET_LIGHT_ZONE:
        case MCU_PROTOCOL_CMD_SET_BUZZER:
        case MCU_PROTOCOL_CMD_SET_INDICATOR:
            handle_output_action(request, reply);
            break;

        default:
            /* 尚未实现的命令：要求 ACK 才回，否则静默丢弃。 */
            if ((request->flags & MCU_PROTOCOL_FLAG_ACK_REQUIRED) == 0U)
            {
                return false;
            }
            build_error_ack(reply, request,
                            MCU_PROTOCOL_RESULT_INVALID_COMMAND);
            break;
    }

    remember_request(request, reply, now_ms);
    return true;
}

/** @brief 现在是否有电机在动；优先问上层，没注册回调则用命令层自己记的状态。 */
static bool is_any_motion_active(uint32_t now_ms)
{
    if ((s_hw != NULL) && (s_hw->is_motion_active != NULL))
    {
        return s_hw->is_motion_active();
    }

    /* 关节动作只在它自己的期限内算数：到期后不再按运动计时，避免动作早已
     * 完成、主机也不再频繁发帧时被误判失联。 */
    bool is_arm_moving = s_is_arm_active &&
                         ((int32_t)(s_arm_deadline_ms - now_ms) > 0);
    return s_is_chassis_active || is_arm_moving;
}

bool mcu_command_poll(uint32_t now_ms)
{
    if ((s_hw == NULL) || (!s_has_link) || (s_has_stopped))
    {
        return false;
    }

    /* 运动中用短时限：失联后不该让电机再跑满 5 秒（问题清单 A4）。 */
    uint32_t timeout_ms = is_any_motion_active(now_ms)
                              ? MCU_COMMAND_MOTION_LINK_TIMEOUT_MS
                              : MCU_COMMAND_LINK_TIMEOUT_MS;
    if ((now_ms - s_last_rx_ms) < timeout_ms)
    {
        return false;
    }

    s_hw->safe_stop();
    s_has_stopped = true;
    s_is_chassis_active = false;
    s_is_arm_active = false;
    s_arm_deadline_ms = 0U;
    /* 挂一个“失联停机”标志，主机重连后第一次 GET_STATUS 就能读到。 */
    s_status_flags |= MCU_COMMAND_STATUS_FLAG_LINK_LOST;
    return true;
}
