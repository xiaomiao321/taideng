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

/* -------------------------------------------------------------------------- */
/* 内部状态                                                                    */
/* -------------------------------------------------------------------------- */

/** @brief USART1 接收缓冲，容量沿用 BSP 允许的最大值。 */
static uint8_t s_rx_buffer[BSP_LINUX_UART_RX_MAX_SIZE];
/** @brief 发送缓冲，最大一帧即可。 */
static uint8_t s_tx_buffer[MCU_PROTOCOL_MAX_FRAME_SIZE];
/** @brief 当前是否有一次接收正在进行。 */
static bool s_is_receive_started = false;
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
        duty = (uint16_t)(((uint32_t)brightness * BSP_LIGHT_PWM_DUTY_MAX) / 100U);
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

/** @brief 报告一个不可恢复的初始化或收发错误。 */
static void fail_hard(void)
{
    (void)bsp_led_set(BSP_LED_ID_ERROR, true);
    Error_Handler();
}

/** @brief 启动一次 USART1 接收。 */
static void arm_receive(void)
{
    if (bsp_linux_uart_start_receive(s_rx_buffer, sizeof(s_rx_buffer),
                                     APP_UART_RX_TIMEOUT_MS) != BSP_OK)
    {
        fail_hard();
    }
    s_is_receive_started = true;
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
    arm_receive();
    LOG_INFO("APP", "protocol ready");
}

void app_run_once(void)
{
    size_t received_length = 0U;
    mcu_frame_t request;
    mcu_frame_t reply;

    if (!s_is_receive_started)
    {
        arm_receive();
    }

    bsp_status_t status = bsp_linux_uart_receive_update(&received_length);
    if (status != BSP_BUSY)
    {
        s_is_receive_started = false;
        if ((status != BSP_OK) && (status != BSP_ERR_TIMEOUT))
        {
            fail_hard();
        }
        if ((status == BSP_OK) && (received_length > 0U))
        {
            mcu_protocol_feed(s_rx_buffer, received_length);
        }
        /* 先重新武装接收再发送回复：发送期间不会漏掉主机下一帧的开头。 */
        arm_receive();
    }

    while (mcu_protocol_pop_frame(&request))
    {
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
        if (tx_length > 0U)
        {
            LOG_DEBUG("PROTO", "tx seq=%u cmd=0x%02X len=%u",
                      (unsigned int)reply.seq, (unsigned int)reply.cmd,
                      (unsigned int)tx_length);
            if (bsp_linux_uart_write(s_tx_buffer, tx_length,
                                     APP_UART_TX_TIMEOUT_MS) != BSP_OK)
            {
                fail_hard();
            }
        }
    }

    (void)mcu_command_poll(HAL_GetTick());
    update_comm_led(HAL_GetTick());
}
