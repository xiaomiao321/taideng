/**
 * @file mcu_protocol.c
 * @brief MCU 协议模块实现：CRC16/MODBUS 与帧解析。
 *
 * 帧格式（协议版本 0x02）：
 *   A5 5A | VERSION | FLAGS | SEQ | CMD | LEN低 LEN高 | PAYLOAD | CRC低 CRC高
 *
 * feed() 把主循环收到的字节累积成完整报文放进队列，pop_frame() 把报文交给
 * 上层。解析层只保证"字节完整、帧头与 CRC 正确"，版本号、命令字和参数是否
 * 合法由命令层判断。
 *
 * @note 本文件在接入硬件之前不包含任何 HAL 头，保证能直接在 PC 上编译单测。
 */

#include "mcu_protocol.h"

#include <string.h>

/* -------------------------------------------------------------------------- */
/* 内部状态 */
/* -------------------------------------------------------------------------- */

/* 这些状态必须跨 feed() 调用保留，所以放在文件作用域，不能是函数局部变量。 */

/** @brief 候选帧缓冲，容量刚好是一帧的最大长度。 */
static uint8_t s_buffer[MCU_PROTOCOL_MAX_FRAME_SIZE];
/** @brief 缓冲中当前有效的字节数。 */
static size_t s_used = 0U;
/** @brief 已解析报文的环形队列。 */
static mcu_frame_t s_queue[MCU_PROTOCOL_FRAME_QUEUE_SIZE];
/** @brief 队列读出位置。 */
static uint8_t s_queue_head = 0U;
/** @brief 队列中当前的报文数。 */
static uint8_t s_queue_count = 0U;
/** @brief 协议错误累计数，供 STATUS 上报。 */
static uint16_t s_error_count = 0U;

/* -------------------------------------------------------------------------- */
/* CRC16/MODBUS */
/* -------------------------------------------------------------------------- */

uint16_t mcu_protocol_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x0001U)
            {
                crc = (crc >> 1) ^ 0xA001U;
            }
            else
            {
                crc = crc >> 1;
            }
        }
    }

    return crc;
}

/* -------------------------------------------------------------------------- */
/* 内部工具 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 丢弃候选缓冲里最早的一个字节，用于重新寻找帧头。
 *
 * @note 校验失败时只能丢一个字节：真正的帧头可能就藏在刚丢掉的那串坏数据里，
 *       整段清空会把紧随其后的好帧一起丢掉。
 */
static void drop_leading_byte(void)
{
    if (s_used > 0U)
    {
        memmove(s_buffer, &s_buffer[1], s_used - 1U);
        --s_used;
    }
}

/**
 * @brief 从候选缓冲最前面尽可能切出完整报文，放入队列。
 *
 * @note 数据不足时原样保留，等下一次 feed() 补齐；队列满时停止切帧。
 * @note 每次只判断缓冲最前面是不是一条完整报文，切走后再看下一条。
 */
static void try_extract_frames(void)
{
    while (s_queue_count < MCU_PROTOCOL_FRAME_QUEUE_SIZE)
    {
        if (s_used < 2U)
        {
            return; /* 连帧头都还不齐 */
        }
        if ((s_buffer[0] != MCU_PROTOCOL_SOF1) ||
            (s_buffer[1] != MCU_PROTOCOL_SOF2))
        {
            drop_leading_byte(); /* 不是帧头，滑窗一格继续找 */
            continue;
        }
        if (s_used < 8U)
        {
            return; /* 还没收到长度字段 */
        }

        uint16_t payload_length =
            (uint16_t)s_buffer[6] | ((uint16_t)s_buffer[7] << 8);
        if (payload_length > MCU_PROTOCOL_MAX_PAYLOAD)
        {
            ++s_error_count;
            drop_leading_byte(); /* 长度非法，重新同步 */
            continue;
        }

        size_t frame_size = MCU_PROTOCOL_FIXED_SIZE + (size_t)payload_length;
        if (s_used < frame_size)
        {
            return; /* 报文还不完整，等剩下的字节 */
        }

        /* CRC 覆盖 VERSION 到 PAYLOAD 末字节，即除帧头和 CRC 自身的部分。 */
        uint16_t received_crc = (uint16_t)s_buffer[frame_size - 2U] |
                                ((uint16_t)s_buffer[frame_size - 1U] << 8);
        uint16_t computed_crc =
            mcu_protocol_crc16(&s_buffer[2], frame_size - 4U);
        if (computed_crc != received_crc)
        {
            ++s_error_count;
            drop_leading_byte(); /* CRC 不符，重新同步 */
            continue;
        }

        uint8_t index = (uint8_t)((s_queue_head + s_queue_count) %
                                  MCU_PROTOCOL_FRAME_QUEUE_SIZE);
        s_queue[index].version = s_buffer[2];
        s_queue[index].flags = s_buffer[3];
        s_queue[index].seq = s_buffer[4];
        s_queue[index].cmd = s_buffer[5];
        s_queue[index].payload_length = (uint8_t)payload_length;
        memcpy(s_queue[index].payload, &s_buffer[8], payload_length);
        ++s_queue_count;

        /* 把这一帧从缓冲里移走，剩余字节前移，继续找下一帧。 */
        memmove(s_buffer, &s_buffer[frame_size], s_used - frame_size);
        s_used -= frame_size;
    }
}

/* -------------------------------------------------------------------------- */
/* 对外接口 */
/* -------------------------------------------------------------------------- */

void mcu_protocol_reset(void)
{
    s_used = 0U;
    s_queue_head = 0U;
    s_queue_count = 0U;
    s_error_count = 0U;
}

size_t mcu_protocol_feed(const uint8_t *data, size_t length)
{
    /* 上次因队列满而留在缓冲里的完整报文，在调用者取走报文后先切出来。 */
    try_extract_frames();

    if ((data == NULL) || (length == 0U))
    {
        return 0U;
    }

    size_t offset = 0U;
    while (offset < length)
    {
        /* 一次收到的字节可能比缓冲还长，所以分段装填。 */
        size_t space = MCU_PROTOCOL_MAX_FRAME_SIZE - s_used;
        if (space == 0U)
        {
            /* 队列未满时缓冲里不可能剩下一整帧还切不出来，所以缓冲满只会是
             * 队列满了。此时不能丢字节，停下来让调用者先取走报文。 */
            break;
        }

        size_t chunk = length - offset;
        if (chunk > space)
        {
            chunk = space;
        }
        memcpy(&s_buffer[s_used], &data[offset], chunk);
        s_used += chunk;
        offset += chunk;

        try_extract_frames();
    }
    return offset;
}

bool mcu_protocol_pop_frame(mcu_frame_t *frame)
{
    if ((frame == NULL) || (s_queue_count == 0U))
    {
        return false;
    }

    *frame = s_queue[s_queue_head];
    s_queue_head =
        (uint8_t)((s_queue_head + 1U) % MCU_PROTOCOL_FRAME_QUEUE_SIZE);
    --s_queue_count;
    return true;
}

uint16_t mcu_protocol_error_count(void)
{
    return s_error_count;
}

/* -------------------------------------------------------------------------- */
/* 组包                                                                        */
/* -------------------------------------------------------------------------- */

void mcu_protocol_make_ack(mcu_frame_t *frame, uint8_t seq, uint8_t acked_cmd,
                           uint8_t result, uint8_t detail)
{
    if (frame == NULL)
    {
        return;
    }

    frame->version = MCU_PROTOCOL_VERSION;
    frame->flags = MCU_PROTOCOL_FLAG_RESPONSE;
    if (result != MCU_PROTOCOL_RESULT_OK)
    {
        frame->flags |= MCU_PROTOCOL_FLAG_ERROR;
    }
    frame->seq = seq;
    frame->cmd = MCU_PROTOCOL_CMD_ACK;
    frame->payload_length = MCU_PROTOCOL_ACK_PAYLOAD_SIZE;
    frame->payload[0] = acked_cmd;
    frame->payload[1] = result;
    frame->payload[2] = detail;
    frame->payload[3] = 0U;
}

void mcu_protocol_make_status(mcu_frame_t *frame, uint8_t seq,
                              const mcu_status_t *status)
{
    if ((frame == NULL) || (status == NULL))
    {
        return;
    }

    frame->version = MCU_PROTOCOL_VERSION;
    frame->flags = MCU_PROTOCOL_FLAG_RESPONSE;
    frame->seq = seq;
    frame->cmd = MCU_PROTOCOL_CMD_STATUS;
    frame->payload_length = MCU_PROTOCOL_STATUS_PAYLOAD_SIZE;

    frame->payload[0] = (uint8_t)(status->light_enabled & 0x01U);
    frame->payload[1] = status->brightness;
    frame->payload[2] = status->motion;
    frame->payload[3] = status->target_motion;
    frame->payload[4] = status->speed;
    frame->payload[5] = status->target_speed;
    frame->payload[6] = status->fault_flags;
    frame->payload[7] = status->status_flags;

    /* 多字节整数一律小端展开，不依赖结构体在内存里的排布。 */
    frame->payload[8] = (uint8_t)(status->uptime_seconds & 0xFFU);
    frame->payload[9] = (uint8_t)((status->uptime_seconds >> 8) & 0xFFU);
    frame->payload[10] = (uint8_t)((status->uptime_seconds >> 16) & 0xFFU);
    frame->payload[11] = (uint8_t)((status->uptime_seconds >> 24) & 0xFFU);
    frame->payload[12] = (uint8_t)(status->protocol_errors & 0xFFU);
    frame->payload[13] = (uint8_t)((status->protocol_errors >> 8) & 0xFFU);
    frame->payload[14] = 0U;
    frame->payload[15] = 0U;
}

size_t mcu_protocol_encode(const mcu_frame_t *frame, uint8_t *out,
                           size_t out_size)
{
    if ((frame == NULL) || (out == NULL) ||
        (frame->payload_length > MCU_PROTOCOL_MAX_PAYLOAD))
    {
        return 0U;
    }

    size_t frame_size = MCU_PROTOCOL_FIXED_SIZE + (size_t)frame->payload_length;
    if (out_size < frame_size)
    {
        return 0U;
    }

    out[0] = MCU_PROTOCOL_SOF1;
    out[1] = MCU_PROTOCOL_SOF2;
    out[2] = frame->version;
    out[3] = frame->flags;
    out[4] = frame->seq;
    out[5] = frame->cmd;
    out[6] = (uint8_t)(frame->payload_length & 0xFFU);
    out[7] = (uint8_t)((frame->payload_length >> 8) & 0xFFU);
    memcpy(&out[8], frame->payload, frame->payload_length);

    /* CRC 覆盖 VERSION 到 PAYLOAD 末字节，即帧头之后、CRC 之前的部分。 */
    uint16_t crc = mcu_protocol_crc16(&out[2], 6U + (size_t)frame->payload_length);
    out[frame_size - 2U] = (uint8_t)(crc & 0xFFU);
    out[frame_size - 1U] = (uint8_t)((crc >> 8) & 0xFFU);

    return frame_size;
}
