/**
 * @file app_main.c
 * @brief 应用层实现：串口收发、协议解析与命令分派的接线。
 *
 * Android 模式：支持灯光开关/亮度、PING、状态查询和全电机 STOP。
 * 非 STOP 的宏观运动需要机构映射后才能启用。
 *
 * @note 调试日志继续走 USART3；USART1 只跑二进制协议，不再发文本。
 */

#include "app_main.h"

#include "bsp_common.h"
#include "bsp_led.h"
#include "bsp_light.h"
#include "bsp_linux_uart.h"
#include "bsp_log.h"
#include "bsp_motor.h"
#include "main.h"
#include "mcu_command.h"

/** @brief 单次接收的空闲/超时时间，单位毫秒。 */
#define APP_UART_RX_TIMEOUT_MS (1000U)

/** @brief 单次发送的超时时间，单位毫秒。 */
#define APP_UART_TX_TIMEOUT_MS (100U)

/** @brief 通信指示灯每次闪烁的持续时间，单位毫秒。 */
#define APP_COMM_LED_PULSE_MS (50U)

/** @brief 重新启动接收失败后，下一次重试前的等待时间，单位毫秒。 */
#define APP_UART_REARM_RETRY_MS (100U)

/* -------------------------------------------------------------------------- */
/* 内部状态                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief USART1 双接收缓冲，容量沿用 BSP 允许的最大值。
 *
 * 一块交给 UART 接收下一批字节，另一块留给主循环慢慢解析；
 * 解析过程中可能要多次发送回复，期间不能让新数据覆盖还没喂完的字节。
 */
static uint8_t s_rx_buffers[2][BSP_LINUX_UART_RX_MAX_SIZE];
/** @brief 当前交给 UART 接收的缓冲下标，0 或 1。 */
static uint8_t s_rx_index = 0U;
/** @brief 发送缓冲，最大一帧即可。 */
static uint8_t s_tx_buffer[MCU_PROTOCOL_MAX_FRAME_SIZE];
/** @brief 当前是否有一次接收正在进行。 */
static bool s_is_receive_started = false;
/** @brief 上一次启动接收是否失败；用于只在状态变化时打一次日志。 */
static bool s_is_rearm_failing = false;
/** @brief 启动接收失败后，允许下一次重试的时刻。 */
static uint32_t s_rearm_retry_ms = 0U;
/** @brief USART1 收发错误累计数，只用于日志排查。 */
static uint32_t s_uart_error_count = 0U;
/** @brief 灯光实际开关，bit0 有效。 */
static uint8_t s_light_enabled = 0U;
/** @brief 最近一次设定的亮度 0～100；关灯时保留，下次开灯继续用。 */
static uint8_t s_brightness = 0U;
/** @brief 通信指示灯是否正处于闪烁脉冲中。 */
static bool s_comm_led_is_on = false;
/** @brief 通信指示灯应当熄灭的时刻。 */
static uint32_t s_comm_led_off_ms = 0U;

/* -------------------------------------------------------------------------- */
/* 命令层的硬件回调                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief 回读当前实际状态。
 *
 * @note 灯光读的是本文件保存的实际状态；运动部分还没接底盘映射，固定回读
 *       停止和 0 速度。
 */
static void hw_read_status(mcu_status_t *status)
{
    status->light_enabled = s_light_enabled;
    status->brightness = s_brightness;
    status->motion = MCU_PROTOCOL_MOTION_STOP;
    status->target_motion = MCU_PROTOCOL_MOTION_STOP;
    status->speed = 0U;
    status->target_speed = 0U;
    status->fault_flags = 0U;
}

/**
 * @brief 应用灯光总开关和亮度。
 *
 * @note 七路灯板一起调：开灯时按亮度统一设定占空比，关灯时全部置 0，
 *       但保留亮度值，下次开灯直接沿用。
 */
static bool hw_apply_light(uint8_t light_flags, uint8_t brightness)
{
    uint16_t duty = 0U;

    s_light_enabled = (uint8_t)(light_flags & 0x01U);
    s_brightness = brightness;

    if (s_light_enabled != 0U)
    {
        duty =
            (uint16_t)(((uint32_t)brightness * BSP_LIGHT_PWM_DUTY_MAX) / 100U);
    }

    for (uint32_t i = 0U; i < (uint32_t)BSP_LIGHT_COUNT; ++i)
    {
        if (bsp_light_set_duty((bsp_light_id_t)i, duty) != BSP_OK)
        {
            /* 有任意一路失败就全部关掉，不留"只亮一半"的中间态。 */
            for (uint32_t j = 0U; j < (uint32_t)BSP_LIGHT_COUNT; ++j)
            {
                (void)bsp_light_set_duty((bsp_light_id_t)j, 0U);
            }
            s_light_enabled = 0U;
            return false;
        }
    }

    return true;
}

/**
 * @brief 应用一个宏观运动动作。
 *
 * @note 当前只接受 STOP，直接让八路电机滑行。速度参数等底盘动作映射定下来
 *       以后再使用。
 */
static bool hw_apply_motion(uint8_t motion, uint8_t speed)
{
    (void)speed;

    if (motion != MCU_PROTOCOL_MOTION_STOP)
    {
        return false;
    }
    return (bsp_motor_stop_all() == BSP_OK);
}

/** @brief 失联时的安全停止：立即停止全部运动输出，灯光保持。 */
static void hw_safe_stop(void)
{
    (void)bsp_motor_stop_all();
}

/** @brief 命令层使用的硬件回调表。 */
static const mcu_command_hw_t g_mcu_hw = {hw_read_status, hw_apply_light,
                                          hw_apply_motion, hw_safe_stop};

/* -------------------------------------------------------------------------- */
/* 内部工具                                                                    */
/* -------------------------------------------------------------------------- */

/** @brief 报告一个不可恢复的初始化错误。 */
static void fail_hard(void)
{
    (void)bsp_led_set(BSP_LED_ID_ERROR, true);
    Error_Handler();
}

/**
 * @brief 点亮通信指示灯，并安排一小段之后自动熄灭。
 *
 * @param[in] now_ms 当前时刻。
 *
 * @note 非阻塞：主循环不会在这里等待，靠 update_comm_led() 到点熄灭。
 */
static void pulse_comm_led(uint32_t now_ms)
{
    (void)bsp_led_set(BSP_LED_ID_COMM, true);
    s_comm_led_is_on = true;
    s_comm_led_off_ms = now_ms + APP_COMM_LED_PULSE_MS;
}

/** @brief 闪烁时间到就把通信指示灯熄灭。 */
static void update_comm_led(uint32_t now_ms)
{
    /* 用有符号差值比较，HAL_GetTick() 回绕（约 49.7 天）时判断依然正确。 */
    if (s_comm_led_is_on && ((int32_t)(now_ms - s_comm_led_off_ms) >= 0))
    {
        (void)bsp_led_set(BSP_LED_ID_COMM, false);
        s_comm_led_is_on = false;
    }
}

/**
 * @brief 在当前接收缓冲上启动一次 USART1 接收。
 *
 * @param[in] now_ms 当前时刻。
 *
 * @note 失败不进 Error_Handler：串口出错是现场常态（插拔线、上电毛刺），
 *       整机停死只能靠复位恢复。这里记录错误，APP_UART_REARM_RETRY_MS
 *       后由 app_run_once() 重试；一直收不到报文时由命令层的失联保护停电机。
 */
static void arm_receive(uint32_t now_ms)
{
    bsp_status_t status = bsp_linux_uart_start_receive(
        s_rx_buffers[s_rx_index], sizeof(s_rx_buffers[s_rx_index]),
        APP_UART_RX_TIMEOUT_MS);
    if (status != BSP_OK)
    {
        if (!s_is_rearm_failing)
        {
            LOG_WARN("APP", "uart1 rx start failed: %s",
                     bsp_status_str(status));
        }
        s_is_rearm_failing = true;
        s_rearm_retry_ms = now_ms + APP_UART_REARM_RETRY_MS;
        return;
    }
    if (s_is_rearm_failing)
    {
        LOG_INFO("APP", "uart1 rx restarted");
    }
    s_is_rearm_failing = false;
    s_is_receive_started = true;
}

/**
 * @brief 取出并处理解析器队列里的全部请求，需要时发送回复。
 *
 * @retval true 至少处理了一条请求。
 * @retval false 队列为空。
 *
 * @note 发送失败只计数和打日志，不停机：主机会因收不到 ACK 而重发。
 */
static bool handle_pending_frames(void)
{
    mcu_frame_t request;
    mcu_frame_t reply;
    bool has_handled = false;

    while (mcu_protocol_pop_frame(&request))
    {
        has_handled = true;

        /* 通信指示灯：每收到一条合法报文闪一下，链路一断就不闪了。 */
        pulse_comm_led(HAL_GetTick());

        /* DEBUG 级日志只在 Debug 构建里编译进去，正式跑不影响时序。 */
        LOG_DEBUG("PROTO", "rx seq=%u cmd=0x%02X len=%u",
                  (unsigned int)request.seq, (unsigned int)request.cmd,
                  (unsigned int)request.payload_length);

        if (mcu_command_handle(&request, &reply, HAL_GetTick()) == false)
        {
            continue;
        }

        size_t tx_length =
            mcu_protocol_encode(&reply, s_tx_buffer, sizeof(s_tx_buffer));
        if (tx_length == 0U)
        {
            continue;
        }
        LOG_DEBUG("PROTO", "tx seq=%u cmd=0x%02X len=%u",
                  (unsigned int)reply.seq, (unsigned int)reply.cmd,
                  (unsigned int)tx_length);
        bsp_status_t status = bsp_linux_uart_write(s_tx_buffer, tx_length,
                                                   APP_UART_TX_TIMEOUT_MS);
        if (status != BSP_OK)
        {
            ++s_uart_error_count;
            LOG_WARN("APP", "uart1 tx failed: %s (errors=%lu)",
                     bsp_status_str(status), (unsigned long)s_uart_error_count);
        }
    }
    return has_handled;
}

/**
 * @brief 把一批收到的字节全部喂给解析器，并处理切出的请求。
 *
 * @param[in] data 收到的字节。
 * @param[in] length 字节数。
 *
 * @note 解析队列容量有限，一批字节里报文多于队列容量时 feed() 会提前返回；
 *       这里先处理已切出的请求腾出队列，再从断点继续喂，直到全部喂完、
 *       缓冲里也没有剩下完整报文为止。
 */
static void process_received(const uint8_t *data, size_t length)
{
    size_t offset = 0U;
    bool has_progress = true;

    while (has_progress)
    {
        size_t consumed = mcu_protocol_feed(&data[offset], length - offset);
        offset += consumed;
        bool has_handled = handle_pending_frames();
        /* 既没取走字节也没处理请求就停下，避免任何意外情况下死循环。 */
        has_progress = (consumed > 0U) || has_handled;
    }
}

/* -------------------------------------------------------------------------- */
/* 对外接口                                                                    */
/* -------------------------------------------------------------------------- */

void app_init(void)
{
    if (bsp_light_init() != BSP_OK)
    {
        fail_hard();
    }
    if (bsp_motor_init() != BSP_OK)
    {
        fail_hard();
    }
    if (bsp_linux_uart_init() != BSP_OK)
    {
        fail_hard();
    }

    mcu_protocol_reset();
    mcu_command_init(&g_mcu_hw, HAL_GetTick());
    arm_receive(HAL_GetTick());
    LOG_INFO("APP", "protocol ready");
}

void app_run_once(void)
{
    uint32_t now_ms = HAL_GetTick();

    if (!s_is_receive_started && ((int32_t)(now_ms - s_rearm_retry_ms) >= 0))
    {
        arm_receive(now_ms);
    }

    if (s_is_receive_started)
    {
        size_t received_length = 0U;
        bsp_status_t status = bsp_linux_uart_receive_update(&received_length);
        if (status != BSP_BUSY)
        {
            s_is_receive_started = false;
            const uint8_t *received = s_rx_buffers[s_rx_index];
            s_rx_index ^= 1U;

            if ((status != BSP_OK) && (status != BSP_ERR_TIMEOUT))
            {
                /* 帧错误、噪声、溢出等：丢掉这一批，重新接收即可。 */
                ++s_uart_error_count;
                LOG_WARN("APP", "uart1 rx failed: %s (errors=%lu)",
                         bsp_status_str(status),
                         (unsigned long)s_uart_error_count);
            }

            /* 先在另一块缓冲上重新接收再处理：处理和回复期间不漏主机下一帧。 */
            arm_receive(now_ms);

            if ((status == BSP_OK) && (received_length > 0U))
            {
                process_received(received, received_length);
            }
        }
    }

    (void)mcu_command_poll(HAL_GetTick());
    update_comm_led(HAL_GetTick());
}
