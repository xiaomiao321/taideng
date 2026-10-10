/**
 * @file command_selftest.c
 * @brief mcu_command 的 PC 端自测：用假硬件验证命令分派、去重和失联安全停止。
 *
 * mcu_command 不依赖 HAL，所以这里注册一组假的 mcu_command_hw_t 回调，就能在
 * 电脑上把整条链路跑完：请求报文 -> 命令处理 -> 回复报文 -> 编码成字节。
 * 不需要板子、串口和电机。
 *
 * 编译运行见同目录 run.ps1。
 *
 * @note 本文件不属于固件，不参与 arm 构建。
 * @note 输出一律 ASCII。
 */

#include <stdio.h>
#include <string.h>

#include "mcu_command.h"
#include "mcu_protocol.h"

/* -------------------------------------------------------------------------- */
/* 断言小工具                                                                  */
/* -------------------------------------------------------------------------- */

static size_t s_passed = 0U;
static size_t s_failed = 0U;

static void check(bool condition, const char *what)
{
    if (condition)
    {
        ++s_passed;
        printf("[PASS] %s\n", what);
    }
    else
    {
        ++s_failed;
        printf("[FAIL] %s\n", what);
    }
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right,
                        size_t length)
{
    for (size_t i = 0U; i < length; ++i)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

/** @brief 判断一条回复是不是指定命令、指定结果码的 ACK。 */
static bool ack_is(const mcu_frame_t *frame, uint8_t acked_cmd, uint8_t result)
{
    return (frame->cmd == MCU_PROTOCOL_CMD_ACK) &&
           (frame->payload_length == MCU_PROTOCOL_ACK_PAYLOAD_SIZE) &&
           (frame->payload[0] == acked_cmd) && (frame->payload[1] == result);
}

/* -------------------------------------------------------------------------- */
/* 假硬件                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct
{
    uint32_t read_status_calls;
    uint32_t apply_light_calls;
    uint32_t apply_motion_calls;
    uint32_t safe_stop_calls;
    uint8_t last_light_flags;
    uint8_t last_brightness;
    uint8_t last_motion;
    uint8_t last_speed;
    uint8_t light_enabled;
    uint8_t brightness;
    uint8_t motion;
    uint8_t speed;
} fake_hw_t;

static fake_hw_t s_fake;

static void fake_read_status(mcu_status_t *status)
{
    ++s_fake.read_status_calls;
    status->light_enabled = s_fake.light_enabled;
    status->brightness = s_fake.brightness;
    status->motion = s_fake.motion;
    status->target_motion = s_fake.motion;
    status->speed = s_fake.speed;
    status->target_speed = s_fake.speed;
    status->fault_flags = 0U;
}

static bool fake_apply_motion(uint8_t motion, uint8_t speed)
{
    ++s_fake.apply_motion_calls;
    s_fake.last_motion = motion;
    s_fake.last_speed = speed;
    s_fake.motion = motion;
    s_fake.speed = speed;
    return true;
}

static bool fake_apply_light(uint8_t light_flags, uint8_t brightness)
{
    ++s_fake.apply_light_calls;
    s_fake.last_light_flags = light_flags;
    s_fake.last_brightness = brightness;
    s_fake.light_enabled = (uint8_t)(light_flags & 0x01U);
    s_fake.brightness = brightness;
    return true;
}

static void fake_safe_stop(void)
{
    ++s_fake.safe_stop_calls;
    s_fake.motion = MCU_PROTOCOL_MOTION_STOP;
    s_fake.speed = 0U;
}

static const mcu_command_hw_t k_fake_hw = {fake_read_status, fake_apply_light,
                                           fake_apply_motion, fake_safe_stop};

static void fake_reset(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    mcu_protocol_reset();
}

/* -------------------------------------------------------------------------- */
/* 小工具：拼一条请求                                                          */
/* -------------------------------------------------------------------------- */

static mcu_frame_t make_request(uint8_t seq, uint8_t cmd, const uint8_t *payload,
                                uint8_t payload_length, uint8_t flags)
{
    mcu_frame_t frame = {0};

    frame.version = MCU_PROTOCOL_VERSION;
    frame.flags = flags;
    frame.seq = seq;
    frame.cmd = cmd;
    frame.payload_length = payload_length;
    if ((payload != NULL) && (payload_length > 0U))
    {
        memcpy(frame.payload, payload, payload_length);
    }
    return frame;
}

/* 一条 CRC 被改坏的帧，用来给解析器制造协议错误 */
static const uint8_t k_bad_crc_frame[] = {0xA5, 0x5A, 0x02, 0x00, 0x04,
                                          0x20, 0x00, 0x00, 0x01, 0x02};

/* -------------------------------------------------------------------------- */
/* 用例                                                                        */
/* -------------------------------------------------------------------------- */

/** @brief GET_STATUS 必须回 16 字节 STATUS，序号沿用，状态为第一阶段的安全值。 */
static void test_get_status_reply(void)
{
    mcu_frame_t request =
        make_request(0x11U, MCU_PROTOCOL_CMD_GET_STATUS, NULL, 0U, 0U);
    mcu_frame_t reply = {0};
    uint8_t out[MCU_PROTOCOL_MAX_FRAME_SIZE];

    fake_reset();
    mcu_command_init(&k_fake_hw, 1000U);

    check(mcu_command_handle(&request, &reply, 1000U), "get_status: has a reply");
    check((reply.cmd == MCU_PROTOCOL_CMD_STATUS) && (reply.seq == 0x11U) &&
              (reply.flags == MCU_PROTOCOL_FLAG_RESPONSE) &&
              (reply.payload_length == MCU_PROTOCOL_STATUS_PAYLOAD_SIZE),
          "get_status: STATUS frame shape");
    check((reply.payload[0] == 0U) && (reply.payload[2] == 0U) &&
              (reply.payload[4] == 0U),
          "get_status: light off, motion stop, speed 0");
    check((reply.payload[6] == 0U) && (reply.payload[7] == 0U) &&
              (reply.payload[8] == 0U) && (reply.payload[14] == 0U) &&
              (reply.payload[15] == 0U),
          "get_status: no fault, reserved bytes zero");

    /* 组出来的回复必须是能通过自身 CRC 校验的合法帧。 */
    size_t length = mcu_protocol_encode(&reply, out, sizeof(out));
    check(length == 26U, "get_status: encodes to 26 bytes");
    check((out[0] == 0xA5U) && (out[1] == 0x5AU) && (out[5] == 0x21U),
          "get_status: encoded header is correct");
}

/** @brief STATUS 里的运行秒数和协议错误数由命令层自己维护。 */
static void test_status_uptime_and_errors(void)
{
    mcu_frame_t request =
        make_request(0x12U, MCU_PROTOCOL_CMD_GET_STATUS, NULL, 0U, 0U);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_protocol_feed(k_bad_crc_frame, sizeof(k_bad_crc_frame));

    mcu_command_init(&k_fake_hw, 1000U);
    check(mcu_command_handle(&request, &reply, 4500U), "uptime: has a reply");
    check(reply.payload[8] == 3U, "uptime: 3 seconds after 3.5 s");
    check((reply.payload[12] == 1U) && (reply.payload[13] == 0U),
          "uptime: protocol error count is reported");
}

/** @brief PING 回成功 ACK。 */
static void test_ping(void)
{
    mcu_frame_t request =
        make_request(0x13U, MCU_PROTOCOL_CMD_PING, NULL, 0U, 0U);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    check(mcu_command_handle(&request, &reply, 10U), "ping: has a reply");
    check(ack_is(&reply, MCU_PROTOCOL_CMD_PING, MCU_PROTOCOL_RESULT_OK),
          "ping: ACK ok");
    check(reply.flags == MCU_PROTOCOL_FLAG_RESPONSE, "ping: no ERROR flag");
}

/** @brief STOP 必须真的作用到硬件，并且回成功 ACK。 */
static void test_stop(void)
{
    const uint8_t payload[4] = {MCU_PROTOCOL_MOTION_STOP, 0U, 0U, 0U};
    mcu_frame_t request = make_request(0x14U, MCU_PROTOCOL_CMD_SET_MOTION,
                                       payload, 4U, MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    check(mcu_command_handle(&request, &reply, 10U), "stop: has a reply");
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_MOTION, MCU_PROTOCOL_RESULT_OK),
          "stop: ACK ok");
    check((s_fake.apply_motion_calls == 1U) &&
              (s_fake.last_motion == MCU_PROTOCOL_MOTION_STOP) &&
              (s_fake.last_speed == 0U),
          "stop: applied to hardware");
}

/** @brief 还没实现的动作要明确拒绝，不能假装接受。 */
static void test_unimplemented_motion_is_rejected(void)
{
    const uint8_t payload[4] = {MCU_PROTOCOL_MOTION_FORWARD, 100U, 0U, 0U};
    mcu_frame_t request = make_request(0x15U, MCU_PROTOCOL_CMD_SET_MOTION,
                                       payload, 4U, MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    check(mcu_command_handle(&request, &reply, 10U), "forward: has a reply");
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_MOTION,
                 MCU_PROTOCOL_RESULT_INVALID_COMMAND),
          "forward: rejected as not implemented");
    check(s_fake.apply_motion_calls == 0U, "forward: hardware untouched");
}

/** @brief 参数越界、保留位非 0、STOP 带速度都要回参数错误。 */
static void test_motion_parameter_errors(void)
{
    mcu_frame_t reply = {0};
    uint8_t payload[4];

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    payload[0] = MCU_PROTOCOL_MOTION_STOP;
    payload[1] = 50U; /* STOP 的速度必须是 0 */
    payload[2] = 0U;
    payload[3] = 0U;
    mcu_frame_t request = make_request(0x16U, MCU_PROTOCOL_CMD_SET_MOTION,
                                       payload, 4U, MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    (void)mcu_command_handle(&request, &reply, 10U);
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_MOTION,
                 MCU_PROTOCOL_RESULT_INVALID_VALUE),
          "motion: STOP with speed 50 is invalid");

    payload[0] = 0x09U; /* 保留动作码 */
    payload[1] = 10U;
    request = make_request(0x17U, MCU_PROTOCOL_CMD_SET_MOTION, payload, 4U,
                           MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    (void)mcu_command_handle(&request, &reply, 20U);
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_MOTION,
                 MCU_PROTOCOL_RESULT_INVALID_VALUE),
          "motion: reserved code is invalid");

    payload[0] = MCU_PROTOCOL_MOTION_STOP;
    payload[1] = 0U;
    payload[2] = 0x01U; /* CONTROL_FLAGS 当前必须写 0 */
    request = make_request(0x18U, MCU_PROTOCOL_CMD_SET_MOTION, payload, 4U,
                           MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    (void)mcu_command_handle(&request, &reply, 30U);
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_MOTION,
                 MCU_PROTOCOL_RESULT_INVALID_VALUE),
          "motion: non-zero control flags is invalid");

    check(s_fake.apply_motion_calls == 0U,
          "motion: no invalid command reached hardware");
}

/** @brief PAYLOAD 长度不对要回长度错误。 */
static void test_length_errors(void)
{
    mcu_frame_t reply = {0};
    const uint8_t one_byte[1] = {0U};
    const uint8_t three_bytes[3] = {0U, 0U, 0U};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    mcu_frame_t request = make_request(0x19U, MCU_PROTOCOL_CMD_GET_STATUS,
                                       one_byte, 1U, 0U);
    (void)mcu_command_handle(&request, &reply, 10U);
    check(ack_is(&reply, MCU_PROTOCOL_CMD_GET_STATUS,
                 MCU_PROTOCOL_RESULT_INVALID_LENGTH),
          "length: GET_STATUS with payload is invalid");

    request = make_request(0x1AU, MCU_PROTOCOL_CMD_SET_MOTION, three_bytes, 3U,
                           MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    (void)mcu_command_handle(&request, &reply, 20U);
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_MOTION,
                 MCU_PROTOCOL_RESULT_INVALID_LENGTH),
          "length: SET_MOTION with 3 bytes is invalid");
}

/** @brief 未知命令：要 ACK 才回错误 ACK，不要 ACK 就静默丢弃。 */
static void test_unknown_command(void)
{
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    mcu_frame_t request = make_request(0x1BU, 0x55U, NULL, 0U,
                                       MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    check(mcu_command_handle(&request, &reply, 10U), "unknown: ACK required");
    check(ack_is(&reply, 0x55U, MCU_PROTOCOL_RESULT_INVALID_COMMAND),
          "unknown: ACK says invalid command");

    request = make_request(0x1CU, 0x55U, NULL, 0U, 0U);
    check(!mcu_command_handle(&request, &reply, 20U),
          "unknown: silently dropped without ACK_REQUIRED");

    /* 扩展命令 0x31 尚未实现，同样按未知命令处理。 */
    request = make_request(0x1DU, 0x31U, NULL, 0U,
                           MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    check(mcu_command_handle(&request, &reply, 30U), "extension: has a reply");
    check(ack_is(&reply, 0x31U, MCU_PROTOCOL_RESULT_INVALID_COMMAND),
          "extension: not implemented yet");
}

/** @brief 带 RESPONSE 位的帧是回复，主机不该发给我们。 */
static void test_response_frame_is_ignored(void)
{
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    mcu_frame_t request = make_request(0x1EU, MCU_PROTOCOL_CMD_GET_STATUS, NULL,
                                       0U, MCU_PROTOCOL_FLAG_RESPONSE);
    check(!mcu_command_handle(&request, &reply, 10U),
          "response: incoming response frame is ignored");
}

/** @brief 版本不符：不碰硬件，回未知命令。 */
static void test_wrong_version(void)
{
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    mcu_frame_t request =
        make_request(0x1FU, MCU_PROTOCOL_CMD_GET_STATUS, NULL, 0U, 0U);
    request.version = 0x03U;

    check(mcu_command_handle(&request, &reply, 10U), "version: has a reply");
    check(ack_is(&reply, MCU_PROTOCOL_CMD_GET_STATUS,
                 MCU_PROTOCOL_RESULT_INVALID_COMMAND),
          "version: rejected without touching hardware");
    check(s_fake.read_status_calls == 0U,
          "version: status was not even read");
}

/** @brief 重复的 (SEQ, CMD) 只重发 ACK，不重复执行。 */
static void test_duplicate_request(void)
{
    const uint8_t payload[4] = {MCU_PROTOCOL_MOTION_STOP, 0U, 0U, 0U};
    mcu_frame_t request = make_request(0x20U, MCU_PROTOCOL_CMD_SET_MOTION,
                                       payload, 4U, MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    mcu_frame_t first = {0};
    mcu_frame_t second = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    (void)mcu_command_handle(&request, &first, 10U);
    check(s_fake.apply_motion_calls == 1U, "duplicate: first stop applied");

    (void)mcu_command_handle(&request, &second, 510U);
    check(s_fake.apply_motion_calls == 1U, "duplicate: not applied twice");
    check((first.cmd == second.cmd) && (first.seq == second.seq) &&
              (first.payload_length == second.payload_length) &&
              bytes_equal(first.payload, second.payload, first.payload_length),
          "duplicate: same ACK is resent");

    /* 序号不同就是新命令，必须重新执行。 */
    request = make_request(0x21U, MCU_PROTOCOL_CMD_SET_MOTION, payload, 4U,
                           MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    (void)mcu_command_handle(&request, &second, 520U);
    check(s_fake.apply_motion_calls == 2U, "duplicate: new SEQ re-executes");
}

/** @brief 5 秒没有合法帧就安全停止，而且只停一次。 */
static void test_link_timeout(void)
{
    mcu_frame_t request =
        make_request(0x22U, MCU_PROTOCOL_CMD_GET_STATUS, NULL, 0U, 0U);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    check(!mcu_command_poll(4999U), "timeout: no stop before any request");

    (void)mcu_command_handle(&request, &reply, 100U);
    check(!mcu_command_poll(5099U), "timeout: not yet at 4.999 s");
    check(mcu_command_poll(5100U), "timeout: stops at 5 s of silence");
    check(s_fake.safe_stop_calls == 1U, "timeout: safe stop called once");
    check(!mcu_command_poll(9000U), "timeout: does not stop again");

    /* 重新收到请求后计时重新开始。 */
    request = make_request(0x23U, MCU_PROTOCOL_CMD_GET_STATUS, NULL, 0U, 0U);
    (void)mcu_command_handle(&request, &reply, 10000U);
    check(!mcu_command_poll(14999U), "timeout: timer restarts after a request");
    check(mcu_command_poll(15000U), "timeout: stops again after another 5 s");
    check(s_fake.safe_stop_calls == 2U, "timeout: safe stop called twice");
}

/** @brief 重复的查询必须返回当前状态，不能重放缓存里的旧状态。 */
static void test_repeated_query_is_fresh(void)
{
    mcu_frame_t request =
        make_request(0x30U, MCU_PROTOCOL_CMD_GET_STATUS, NULL, 0U, 0U);
    mcu_frame_t first = {0};
    mcu_frame_t second = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    (void)mcu_command_handle(&request, &first, 1000U);
    (void)mcu_command_handle(&request, &second, 5000U);

    check(first.payload[8] == 1U, "repeat query: first reply reports 1 s");
    check(second.payload[8] == 5U,
          "repeat query: same SEQ gets a fresh status, not a cached one");
}

/** @brief 中间夹一次查询，不能把控制命令的去重记录冲掉。 */
static void test_dedup_survives_interleaved_query(void)
{
    const uint8_t payload[4] = {MCU_PROTOCOL_MOTION_STOP, 0U, 0U, 0U};
    mcu_frame_t stop = make_request(0x31U, MCU_PROTOCOL_CMD_SET_MOTION, payload,
                                    4U, MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    mcu_frame_t query =
        make_request(0x32U, MCU_PROTOCOL_CMD_GET_STATUS, NULL, 0U, 0U);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    (void)mcu_command_handle(&stop, &reply, 10U);
    (void)mcu_command_handle(&query, &reply, 20U);
    (void)mcu_command_handle(&stop, &reply, 30U);

    check(s_fake.apply_motion_calls == 1U,
          "dedup: interleaved query does not defeat dedup");
}

/** @brief 重复帧同样算链路活动，不能因为一直重发而被判失联。 */
static void test_duplicate_refreshes_link(void)
{
    const uint8_t payload[4] = {MCU_PROTOCOL_MOTION_STOP, 0U, 0U, 0U};
    mcu_frame_t stop = make_request(0x33U, MCU_PROTOCOL_CMD_SET_MOTION, payload,
                                    4U, MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    (void)mcu_command_handle(&stop, &reply, 1000U);
    (void)mcu_command_handle(&stop, &reply, 5900U); /* 重复帧 */

    check(!mcu_command_poll(6000U), "duplicate: link timer was refreshed");
    check(s_fake.safe_stop_calls == 0U,
          "duplicate: retries alone do not trigger a link timeout");
}

/** @brief 开灯 50%：ACK 成功，硬件被驱动，随后查询能读到实际状态。 */
static void test_set_light_on(void)
{
    const uint8_t payload[4] = {0x01U, 50U, 0U, 0U};
    mcu_frame_t request =
        make_request(0x40U, MCU_PROTOCOL_CMD_SET_LIGHT, payload, 4U,
                     MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    check(mcu_command_handle(&request, &reply, 10U), "light on: has a reply");
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_LIGHT, MCU_PROTOCOL_RESULT_OK),
          "light on: ACK ok");
    check((s_fake.apply_light_calls == 1U) &&
              (s_fake.last_light_flags == 0x01U) &&
              (s_fake.last_brightness == 50U),
          "light on: applied to hardware");

    request = make_request(0x41U, MCU_PROTOCOL_CMD_GET_STATUS, NULL, 0U, 0U);
    (void)mcu_command_handle(&request, &reply, 20U);
    check((reply.payload[0] == 1U) && (reply.payload[1] == 50U),
          "light on: status reads back on and 50%");
}

/** @brief 关灯保留亮度，下一次开灯直接沿用。 */
static void test_set_light_off_keeps_brightness(void)
{
    const uint8_t on[4] = {0x01U, 50U, 0U, 0U};
    const uint8_t off[4] = {0x00U, 50U, 0U, 0U};
    mcu_frame_t request = make_request(0x42U, MCU_PROTOCOL_CMD_SET_LIGHT, on, 4U,
                                       MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    (void)mcu_command_handle(&request, &reply, 10U);
    request = make_request(0x43U, MCU_PROTOCOL_CMD_SET_LIGHT, off, 4U,
                           MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    (void)mcu_command_handle(&request, &reply, 20U);

    check(s_fake.apply_light_calls == 2U, "light off: applied");
    check((s_fake.light_enabled == 0U) && (s_fake.brightness == 50U),
          "light off: brightness is retained");
}

/** @brief 参数越界、保留位非 0 都要回参数错误，且不碰硬件。 */
static void test_set_light_parameter_errors(void)
{
    mcu_frame_t reply = {0};
    uint8_t payload[4] = {0x01U, 50U, 0U, 0U};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    payload[1] = 101U; /* 亮度上限 100 */
    mcu_frame_t request =
        make_request(0x44U, MCU_PROTOCOL_CMD_SET_LIGHT, payload, 4U,
                     MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    (void)mcu_command_handle(&request, &reply, 10U);
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_LIGHT,
                 MCU_PROTOCOL_RESULT_INVALID_VALUE),
          "light: brightness 101 is invalid");

    payload[1] = 50U;
    payload[0] = 0x02U; /* LIGHT_FLAGS 只允许 bit0 */
    request = make_request(0x45U, MCU_PROTOCOL_CMD_SET_LIGHT, payload, 4U,
                           MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    (void)mcu_command_handle(&request, &reply, 20U);
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_LIGHT,
                 MCU_PROTOCOL_RESULT_INVALID_VALUE),
          "light: reserved flag bits are invalid");

    payload[0] = 0x01U;
    payload[2] = 0x01U; /* RESERVED_FLAGS 必须写 0 */
    request = make_request(0x46U, MCU_PROTOCOL_CMD_SET_LIGHT, payload, 4U,
                           MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    (void)mcu_command_handle(&request, &reply, 30U);
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_LIGHT,
                 MCU_PROTOCOL_RESULT_INVALID_VALUE),
          "light: non-zero reserved field is invalid");

    check(s_fake.apply_light_calls == 0U,
          "light: invalid parameters never reach hardware");
}

/** @brief SET_LIGHT 的 PAYLOAD 必须是 4 字节。 */
static void test_set_light_length_error(void)
{
    const uint8_t short_payload[3] = {0x01U, 50U, 0U};
    mcu_frame_t request =
        make_request(0x47U, MCU_PROTOCOL_CMD_SET_LIGHT, short_payload, 3U,
                     MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    (void)mcu_command_handle(&request, &reply, 10U);
    check(ack_is(&reply, MCU_PROTOCOL_CMD_SET_LIGHT,
                 MCU_PROTOCOL_RESULT_INVALID_LENGTH),
          "light: 3-byte payload is invalid");
}

/** @brief 重复的 SET_LIGHT 只重发 ACK，不重复驱动灯。 */
static void test_duplicate_set_light(void)
{
    const uint8_t payload[4] = {0x01U, 80U, 0U, 0U};
    mcu_frame_t request =
        make_request(0x48U, MCU_PROTOCOL_CMD_SET_LIGHT, payload, 4U,
                     MCU_PROTOCOL_FLAG_ACK_REQUIRED);
    mcu_frame_t reply = {0};

    fake_reset();
    mcu_command_init(&k_fake_hw, 0U);

    (void)mcu_command_handle(&request, &reply, 10U);
    (void)mcu_command_handle(&request, &reply, 510U);

    check(s_fake.apply_light_calls == 1U,
          "light duplicate: applied only once, ACK resent");
}

int main(void)
{
    test_get_status_reply();
    test_status_uptime_and_errors();
    test_ping();
    test_stop();
    test_unimplemented_motion_is_rejected();
    test_motion_parameter_errors();
    test_length_errors();
    test_unknown_command();
    test_response_frame_is_ignored();
    test_wrong_version();
    test_duplicate_request();
    test_link_timeout();
    test_repeated_query_is_fresh();
    test_dedup_survives_interleaved_query();
    test_duplicate_refreshes_link();
    test_set_light_on();
    test_set_light_off_keeps_brightness();
    test_set_light_parameter_errors();
    test_set_light_length_error();
    test_duplicate_set_light();

    printf("\n%u passed, %u failed\n", (unsigned int)s_passed,
           (unsigned int)s_failed);
    return (s_failed == 0U) ? 0 : 1;
}
