/**
 * @file protocol_selftest.c
 * @brief mcu_protocol 的 PC 端自测：验证 CRC 与帧解析，不需要 MCU 和串口。
 *
 * 编译运行见同目录 run.ps1。
 *
 * 第一部分 CRC：向量取自 MCU通信协议.md 的示例帧，其中 "123456789" 是
 * CRC16/MODBUS 的经典参考向量 0x4B37。
 * 第二部分 解析：字节流由仓库内参考实现 mcu-tools/protocol.py 生成，覆盖拆包、
 * 粘包、噪声、坏 CRC 重同步、LEN 超过 64、PAYLOAD 里出现帧头等情形。
 *
 * @note 本文件不属于固件，不参与 arm 构建。
 * @note 用例名和输出一律 ASCII：UTF-8 源码里的中文输出到 GBK 控制台会变成乱码。
 */

#include <stdio.h>
#include <string.h>

#include "mcu_protocol.h"

/* -------------------------------------------------------------------------- */
/* 断言小工具                                                                  */
/* -------------------------------------------------------------------------- */

static size_t s_passed = 0U;
static size_t s_failed = 0U;

/** @brief 记录并打印一条断言结果。 */
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

/* -------------------------------------------------------------------------- */
/* 第一部分：CRC16/MODBUS                                                      */
/* -------------------------------------------------------------------------- */

/** @brief 一条 CRC 测试向量。 */
typedef struct
{
    const char *name;    /**< 用例名，用于打印。 */
    const uint8_t *data; /**< 校验数据，对应帧内 VERSION 到 PAYLOAD 的部分。 */
    size_t length;       /**< 校验字节数。 */
    uint16_t expected;   /**< 期望 CRC。 */
} crc_case_t;

static const uint8_t k_ascii[] = "123456789";
static const uint8_t k_crc_get_status[] = {0x02, 0x00, 0x04, 0x20, 0x00, 0x00};
static const uint8_t k_crc_set_light[] = {0x02, 0x02, 0x01, 0x10, 0x04,
                                          0x00, 0x01, 0x32, 0x00, 0x00};
static const uint8_t k_crc_ack_light[] = {0x02, 0x01, 0x01, 0x7F, 0x04,
                                          0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t k_crc_motion_fwd[] = {0x02, 0x02, 0x02, 0x11, 0x04,
                                           0x00, 0x01, 0x4B, 0x00, 0x00};
static const uint8_t k_crc_motion_stop[] = {0x02, 0x02, 0x03, 0x11, 0x04,
                                            0x00, 0x00, 0x00, 0x00, 0x00};

/* 用例名一律用 ASCII：UTF-8 源码里的中文输出到 GBK 控制台会变成乱码，
 * 串口助手和日志工具也各不相同。仓库内的日志同样遵循纯 ASCII 约定。 */
static const crc_case_t g_crc_cases[] = {
    {"ASCII 123456789", k_ascii, sizeof(k_ascii) - 1U, 0x4B37U},
    {"GET_STATUS query", k_crc_get_status, sizeof(k_crc_get_status), 0x0301U},
    {"SET_LIGHT on, 50%", k_crc_set_light, sizeof(k_crc_set_light), 0x17B9U},
    {"ACK SET_LIGHT ok", k_crc_ack_light, sizeof(k_crc_ack_light), 0x1296U},
    {"SET_MOTION fwd 75%", k_crc_motion_fwd, sizeof(k_crc_motion_fwd), 0x1B38U},
    {"SET_MOTION stop", k_crc_motion_stop, sizeof(k_crc_motion_stop), 0x3D88U},
};

static void run_crc_tests(void)
{
    const size_t count = sizeof(g_crc_cases) / sizeof(g_crc_cases[0]);

    for (size_t i = 0U; i < count; ++i)
    {
        const crc_case_t *test_case = &g_crc_cases[i];
        uint16_t actual =
            mcu_protocol_crc16(test_case->data, test_case->length);

        if (actual == test_case->expected)
        {
            ++s_passed;
            printf("[PASS] CRC = 0x%04X  (%s)\n", (unsigned int)actual,
                   test_case->name);
        }
        else
        {
            ++s_failed;
            printf("[FAIL] expected 0x%04X, got 0x%04X  (%s)\n",
                   (unsigned int)test_case->expected, (unsigned int)actual,
                   test_case->name);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 第二部分：帧解析                                                            */
/*                                                                             */
/* 下面的字节流全部由 mcu-tools/protocol.py 生成，不是手算的。                  */
/* -------------------------------------------------------------------------- */

/* GET_STATUS 查询：VERSION=02, SEQ=04, CMD=20, LEN=0（协议文档示例） */
static const uint8_t k_get_status[] = {0xA5, 0x5A, 0x02, 0x00, 0x04,
                                       0x20, 0x00, 0x00, 0x01, 0x03};

/* SET_LIGHT 开灯 50%：SEQ=01（协议文档示例） */
static const uint8_t k_set_light[] = {0xA5, 0x5A, 0x02, 0x02, 0x01, 0x10, 0x04,
                                      0x00, 0x01, 0x32, 0x00, 0x00, 0xB9, 0x17};

/* SET_MOTION 停止：SEQ=03（协议文档示例） */
static const uint8_t k_motion_stop[] = {0xA5, 0x5A, 0x02, 0x02, 0x03,
                                        0x11, 0x04, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x88, 0x3D};

/* 把 GET_STATUS 的 CRC 最后一个字节改坏，其余完全一致 */
static const uint8_t k_bad_crc[] = {0xA5, 0x5A, 0x02, 0x00, 0x04,
                                    0x20, 0x00, 0x00, 0x01, 0x02};

/* LEN=0x41=65，超过最大 64，长度非法；但 CRC 本身是对的（共 75 字节） */
static const uint8_t k_len_too_large[] = {
    0xA5, 0x5A, 0x02, 0x00, 0x09, 0x20, 0x41, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3B, 0xE5,
};

/* PAYLOAD 内部含 A5 5A：解析器不能被它带偏。SEQ=09, CMD=10, LEN=4 */
static const uint8_t k_payload_with_sof[] = {0xA5, 0x5A, 0x02, 0x02, 0x09,
                                             0x10, 0x04, 0x00, 0xA5, 0x5A,
                                             0x00, 0x00, 0x1A, 0x5D};

/* VERSION=03：解析层不该拒绝版本，应照常交给命令层判断。SEQ=07, CMD=21 */
static const uint8_t k_wrong_version[] = {0xA5, 0x5A, 0x03, 0x00, 0x07,
                                          0x21, 0x00, 0x00, 0x51, 0x56};

/* 帧前噪声："noise" 的 ASCII 字节 */
static const uint8_t k_noise[] = {0x6E, 0x6F, 0x69, 0x73, 0x65};

/** @brief 半条报文分三次喂，最后一块到达时才应切出一帧。 */
static void test_fragmented_frame(void)
{
    mcu_frame_t frame = {0};

    mcu_protocol_reset();
    check(!mcu_protocol_pop_frame(&frame), "fragmented: empty queue at start");

    mcu_protocol_feed(k_get_status, 4U);
    check(!mcu_protocol_pop_frame(&frame),
          "fragmented: no frame after 4 bytes");

    mcu_protocol_feed(&k_get_status[4], 3U);
    check(!mcu_protocol_pop_frame(&frame),
          "fragmented: no frame after 7 bytes");

    mcu_protocol_feed(&k_get_status[7], 3U);
    check(mcu_protocol_pop_frame(&frame), "fragmented: frame completed");
    check((frame.version == 0x02U) && (frame.flags == 0x00U) &&
              (frame.seq == 0x04U) && (frame.cmd == 0x20U) &&
              (frame.payload_length == 0U),
          "fragmented: frame fields");
    check(!mcu_protocol_pop_frame(&frame), "fragmented: empty queue at end");
}

/** @brief 每次只喂一个字节，最容易暴露"状态没跨调用保留"的问题。 */
static void test_byte_by_byte(void)
{
    mcu_frame_t frame = {0};
    size_t frames = 0U;

    mcu_protocol_reset();
    for (size_t i = 0U; i < sizeof(k_motion_stop); ++i)
    {
        mcu_protocol_feed(&k_motion_stop[i], 1U);
        while (mcu_protocol_pop_frame(&frame))
        {
            ++frames;
        }
    }

    check(frames == 1U, "byte-by-byte: exactly one frame");
    check((frame.seq == 0x03U) && (frame.cmd == 0x11U) &&
              (frame.payload_length == 4U) && (frame.payload[0] == 0x00U),
          "byte-by-byte: frame fields");
}

/** @brief 两条报文粘在一次接收里，应切出两帧且顺序正确。 */
static void test_two_frames_one_chunk(void)
{
    uint8_t stream[sizeof(k_set_light) + sizeof(k_motion_stop)];
    mcu_frame_t frame = {0};

    memcpy(stream, k_set_light, sizeof(k_set_light));
    memcpy(&stream[sizeof(k_set_light)], k_motion_stop, sizeof(k_motion_stop));

    mcu_protocol_reset();
    mcu_protocol_feed(stream, sizeof(stream));

    check(mcu_protocol_pop_frame(&frame) && (frame.seq == 0x01U) &&
              (frame.cmd == 0x10U) && (frame.payload_length == 4U) &&
              (frame.payload[1] == 0x32U),
          "two-in-one: first frame");
    check(mcu_protocol_pop_frame(&frame) && (frame.seq == 0x03U) &&
              (frame.cmd == 0x11U),
          "two-in-one: second frame");
    check(!mcu_protocol_pop_frame(&frame), "two-in-one: no third frame");
}

/**
 * @brief 一次收到的报文多于队列容量时，多出的字节不能被丢掉。
 *
 * feed() 应在队列满时停下并返回已取走的字节数；调用者取走报文后
 * 从断点继续喂，全部报文按顺序到达，错误计数保持为 0。
 */
static void test_queue_full_keeps_bytes(void)
{
    enum
    {
        FRAME_COUNT = 12
    };
    uint8_t stream[FRAME_COUNT * MCU_PROTOCOL_FIXED_SIZE];
    size_t stream_length = 0U;
    mcu_frame_t frame = {0};

    for (uint8_t seq = 0U; seq < FRAME_COUNT; ++seq)
    {
        mcu_frame_t request = {0};
        request.version = MCU_PROTOCOL_VERSION;
        request.seq = seq;
        request.cmd = MCU_PROTOCOL_CMD_GET_STATUS;
        stream_length += mcu_protocol_encode(&request, &stream[stream_length],
                                             sizeof(stream) - stream_length);
    }

    mcu_protocol_reset();
    size_t offset = mcu_protocol_feed(stream, stream_length);
    check(offset < stream_length, "queue-full: feed stops early");

    bool in_order = true;
    uint8_t expected_seq = 0U;
    for (unsigned round = 0U; round <= FRAME_COUNT; ++round)
    {
        bool popped = false;
        while (mcu_protocol_pop_frame(&frame))
        {
            popped = true;
            in_order = in_order && (frame.seq == expected_seq);
            ++expected_seq;
        }
        if (!popped && (offset >= stream_length))
        {
            break;
        }
        /* 字节喂完以后仍要空喂，切出缓冲里因队列满而留下的完整报文。 */
        offset += mcu_protocol_feed(&stream[offset], stream_length - offset);
    }

    check(in_order && (expected_seq == FRAME_COUNT),
          "queue-full: all frames arrive in order");
    check(mcu_protocol_error_count() == 0U, "queue-full: no error counted");
}

/** @brief 帧头前面有噪声字节，应能跳过噪声找到真正的帧。 */
static void test_noise_prefix(void)
{
    mcu_frame_t frame = {0};

    mcu_protocol_reset();
    mcu_protocol_feed(k_noise, sizeof(k_noise));
    check(!mcu_protocol_pop_frame(&frame), "noise: nothing parsed from noise");

    mcu_protocol_feed(k_get_status, sizeof(k_get_status));
    check(mcu_protocol_pop_frame(&frame) && (frame.seq == 0x04U),
          "noise: frame found after noise");
}

/** @brief CRC 错的帧必须被丢弃，并且不能影响紧随其后的好帧。 */
static void test_bad_crc_resync(void)
{
    mcu_frame_t frame = {0};

    mcu_protocol_reset();
    mcu_protocol_feed(k_bad_crc, sizeof(k_bad_crc));
    check(!mcu_protocol_pop_frame(&frame), "bad-crc: frame rejected");

    mcu_protocol_feed(k_get_status, sizeof(k_get_status));
    check(mcu_protocol_pop_frame(&frame) && (frame.seq == 0x04U),
          "bad-crc: resynced to the next frame");
    check(mcu_protocol_error_count() >= 1U, "bad-crc: error counted");
}

/** @brief LEN 超过 64 的帧非法，同样要丢弃并重新同步。 */
static void test_length_too_large(void)
{
    mcu_frame_t frame = {0};

    mcu_protocol_reset();
    mcu_protocol_feed(k_len_too_large, sizeof(k_len_too_large));
    check(!mcu_protocol_pop_frame(&frame), "len>64: frame rejected");

    mcu_protocol_feed(k_get_status, sizeof(k_get_status));
    check(mcu_protocol_pop_frame(&frame) && (frame.seq == 0x04U),
          "len>64: next frame still parsed");
    check(mcu_protocol_error_count() >= 1U, "len>64: error counted");
}

/** @brief PAYLOAD 里出现 A5 5A 时，解析器不能被它当成帧头。 */
static void test_payload_contains_sof(void)
{
    mcu_frame_t frame = {0};

    mcu_protocol_reset();
    mcu_protocol_feed(k_payload_with_sof, sizeof(k_payload_with_sof));

    check(mcu_protocol_pop_frame(&frame), "sof-in-payload: frame parsed");
    check((frame.payload_length == 4U) && (frame.payload[0] == 0xA5U) &&
              (frame.payload[1] == 0x5AU),
          "sof-in-payload: payload intact");
    check(!mcu_protocol_pop_frame(&frame),
          "sof-in-payload: no phantom second frame");
}

/** @brief 版本号不是 0x02 的帧由命令层决定怎么处理，解析层应照常交付。 */
static void test_version_not_filtered(void)
{
    mcu_frame_t frame = {0};

    mcu_protocol_reset();
    mcu_protocol_feed(k_wrong_version, sizeof(k_wrong_version));

    check(mcu_protocol_pop_frame(&frame),
          "version 3: delivered to the command layer");
    check(frame.version == 0x03U, "version 3: version preserved");
}

/* -------------------------------------------------------------------------- */
/* 第三部分：组包                                                              */
/*                                                                             */
/* 期望字节由 mcu-tools/protocol.py 生成。                                      */
/* -------------------------------------------------------------------------- */

/** @brief 逐字节比较两段数据。 */
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

/** @brief 打印一段字节，便于对不上时人工比对。 */
static void print_hex(const uint8_t *data, size_t length)
{
    for (size_t i = 0U; i < length; ++i)
    {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

/* ACK：SEQ=01, CMD=10, RESULT=00 —— 与 MCU通信协议.md 的示例完全一致 */
static const uint8_t k_exp_ack_ok[] = {0xA5, 0x5A, 0x02, 0x01, 0x01,
                                       0x7F, 0x04, 0x00, 0x10, 0x00,
                                       0x00, 0x00, 0x96, 0x12};

/* ACK 出错：SEQ=05, CMD=11, RESULT=03，FLAGS 应为 0x05（RESPONSE|ERROR） */
static const uint8_t k_exp_ack_err[] = {0xA5, 0x5A, 0x02, 0x05, 0x05,
                                        0x7F, 0x04, 0x00, 0x11, 0x03,
                                        0x00, 0x00, 0x54, 0xDD};

/* STATUS：SEQ=07，灯亮、亮度 82、前进 75%、故障位 8、运行 0x12345678 秒、错误 5 次 */
static const uint8_t k_exp_status[] = {0xA5, 0x5A, 0x02, 0x01, 0x07, 0x21, 0x10,
                                       0x00, 0x01, 0x52, 0x01, 0x01, 0x4B, 0x4B,
                                       0x08, 0x00, 0x78, 0x56, 0x34, 0x12, 0x05,
                                       0x00, 0x00, 0x00, 0x67, 0x70};

/** @brief 成功的 ACK 必须和协议文档里的示例报文逐字节一致。 */
static void test_encode_ack_ok(void)
{
    mcu_frame_t frame = {0};
    uint8_t out[MCU_PROTOCOL_MAX_FRAME_SIZE];
    size_t length;

    mcu_protocol_make_ack(&frame, 0x01U, MCU_PROTOCOL_CMD_SET_LIGHT,
                          MCU_PROTOCOL_RESULT_OK, 0x00U);
    length = mcu_protocol_encode(&frame, out, sizeof(out));

    check(length == sizeof(k_exp_ack_ok), "ack ok: frame is 14 bytes");
    check((frame.flags == MCU_PROTOCOL_FLAG_RESPONSE),
          "ack ok: flags is RESPONSE only");
    if (length != sizeof(k_exp_ack_ok))
    {
        print_hex(out, length);
        return;
    }
    check(bytes_equal(out, k_exp_ack_ok, length),
          "ack ok: matches protocol doc example");
    if (!bytes_equal(out, k_exp_ack_ok, length))
    {
        print_hex(out, length);
    }
}

/** @brief 结果码非 0 时，FLAGS 必须带上 ERROR 位。 */
static void test_encode_ack_error(void)
{
    mcu_frame_t frame = {0};
    uint8_t out[MCU_PROTOCOL_MAX_FRAME_SIZE];
    size_t length;

    mcu_protocol_make_ack(&frame, 0x05U, MCU_PROTOCOL_CMD_SET_MOTION,
                          MCU_PROTOCOL_RESULT_INVALID_VALUE, 0x00U);
    length = mcu_protocol_encode(&frame, out, sizeof(out));

    check(frame.flags == (MCU_PROTOCOL_FLAG_RESPONSE | MCU_PROTOCOL_FLAG_ERROR),
          "ack err: flags has RESPONSE and ERROR");
    check((length == sizeof(k_exp_ack_err)) &&
              bytes_equal(out, k_exp_ack_err, length),
          "ack err: matches reference bytes");
}

/** @brief STATUS 的 16 字节字段布局、字节序和整帧 CRC。 */
static void test_encode_status(void)
{
    mcu_frame_t frame = {0};
    uint8_t out[MCU_PROTOCOL_MAX_FRAME_SIZE];
    mcu_status_t status = {0};
    size_t length;

    status.light_enabled = 1U;
    status.brightness = 82U;
    status.motion = MCU_PROTOCOL_MOTION_FORWARD;
    status.target_motion = MCU_PROTOCOL_MOTION_FORWARD;
    status.speed = 75U;
    status.target_speed = 75U;
    status.fault_flags = 8U;
    status.status_flags = 0U;
    status.uptime_seconds = 0x12345678U;
    status.protocol_errors = 5U;

    mcu_protocol_make_status(&frame, 0x07U, &status);
    length = mcu_protocol_encode(&frame, out, sizeof(out));

    check(frame.payload_length == MCU_PROTOCOL_STATUS_PAYLOAD_SIZE,
          "status: payload is 16 bytes");
    check(length == sizeof(k_exp_status), "status: frame is 26 bytes");
    if (length != sizeof(k_exp_status))
    {
        print_hex(out, length);
        return;
    }
    check(bytes_equal(out, k_exp_status, length),
          "status: matches reference bytes");
    if (!bytes_equal(out, k_exp_status, length))
    {
        print_hex(out, length);
    }
}

/** @brief 参数无效或缓冲区不足时，encode 必须返回 0 而不是越界写。 */
static void test_encode_rejects_bad_arguments(void)
{
    mcu_frame_t frame = {0};
    uint8_t out[MCU_PROTOCOL_MAX_FRAME_SIZE];
    mcu_status_t status = {0};

    mcu_protocol_make_status(&frame, 0x01U, &status);

    check(mcu_protocol_encode(&frame, out, 25U) == 0U,
          "encode: rejects buffer that is one byte short");
    check(mcu_protocol_encode(&frame, out, sizeof(out)) == 26U,
          "encode: accepts an exactly big enough buffer");
    check(mcu_protocol_encode(NULL, out, sizeof(out)) == 0U,
          "encode: rejects NULL frame");
    check(mcu_protocol_encode(&frame, NULL, sizeof(out)) == 0U,
          "encode: rejects NULL output");
}

/** @brief 组出来的帧喂回解析器，字段应当原样还原。 */
static void test_encode_parse_round_trip(void)
{
    mcu_frame_t outgoing = {0};
    mcu_frame_t incoming = {0};
    mcu_status_t status = {0};
    uint8_t out[MCU_PROTOCOL_MAX_FRAME_SIZE];
    size_t length;

    status.light_enabled = 1U;
    status.brightness = 50U;
    status.uptime_seconds = 123U;

    mcu_protocol_make_status(&outgoing, 0x2AU, &status);
    length = mcu_protocol_encode(&outgoing, out, sizeof(out));
    check(length == 26U, "round-trip: encoded 26 bytes");

    mcu_protocol_reset();
    mcu_protocol_feed(out, length);

    check(mcu_protocol_pop_frame(&incoming), "round-trip: frame parsed back");
    check((incoming.seq == 0x2AU) &&
              (incoming.cmd == MCU_PROTOCOL_CMD_STATUS) &&
              (incoming.flags == MCU_PROTOCOL_FLAG_RESPONSE) &&
              (incoming.payload_length == MCU_PROTOCOL_STATUS_PAYLOAD_SIZE) &&
              (incoming.payload[1] == 50U) && (incoming.payload[8] == 123U),
          "round-trip: fields survive encode and parse");
}

int main(void)
{
    printf("--- CRC16/MODBUS ---\n");
    run_crc_tests();

    printf("\n--- frame parser ---\n");
    test_fragmented_frame();
    test_byte_by_byte();
    test_two_frames_one_chunk();
    test_queue_full_keeps_bytes();
    test_noise_prefix();
    test_bad_crc_resync();
    test_length_too_large();
    test_payload_contains_sof();
    test_version_not_filtered();

    printf("\n--- frame builder ---\n");
    test_encode_ack_ok();
    test_encode_ack_error();
    test_encode_status();
    test_encode_rejects_bad_arguments();
    test_encode_parse_round_trip();

    printf("\n%u passed, %u failed\n", (unsigned int)s_passed,
           (unsigned int)s_failed);
    return (s_failed == 0U) ? 0 : 1;
}
