/**
 * @file mcu_protocol.h
 * @brief 与 Android 主机通信的 MCU 协议模块：帧解析、组包与命令处理。
 *
 * 协议版本 0x02，一条报文的结构为：
 *
 *   A5 5A | VERSION | FLAGS | SEQ | CMD | LEN低 LEN高 | PAYLOAD | CRC低 CRC高
 *     帧头    版本      标志   序号  命令     数据长度       数据        校验
 *
 * 完整定义见 Linux/robot-sdk-source-0.3.0-complete/robot-sdk-source-0.3.0/
 * MCU通信协议.md。CRC 计算范围从 VERSION 开始到 PAYLOAD 最后一个字节，
 * 不含帧头 A5 5A，也不含 CRC 自身。
 *
 * 本模块只做协议逻辑，不直接操作 HAL：调用者用 mcu_protocol_feed() 把收到的
 * 字节喂进来，用 mcu_protocol_update() 让模块周期性地执行命令并发出回复。
 *
 * @note 当前进度：CRC16/MODBUS、帧解析和组包已完成；命令处理尚未开始。
 * @note 在接入硬件之前，本文件不包含任何 HAL 头，以便在 PC 上用 gcc 直接单测。
 */

#ifndef AITAID_MCU_PROTOCOL_H
#define AITAID_MCU_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief 单条报文 PAYLOAD 的最大字节数。 */
#define MCU_PROTOCOL_MAX_PAYLOAD (64U)

/** @brief 帧头第一字节，固定 0xA5。 */
#define MCU_PROTOCOL_SOF1 (0xA5U)

/** @brief 帧头第二字节，固定 0x5A。 */
#define MCU_PROTOCOL_SOF2 (0x5AU)

/** @brief 一帧中除 PAYLOAD 之外的固定字节数：帧头、版本、标志、序号、命令、长度、CRC。 */
#define MCU_PROTOCOL_FIXED_SIZE (10U)

/** @brief 一条完整报文的最大字节数。 */
#define MCU_PROTOCOL_MAX_FRAME_SIZE \
    (MCU_PROTOCOL_FIXED_SIZE + MCU_PROTOCOL_MAX_PAYLOAD)

/** @brief 已解析报文的排队深度。 */
#define MCU_PROTOCOL_FRAME_QUEUE_SIZE (4U)

/** @brief 本模块实现的协议版本。 */
#define MCU_PROTOCOL_VERSION (0x02U)

/** @brief FLAGS：该帧是响应。 */
#define MCU_PROTOCOL_FLAG_RESPONSE (0x01U)

/** @brief FLAGS：接收方必须返回 ACK。 */
#define MCU_PROTOCOL_FLAG_ACK_REQUIRED (0x02U)

/** @brief FLAGS：该响应表示错误。 */
#define MCU_PROTOCOL_FLAG_ERROR (0x04U)

/** @brief 命令字：链路检测。 */
#define MCU_PROTOCOL_CMD_PING (0x01U)

/** @brief 命令字：设置灯光。 */
#define MCU_PROTOCOL_CMD_SET_LIGHT (0x10U)

/** @brief 命令字：设置宏观运动。 */
#define MCU_PROTOCOL_CMD_SET_MOTION (0x11U)

/** @brief 命令字：查询状态。 */
#define MCU_PROTOCOL_CMD_GET_STATUS (0x20U)

/** @brief 命令字：状态响应。 */
#define MCU_PROTOCOL_CMD_STATUS (0x21U)

/** @brief 命令字：应答。 */
#define MCU_PROTOCOL_CMD_ACK (0x7FU)

/** @brief ACK 结果码：成功。 */
#define MCU_PROTOCOL_RESULT_OK (0x00U)

/** @brief ACK 结果码：未知命令。 */
#define MCU_PROTOCOL_RESULT_INVALID_COMMAND (0x01U)

/** @brief ACK 结果码：PAYLOAD 长度错误。 */
#define MCU_PROTOCOL_RESULT_INVALID_LENGTH (0x02U)

/** @brief ACK 结果码：参数值错误。 */
#define MCU_PROTOCOL_RESULT_INVALID_VALUE (0x03U)

/** @brief ACK 结果码：MCU 忙。 */
#define MCU_PROTOCOL_RESULT_BUSY (0x04U)

/** @brief ACK 结果码：硬件故障。 */
#define MCU_PROTOCOL_RESULT_HARDWARE (0x05U)

/** @brief ACK 结果码：MCU 内部错误。 */
#define MCU_PROTOCOL_RESULT_INTERNAL (0xFFU)

/** @brief 宏观运动动作：停止，速度为 0。 */
#define MCU_PROTOCOL_MOTION_STOP (0x00U)

/** @brief 宏观运动动作：前进。 */
#define MCU_PROTOCOL_MOTION_FORWARD (0x01U)

/** @brief 宏观运动动作：后退。 */
#define MCU_PROTOCOL_MOTION_BACKWARD (0x02U)

/** @brief 宏观运动动作：左转。 */
#define MCU_PROTOCOL_MOTION_TURN_LEFT (0x03U)

/** @brief 宏观运动动作：右转。 */
#define MCU_PROTOCOL_MOTION_TURN_RIGHT (0x04U)

/** @brief ACK 报文的 PAYLOAD 字节数，固定 4。 */
#define MCU_PROTOCOL_ACK_PAYLOAD_SIZE (4U)

/** @brief STATUS 报文的 PAYLOAD 字节数，固定 16。 */
#define MCU_PROTOCOL_STATUS_PAYLOAD_SIZE (16U)

/**
 * @brief STATUS 报文要回读的 16 字节状态。
 *
 * @note 本结构只描述字段含义；写入 PAYLOAD 时按字段逐个展开成小端字节，
 *       不做整体 memcpy，避免依赖编译器的结构体对齐和填充。
 */
typedef struct
{
    uint8_t light_enabled;    /**< 偏移 0：实际灯光开关，bit0 有效。 */
    uint8_t brightness;       /**< 偏移 1：实际亮度 0～100。 */
    uint8_t motion;           /**< 偏移 2：当前实际运动动作。 */
    uint8_t target_motion;    /**< 偏移 3：最近接受的目标动作。 */
    uint8_t speed;            /**< 偏移 4：当前实际速度 0～100。 */
    uint8_t target_speed;     /**< 偏移 5：目标速度 0～100。 */
    uint8_t fault_flags;      /**< 偏移 6：故障标志，0 表示无故障。 */
    uint8_t status_flags;     /**< 偏移 7：状态扩展位，当前写 0。 */
    uint32_t uptime_seconds;  /**< 偏移 8～11：MCU 运行秒数，小端。 */
    uint16_t protocol_errors; /**< 偏移 12～13：协议错误累计数，小端。 */
} mcu_status_t;

/**
 * @brief 一条已解析出来的完整报文。
 *
 * @note 解析层只保证"字节完整、帧头与 CRC 正确"，不判断 version、cmd 和参数
 *       是否合法；这些留给命令层（协议第 8 节第 6 步）。
 */
typedef struct
{
    uint8_t version;        /**< 协议版本，正常为 0x02。 */
    uint8_t flags;          /**< 标志位：bit0 响应、bit1 要求 ACK、bit2 错误。 */
    uint8_t seq;            /**< 序号；响应必须沿用请求的序号。 */
    uint8_t cmd;            /**< 命令字。 */
    uint8_t payload_length; /**< PAYLOAD 实际字节数，0～MCU_PROTOCOL_MAX_PAYLOAD。 */
    uint8_t payload[MCU_PROTOCOL_MAX_PAYLOAD]; /**< PAYLOAD 内容。 */
} mcu_frame_t;

/**
 * @brief 计算 CRC16/MODBUS 校验值。
 *
 * 参数：初值 0xFFFF，反射多项式 0xA001，结果不异或、不反转。
 *
 * @param[in] data 待校验数据首地址，不允许为 NULL；length 为 0 时返回初值 0xFFFF。
 * @param[in] length 待校验字节数。
 *
 * @return 16 位校验值；调用者需自行按小端拆成两个字节发送（低字节先）。
 *
 * @note 计算范围是帧的 VERSION 到 PAYLOAD 最后一个字节，不含帧头，也不含 CRC。
 * @note 纯函数，无全局状态，可在 PC 上直接调用测试。
 */
uint16_t mcu_protocol_crc16(const uint8_t *data, size_t length);

/**
 * @brief 清空解析器的全部内部状态：候选缓冲、报文队列和错误计数。
 *
 * @note 用于初始化，以及 PC 端测试在每个用例开始前复位。
 */
void mcu_protocol_reset(void);

/**
 * @brief 把主循环收到的原始字节喂给解析器，累积并切出完整报文。
 *
 * @param[in] data 收到的字节首地址；data 为 NULL 或 length 为 0 时不取新字节，
 *                 只尝试切出缓冲中已经完整的报文。
 * @param[in] length 本次收到的字节数，可以是任意大小。
 *
 * @return 实际取走的字节数，0～length。小于 length 说明报文队列已满，
 *         调用者应先用 mcu_protocol_pop_frame() 取走报文，再从
 *         data + 返回值 处继续喂剩下的字节；未取走的字节不会被丢弃或计错。
 *
 * @note 一次收到的数据可能是半条报文、恰好一条，或者好几条粘在一起，
 *       也可能前面带着噪声；解析器必须跨调用保留状态。
 * @note 一次 length 可能比内部缓冲还长，实现要能分段处理，不能假设装得下。
 * @note 本函数只负责切帧，不执行命令、不发送任何数据。
 * @warning 不在中断里调用；由主循环串行调用，不可重入。
 */
size_t mcu_protocol_feed(const uint8_t *data, size_t length);

/**
 * @brief 取出队列中最早的一条完整报文。
 *
 * @param[out] frame 输出报文，不允许为 NULL。
 *
 * @retval true 已取出一条报文，frame 已填好。
 * @retval false 当前没有待处理的报文，frame 内容未定义。
 *
 * @note 先进先出；取出后该条不再重复返回。
 */
bool mcu_protocol_pop_frame(mcu_frame_t *frame);

/**
 * @brief 返回累计的协议错误数，供 STATUS 上报。
 *
 * @return 自上次 reset 以来被丢弃的非法帧数量；无错误时为 0。
 *
 * @note 计什么由实现决定，但至少要包含 CRC 校验失败和长度非法的帧。
 */
uint16_t mcu_protocol_error_count(void);

/**
 * @brief 构造一条 ACK 应答报文。
 *
 * @param[out] frame 输出的报文，不允许为 NULL。
 * @param[in] seq 沿用请求的序号。
 * @param[in] acked_cmd 被确认的命令字。
 * @param[in] result 结果码，见 MCU_PROTOCOL_RESULT_*；非 0 时自动置 ERROR 标志。
 * @param[in] detail 命令相关错误详情，成功时写 0。
 *
 * @note FLAGS 恒含 RESPONSE；PAYLOAD 固定 4 字节：命令字、结果码、详情、保留 0。
 */
void mcu_protocol_make_ack(mcu_frame_t *frame, uint8_t seq, uint8_t acked_cmd,
                           uint8_t result, uint8_t detail);

/**
 * @brief 构造一条 STATUS 状态应答报文。
 *
 * @param[out] frame 输出的报文，不允许为 NULL。
 * @param[in] seq 沿用请求的序号。
 * @param[in] status 要回读的状态，不允许为 NULL。
 *
 * @note FLAGS 恒含 RESPONSE；PAYLOAD 固定 16 字节，多字节整数按小端展开。
 */
void mcu_protocol_make_status(mcu_frame_t *frame, uint8_t seq,
                              const mcu_status_t *status);

/**
 * @brief 把一条报文编码成线路上要发送的字节。
 *
 * @param[in] frame 待编码的报文，不允许为 NULL。
 * @param[out] out 输出缓冲区，不允许为 NULL，容量至少为返回的字节数。
 * @param[in] out_size out 的容量，单位为字节。
 *
 * @return 写入的字节数，等于 10 + payload_length；参数无效或缓冲区不足时返回 0。
 *
 * @note 输出内容为完整的 帧头 + 头 + PAYLOAD + CRC，可直接交给 UART 发送。
 * @note 不做任何合法性检查：version、flags、cmd 按 frame 中的值原样编码。
 */
size_t mcu_protocol_encode(const mcu_frame_t *frame, uint8_t *out,
                           size_t out_size);

#endif /* AITAID_MCU_PROTOCOL_H */
