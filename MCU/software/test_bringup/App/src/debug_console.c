/**
 * @file debug_console.c
 * @brief 调试终端：USART3 上的文本命令与周期状态上报，接到实际硬件。
 *
 * 命令解析和安全规则在 debug_control，全板测试在 debug_board，关节闭环在
 * joint_control；本文件只负责把它们的回调接到 BSP，并按行收发文本。
 * USART1 可切换为透传模式（LMODE 1），把 Linux 串口的字节转发给上位机。
 */

#include "debug_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp_adc.h"
#include "bsp_buzzer.h"
#include "bsp_debug_uart.h"
#include "bsp_key.h"
#include "bsp_led.h"
#include "bsp_light.h"
#include "bsp_linux_power.h"
#include "bsp_linux_uart.h"
#include "bsp_motor.h"
#include "bsp_param_flash.h"
#include "debug_board.h"
#include "debug_control.h"
#include "joint_control.h"
#include "main.h"

_Static_assert(JOINT_COUNT == BSP_ADC_CHANNEL_COUNT,
               "each joint needs one potentiometer channel");
_Static_assert(DEBUG_BOARD_LAMP_COUNT == BSP_LIGHT_COUNT,
               "board test lamp count must match the light driver");
_Static_assert(DEBUG_BOARD_LED_COUNT == BSP_LED_ID_COUNT,
               "board test LED count must match the LED driver");

/** @brief 文本发送超时；断开的上位机不能拖住控制循环，单位毫秒。 */
#define CONSOLE_TX_TIMEOUT_MS (15U)

/** @brief 每次 update 最多处理的接收字节数，限制单次循环耗时。 */
#define CONSOLE_RX_BUDGET (128U)

/** @brief 电位器采样周期，单位毫秒。 */
#define ADC_PERIOD_MS (10U)

/** @brief 单次六路 ADC 扫描的等待超时，单位毫秒。 */
#define ADC_TIMEOUT_MS (5U)

/** @brief 每个采样周期连续扫描的次数，合成一组读数以压低 PWM 干扰。 */
#define ADC_OVERSAMPLE (16U)

/** @brief 状态行上报周期，单位毫秒。 */
#define STATUS_PERIOD_MS (100U)

/** @brief 一行 LRX 最多携带的透传字节数。 */
#define LINUX_RX_BYTES_PER_LINE (32U)

/** @brief LTX 一次最多发送的字节数。 */
#define LINUX_TX_MAX_BYTES (48U)

/** @brief LTX 发送超时，单位毫秒。 */
#define LINUX_TX_TIMEOUT_MS (10U)

/** @brief 状态行的最大长度。 */
#define STATUS_LINE_MAX (192U)

/* -------------------------------------------------------------------------- */
/* 内部状态                                                                    */
/* -------------------------------------------------------------------------- */

static bool s_is_initialized = false;
static bool s_is_adc_ok = false;
static bool s_is_light_ok = false;
static bool s_is_buzzer_ok = false;
static bool s_is_power_ok = false;
static bool s_is_linux_ok = false;
/** @brief 标定参数的 Flash 存储是否可用。 */
static bool s_is_cal_store_ok = false;

static debug_control_t s_control;
static debug_board_t s_board;
static joint_control_t s_joints;
/** @brief JGET/CAL/JMOVE 最近操作的关节，周期 JSTATE 报告它。 */
static unsigned s_joint_focus = 0U;

/** @brief 正在拼接的命令行。 */
static char s_line[DEBUG_CONTROL_LINE_MAX];
static unsigned s_line_length = 0U;
/** @brief 当前行已出错，丢弃到下一个换行为止。 */
static bool s_is_discarding = false;
/** @brief 已处理过的 USART3 接收错误和溢出计数，用于发现新事件。 */
static uint32_t s_seen_rx_errors = 0U;
static uint32_t s_seen_rx_overflows = 0U;
/** @brief USART3 发送失败次数，仅供调试器查看。 */
static uint32_t s_tx_error_count = 0U;

/** @brief 已处理过的 USART1 透传错误和溢出计数。 */
static uint32_t s_seen_linux_errors = 0U;
static uint32_t s_seen_linux_overflows = 0U;

static uint16_t s_pot_cache[BSP_ADC_CHANNEL_COUNT];
/** @brief 最近一次采样中每路样本的极差。 */
static uint16_t s_pot_spread[BSP_ADC_CHANNEL_COUNT];
static bool s_is_pot_valid = false;
/** @brief JTRACE 打开时每次采样上报一行 JT。 */
static bool s_is_trace_on = false;
/** @brief 最近一次已上报的采样时刻，用来发现新采样。 */
static uint32_t s_trace_sample_ms = 0U;
static uint32_t s_last_adc_ms = 0U;
static uint32_t s_last_status_ms = 0U;

/** @brief LSET 设定的各路灯板占空比，用于 LSTATE 上报。 */
static unsigned s_lamp_duty[BSP_LIGHT_COUNT];

static bool s_is_home_pressed = false;
static uint32_t s_last_home_count = 0U;

/* -------------------------------------------------------------------------- */
/* 输出                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 发送一行文本。
 *
 * @retval false 发送失败（只计数）：发送失败不说明上位机断开，
 *         真正的失联由心跳超时负责停机。
 */
static bool send_line(const char *text)
{
    if (bsp_debug_uart_write((const uint8_t *)text, strlen(text),
                             CONSOLE_TX_TIMEOUT_MS) != BSP_OK)
    {
        ++s_tx_error_count;
        return false;
    }
    return true;
}

/** @brief 供 reply 回调使用的无返回值版本。 */
static void reply_line(const char *text)
{
    (void)send_line(text);
}

/** @brief 往 out 末尾追加格式化文本，空间不足时截断。 */
__attribute__((format(printf, 4, 5))) static void
append(char *out, size_t size, size_t *used, const char *format, ...)
{
    if (*used >= size)
    {
        return;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(&out[*used], size - *used, format, args);
    va_end(args);
    if (written > 0)
    {
        *used += (size_t)written;
    }
}

/* -------------------------------------------------------------------------- */
/* 硬件回调                                                                    */
/* -------------------------------------------------------------------------- */

/** @brief 关闭电机、指示灯、灯板、蜂鸣器和 Linux 电源脉冲。 */
static bool outputs_off(void)
{
    bool ok = bsp_motor_stop_all() == BSP_OK;
    for (unsigned i = 0; i < BSP_LED_ID_COUNT; ++i)
    {
        if (bsp_led_set((bsp_led_id_t)i, false) != BSP_OK)
        {
            ok = false;
        }
    }
    if (s_is_light_ok)
    {
        for (unsigned i = 0; i < BSP_LIGHT_COUNT; ++i)
        {
            if (bsp_light_stop((bsp_light_id_t)i) != BSP_OK)
            {
                ok = false;
            }
        }
    }
    memset(s_lamp_duty, 0, sizeof(s_lamp_duty));
    if (s_is_buzzer_ok && bsp_buzzer_stop() != BSP_OK)
    {
        ok = false;
    }
    if (s_is_power_ok && bsp_linux_power_release() != BSP_OK)
    {
        ok = false;
    }
    return ok;
}

/** @brief 全部停止：关透传、取消关节运动和全板测试、关闭全部输出。 */
static bool stop_all(void)
{
    if (s_is_linux_ok)
    {
        (void)bsp_linux_uart_stream_stop();
    }
    bool ok = joint_cancel(&s_joints);
    debug_board_cancel(&s_board);
    return outputs_off() && ok;
}

static bool board_apply(board_kind_t kind, unsigned index, int value)
{
    switch (kind)
    {
        case BOARD_LOG:
            return send_line("LOGTEST BRINGUP log ok\r\n");
        case BOARD_LED:
            return bsp_led_set((bsp_led_id_t)index, true) == BSP_OK;
        case BOARD_KEY:
            return true;
        case BOARD_BUZZER:
            return s_is_buzzer_ok &&
                   bsp_buzzer_start((uint32_t)value) == BSP_OK;
        case BOARD_PWR:
            return s_is_power_ok &&
                   bsp_linux_power_start(s_board.duration_ms) == BSP_OK;
        case BOARD_ADC:
        {
            uint16_t values[BSP_ADC_CHANNEL_COUNT];
            return s_is_adc_ok &&
                   bsp_adc_read_all(values, ADC_TIMEOUT_MS) == BSP_OK;
        }
        case BOARD_LAMP:
            return s_is_light_ok &&
                   bsp_light_set_duty((bsp_light_id_t)index, (uint16_t)value) ==
                       BSP_OK;
        case BOARD_MOTOR:
            return bsp_motor_set_duty((bsp_motor_id_t)index, (int16_t)value) ==
                   BSP_OK;
        default:
            return false;
    }
}

static const char *start_test(unsigned seq, const char *name, int value,
                              uint32_t duration)
{
    return debug_board_start(&s_board, seq, name, value, duration,
                             HAL_GetTick());
}

static const char *confirm_test(bool pass)
{
    return debug_board_confirm(&s_board, pass);
}

static bool run_motor(unsigned index, int duty)
{
    return bsp_motor_set_duty((bsp_motor_id_t)index, (int16_t)duty) == BSP_OK;
}

/** @brief 多路手动控制接管电机前，取消全板测试和关节运动。 */
static bool manual_control(void)
{
    if (s_board.phase != BOARD_IDLE)
    {
        debug_board_cancel(&s_board);
    }
    return s_joints.active < 0 || joint_cancel(&s_joints);
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f')
    {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F')
    {
        return ch - 'A' + 10;
    }
    return -1;
}

/** @brief LSET：一次设定全部灯板通道的占空比。 */
static const char *set_lamps(const char *const *args, unsigned count)
{
    unsigned values[BSP_LIGHT_COUNT];
    if (count != BSP_LIGHT_COUNT || !s_is_light_ok)
    {
        return "LIGHT_PARAM";
    }
    for (unsigned i = 0; i < BSP_LIGHT_COUNT; ++i)
    {
        unsigned value = 0;
        /* 最多 4 位数字，足够表示 0～BSP_LIGHT_PWM_DUTY_MAX。 */
        if (!*args[i] || strlen(args[i]) > 4U)
        {
            return "LIGHT_PARAM";
        }
        for (const char *p = args[i]; *p; ++p)
        {
            if (*p < '0' || *p > '9')
            {
                return "LIGHT_PARAM";
            }
            value = value * 10U + (unsigned)(*p - '0');
        }
        if (value > BSP_LIGHT_PWM_DUTY_MAX)
        {
            return "LIGHT_PARAM";
        }
        values[i] = value;
    }
    if (s_board.phase != BOARD_IDLE)
    {
        return "BOARD_BUSY";
    }
    for (unsigned i = 0; i < BSP_LIGHT_COUNT; ++i)
    {
        if (bsp_light_set_duty((bsp_light_id_t)i, (uint16_t)values[i]) !=
            BSP_OK)
        {
            debug_control_fault(&s_control, "EVENT HARDWARE\r\n");
            return "HARDWARE";
        }
        s_lamp_duty[i] = values[i];
    }
    return NULL;
}

/** @brief LMODE：开关 USART1 透传。 */
static const char *set_linux_mode(const char *const *args, unsigned count)
{
    if (count != 1 || (strcmp(args[0], "0") && strcmp(args[0], "1")))
    {
        return "PARAM";
    }
    if (bsp_linux_uart_stream_stop() != BSP_OK)
    {
        return "LINUX_RX_ERROR";
    }
    if (args[0][0] == '1')
    {
        if (bsp_linux_uart_stream_start() != BSP_OK)
        {
            return "LINUX_RX_ERROR";
        }
        s_seen_linux_errors = bsp_linux_uart_stream_error_count();
        s_seen_linux_overflows = bsp_linux_uart_stream_overflow_count();
    }
    return NULL;
}

/** @brief LTX：把十六进制参数解码后从 USART1 发出。 */
static const char *send_linux_hex(const char *const *args, unsigned count)
{
    uint8_t data[LINUX_TX_MAX_BYTES];
    if (!bsp_linux_uart_stream_is_active())
    {
        return "BRIDGE_OFF";
    }
    size_t length = (count == 1) ? strlen(args[0]) : 0U;
    if (length == 0U || length > sizeof(data) * 2U || (length % 2U) != 0U)
    {
        return "HEX_PARAM";
    }
    size_t size = length / 2U;
    for (size_t i = 0; i < size; ++i)
    {
        int high = hex_digit(args[0][i * 2U]);
        int low = hex_digit(args[0][i * 2U + 1U]);
        if (high < 0 || low < 0)
        {
            return "HEX_PARAM";
        }
        data[i] = (uint8_t)((high << 4) | low);
    }
    return (bsp_linux_uart_write(data, size, LINUX_TX_TIMEOUT_MS) == BSP_OK)
               ? NULL
               : "LINUX_TX_ERROR";
}

/** @brief MPWM：无参数时查询，否则设置电机 PWM 频率（kHz）和衰减方式。 */
static const char *set_motor_pwm(const char *const *args, unsigned count)
{
    uint32_t khz;
    bool is_slow;
    if (count == 2U)
    {
        unsigned value = 0U;
        for (const char *p = args[0]; *p; ++p)
        {
            if (*p < '0' || *p > '9' || value > BSP_MOTOR_PWM_BASE_KHZ)
            {
                return "PWM_PARAM";
            }
            value = value * 10U + (unsigned)(*p - '0');
        }
        if (strcmp(args[1], "0") && strcmp(args[1], "1"))
        {
            return "PWM_PARAM";
        }
        if (s_joints.active >= 0 || s_board.phase != BOARD_IDLE)
        {
            return "BUSY";
        }
        bsp_status_t status = bsp_motor_set_pwm(value, args[1][0] == '1');
        if (status == BSP_ERR_CONFIG)
        {
            return "BUSY";
        }
        if (status != BSP_OK)
        {
            return "PWM_PARAM";
        }
    }
    else if (count != 0U)
    {
        return "PWM_PARAM";
    }
    bsp_motor_get_pwm(&khz, &is_slow);
    char out[32];
    (void)snprintf(out, sizeof(out), "MPWM %lu %u\r\n", (unsigned long)khz,
                   (unsigned)is_slow);
    (void)send_line(out);
    return NULL;
}

static const char *aux_command(const char *op, const char *const *args,
                               unsigned count)
{
    if (!strcmp(op, "LSET"))
    {
        return set_lamps(args, count);
    }
    if (!strcmp(op, "MPWM"))
    {
        return set_motor_pwm(args, count);
    }
    if (!s_is_linux_ok)
    {
        return "LINUX_UNAVAILABLE";
    }
    if (!strcmp(op, "LMODE"))
    {
        return set_linux_mode(args, count);
    }
    if (!strcmp(op, "LTX"))
    {
        return send_linux_hex(args, count);
    }
    return "UNSUPPORTED";
}

static bool joint_drive(unsigned index, int duty)
{
    return bsp_motor_set_duty((bsp_motor_id_t)index, (int16_t)duty) == BSP_OK;
}

static bool joint_brake(unsigned index)
{
    return bsp_motor_brake((bsp_motor_id_t)index) == BSP_OK;
}

/** @brief 上报焦点关节的状态；config 为 true 时先上报标定参数。 */
static void joint_report(uint32_t now, bool config)
{
    const joint_state_t *s = &s_joints.states[s_joint_focus];
    char out[128];
    if (config && s->calibrated)
    {
        (void)snprintf(
            out, sizeof(out), "JCAL %lu M%u %u %ld %u %ld %d %u %u\r\n",
            (unsigned long)now, s_joint_focus + 1U, (unsigned)s->cal.raw_a,
            (long)s->cal.angle_a, (unsigned)s->cal.raw_b, (long)s->cal.angle_b,
            s->cal.direction, s->cal.duty, s->cal.tolerance);
        (void)send_line(out);
        (void)snprintf(out, sizeof(out), "JLIM %lu M%u %ld %ld\r\n",
                       (unsigned long)now, s_joint_focus + 1U,
                       (long)s->cal.limit_min, (long)s->cal.limit_max);
        (void)send_line(out);
    }
    if (config)
    {
        (void)snprintf(out, sizeof(out), "JTUNE %lu M%u %u %u %u %u\r\n",
                       (unsigned long)now, s_joint_focus + 1U,
                       (unsigned)s->tune.mode, s->tune.kp, s->tune.duty_min,
                       s->tune.brake_ms);
        (void)send_line(out);
    }
    bool is_fresh =
        s_joints.valid &&
        (uint32_t)(HAL_GetTick() - s_joints.sample_ms) <= JOINT_SENSOR_STALE_MS;
    (void)snprintf(
        out, sizeof(out), "JSTATE %lu M%u %u %u %u %ld %ld %d %s %s\r\n",
        (unsigned long)now, s_joint_focus + 1U, (unsigned)s->calibrated,
        (unsigned)is_fresh, (unsigned)s->raw, (long)s->angle, (long)s->target,
        s->duty, joint_phase_name(s->phase), s->fault);
    (void)send_line(out);
}

/** @brief 把全部关节的当前标定写入 Flash；返回是否成功。 */
static bool save_calibration(void)
{
    uint8_t blob[JOINT_CAL_BLOB_SIZE];
    joint_cal_export(&s_joints, blob);
    return s_is_cal_store_ok && bsp_param_flash_save(blob) == BSP_OK;
}

/**
 * @brief 初始化 Flash 参数存储并恢复上次保存的标定。
 *
 * @return 恢复的关节数。
 * @note 需要擦除备用扇区时会阻塞约 2 秒，只在初始化阶段调用。
 */
static unsigned load_calibration(void)
{
    bsp_status_t status = bsp_param_flash_init(JOINT_CAL_BLOB_SIZE);
    s_is_cal_store_ok = (status == BSP_OK);
    /* 备用扇区擦除失败（ERR_HAL）时已有记录仍可读，只是之后保存可能失败。 */
    uint8_t blob[JOINT_CAL_BLOB_SIZE];
    if (((status != BSP_OK) && (status != BSP_ERR_HAL)) ||
        (bsp_param_flash_load(blob) != BSP_OK))
    {
        return 0U;
    }
    return joint_cal_import(&s_joints, blob);
}

/** @brief 上报全部已标定关节的参数和状态，供上位机同步。 */
static void report_calibrations(void)
{
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        if (s_joints.states[i].calibrated)
        {
            s_joint_focus = i;
            joint_report(HAL_GetTick(), true);
        }
    }
    s_joint_focus = 0U;
}

/** @brief CAL 参数顺序：raw_a angle_a raw_b angle_b direction duty tolerance。 */
/** @brief 读数不能取 0 或满量程：那两个值同时也是断线、短路的读数。 */
static bool is_raw_usable(int32_t raw)
{
    return raw >= 1 && raw <= (int32_t)BSP_ADC_RAW_MAX - 1;
}

/**
 * @brief 标定或允许范围修改成功后：保存到 Flash 并回报新参数。
 *
 * @note 新参数已在 RAM 生效，ACK 照常回复；保存失败另行报告，下次上电会丢失。
 */
static void commit_calibration(uint32_t now)
{
    if (!save_calibration())
    {
        (void)send_line("EVENT CAL_SAVE_ERROR\r\n");
    }
    joint_report(now, true);
}

/**
 * @brief CAL：两点实测标定。参数顺序：raw_a angle_a raw_b angle_b direction
 *        duty tolerance。允许范围取两个标定点之间，可再用 JLIM 修改。
 */
static const char *calibrate_joint(unsigned index, const int32_t *v,
                                   uint32_t now)
{
    if (!is_raw_usable(v[0]) || !is_raw_usable(v[2]) || v[5] < 1 || v[6] < 0)
    {
        return "CAL_PARAM";
    }
    joint_cal_t cal = {
        .raw_a = (uint16_t)v[0],
        .raw_b = (uint16_t)v[2],
        .angle_a = v[1],
        .angle_b = v[3],
        .direction = (int)v[4],
        .duty = (unsigned)v[5],
        .tolerance = (unsigned)v[6],
        .limit_min = v[1],
        .limit_max = v[3],
    };
    const char *err = joint_calibrate(&s_joints, index, cal);
    if (!err)
    {
        commit_calibration(now);
    }
    return err;
}

/**
 * @brief JNOM：理论标定。参数顺序：raw_ref angle_ref sensor_direction
 *        direction duty tolerance limit_min limit_max pot_ohms（0 为标称值）。
 */
static const char *calibrate_joint_nominal(unsigned index, const int32_t *v,
                                           uint32_t now)
{
    if (!is_raw_usable(v[0]) || v[4] < 1 || v[5] < 0 || v[8] < 0)
    {
        return "CAL_PARAM";
    }
    joint_nominal_t nominal = {
        .raw_ref = (uint16_t)v[0],
        .angle_ref = v[1],
        .sensor_direction = (int)v[2],
        .direction = (int)v[3],
        .duty = (unsigned)v[4],
        .tolerance = (unsigned)v[5],
        .limit_min = v[6],
        .limit_max = v[7],
        .pot_ohms = (uint32_t)v[8],
    };
    joint_cal_t cal;
    const char *err = joint_cal_from_nominal(&nominal, &cal);
    if (!err)
    {
        err = joint_calibrate(&s_joints, index, cal);
    }
    if (!err)
    {
        commit_calibration(now);
    }
    return err;
}

static const char *joint_command(const char *op, unsigned seq, unsigned index,
                                 const int32_t *v, unsigned count, uint32_t now)
{
    s_joint_focus = index;
    if (!strcmp(op, "JGET"))
    {
        joint_report(now, true);
        return NULL;
    }
    if (!strcmp(op, "CAL") && count == 7U)
    {
        return calibrate_joint(index, v, now);
    }
    if (!strcmp(op, "JNOM") && count == 9U)
    {
        return calibrate_joint_nominal(index, v, now);
    }
    if (!strcmp(op, "JLIM") && count == 2U)
    {
        const char *err = joint_set_limits(&s_joints, index, v[0], v[1]);
        if (!err)
        {
            commit_calibration(now);
        }
        return err;
    }
    if (!strcmp(op, "JMOVE") && count == 2U)
    {
        return joint_move(&s_joints, seq, index, v[0], (uint32_t)v[1], now);
    }
    if (!strcmp(op, "JTUNE") && count == 4U)
    {
        if (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[3] < 0)
        {
            return "TUNE_PARAM";
        }
        joint_tune_t tune = {(joint_mode_t)v[0], (unsigned)v[1],
                             (unsigned)v[2], (unsigned)v[3]};
        const char *err = joint_set_tune(&s_joints, index, tune);
        if (!err)
        {
            joint_report(now, true);
        }
        return err;
    }
    if (!strcmp(op, "JTRACE") && count == 1U)
    {
        if (v[0] != 0 && v[0] != 1)
        {
            return "JOINT_PARAM";
        }
        s_is_trace_on = v[0] == 1;
        return NULL;
    }
    return "JOINT_PARAM";
}

/* -------------------------------------------------------------------------- */
/* 周期处理                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 读 HOME、推进各状态机、采样电位器、推进关节闭环。
 *
 * @note 在一次 update 中被多次调用：每次阻塞发送之后都调一次，
 *       使 HOME 急停和心跳检查不会被状态上报拖延。
 */
static void check_control(void)
{
    bool pressed = false;
    uint32_t count = s_last_home_count;
    if (bsp_key_read(&pressed) != BSP_OK ||
        bsp_key_get_press_count(&count) != BSP_OK)
    {
        debug_control_fault(&s_control, "EVENT KEY_ERROR\r\n");
        return;
    }
    /* 计数能抓到 ADC 采样或限时发送期间发生的短按。 */
    bool edge = count != s_last_home_count;
    s_last_home_count = count;
    s_is_home_pressed = pressed;
    uint32_t now = HAL_GetTick();
    /* KEY 测试要先看到真实按键，再应用 HOME 急停锁定。 */
    if (s_board.phase == BOARD_KEY_WAIT)
    {
        debug_board_update(&s_board, now, pressed || edge);
    }
    debug_control_update(&s_control, now, pressed || edge);
    if (s_is_power_ok && bsp_linux_power_update() != BSP_OK)
    {
        debug_control_fault(&s_control, "EVENT POWER_ERROR\r\n");
    }
    if (!s_control.locked)
    {
        debug_board_update(&s_board, HAL_GetTick(), false);
    }
    now = HAL_GetTick();
    if ((uint32_t)(now - s_last_adc_ms) >= ADC_PERIOD_MS)
    {
        s_last_adc_ms = now;
        static uint16_t samples[ADC_OVERSAMPLE][BSP_ADC_CHANNEL_COUNT];
        s_is_pot_valid = s_is_adc_ok;
        for (unsigned i = 0; i < ADC_OVERSAMPLE && s_is_pot_valid; ++i)
        {
            s_is_pot_valid =
                bsp_adc_read_all(samples[i], ADC_TIMEOUT_MS) == BSP_OK;
        }
        if (s_is_pot_valid)
        {
            joint_filter((const uint16_t(*)[JOINT_COUNT])samples,
                         ADC_OVERSAMPLE, s_pot_cache, s_pot_spread);
        }
        joint_feed(&s_joints, s_pot_cache, s_pot_spread, s_is_pot_valid,
                   HAL_GetTick());
    }
    if (!joint_tick(&s_joints, HAL_GetTick()))
    {
        debug_control_fault(&s_control, "EVENT JOINT_FAULT\r\n");
    }
}

/** @brief 处理 USART1 透传：报告错误和溢出，把收到的字节以 LRX 行转发。 */
static void linux_update(void)
{
    if (!s_is_linux_ok || !bsp_linux_uart_stream_is_active())
    {
        return;
    }
    if (bsp_linux_uart_stream_update() != BSP_OK)
    {
        (void)bsp_linux_uart_stream_stop();
    }
    uint32_t errors = bsp_linux_uart_stream_error_count();
    if (errors != s_seen_linux_errors)
    {
        s_seen_linux_errors = errors;
        (void)send_line("LERR RX_ERROR\r\n");
    }
    uint32_t overflows = bsp_linux_uart_stream_overflow_count();
    if (overflows != s_seen_linux_overflows)
    {
        s_seen_linux_overflows = overflows;
        (void)send_line("LERR RX_OVERFLOW\r\n");
    }

    static const char digits[] = "0123456789ABCDEF";
    char out[sizeof("LRX ") + 2U * LINUX_RX_BYTES_PER_LINE + sizeof("\r\n")] =
        "LRX ";
    size_t n = sizeof("LRX ") - 1U;
    uint8_t value;
    for (unsigned i = 0;
         i < LINUX_RX_BYTES_PER_LINE && bsp_linux_uart_stream_read(&value); ++i)
    {
        out[n++] = digits[value >> 4];
        out[n++] = digits[value & 0x0FU];
    }
    if (n > sizeof("LRX ") - 1U)
    {
        out[n++] = '\r';
        out[n++] = '\n';
        out[n] = '\0';
        (void)send_line(out);
    }
}

/** @brief 处理 USART3 接收错误：丢弃缓冲和当前行，停机锁定。 */
static void check_console_rx(void)
{
    if (bsp_debug_uart_update() != BSP_OK)
    {
        /* 无法重新启动接收，调试终端已不可用：停机等待看门狗或人工复位。 */
        Error_Handler();
    }
    uint32_t errors = bsp_debug_uart_rx_error_count();
    uint32_t overflows = bsp_debug_uart_rx_overflow_count();
    if (errors != s_seen_rx_errors || overflows != s_seen_rx_overflows)
    {
        s_seen_rx_errors = errors;
        s_seen_rx_overflows = overflows;
        bsp_debug_uart_flush();
        s_line_length = 0U;
        s_is_discarding = true;
        debug_control_fault(&s_control, "EVENT UART_ERROR\r\n");
    }
}

/** @brief 把收到的字节拼成行，每满一行交给 debug_control。 */
static void process_console_rx(void)
{
    uint8_t byte;
    for (unsigned i = 0; i < CONSOLE_RX_BUDGET && bsp_debug_uart_read(&byte);
         ++i)
    {
        char ch = (char)byte;
        if (ch == '\r')
        {
            continue;
        }
        if (ch == '\n')
        {
            if (!s_is_discarding && s_line_length > 0U)
            {
                s_line[s_line_length] = '\0';
                check_control();
                debug_control_line(&s_control, s_line, HAL_GetTick());
            }
            s_line_length = 0U;
            s_is_discarding = false;
        }
        else if (!s_is_discarding)
        {
            /* 只接受可打印 ASCII；超长或含控制字符的行整行丢弃。 */
            if (ch < ' ' || ch > '~' || s_line_length >= sizeof(s_line) - 1U)
            {
                s_is_discarding = true;
                s_line_length = 0U;
                debug_control_fault(&s_control, "EVENT BAD_LINE\r\n");
            }
            else
            {
                s_line[s_line_length++] = ch;
            }
        }
    }
}

/**
 * @brief 当前转动的电机及占空比，供 STATE 和 MSTATE 上报。
 *
 * @param[out] motor M 编号（1 起），0 表示没有。
 * @param[out] duty 占空比。
 * @param[out] is_pending 是否有电机在等待启动。
 *
 * @note 优先级：关节运动 > 全板电机测试 > 多路手动 > 单路 RUN。
 */
static void active_motor(unsigned *motor, int *duty, bool *is_pending)
{
    *motor = s_control.motor;
    *duty = s_control.duty;
    *is_pending = s_control.pending;
    for (unsigned i = 0; i < MOTOR_COUNT; ++i)
    {
        if (s_control.multi_duty[i])
        {
            *motor = i + 1U;
            *duty = s_control.multi_duty[i];
        }
    }
    *is_pending = *is_pending || s_control.multi_pending != 0U;
    if (s_board.kind == BOARD_MOTOR && s_board.phase == BOARD_DRIVE)
    {
        *motor = s_board.index + 1U;
        *duty = s_board.value;
    }
    if (s_board.kind == BOARD_MOTOR && s_board.phase == BOARD_SETTLE)
    {
        *is_pending = true;
    }
    if (s_joints.active >= 0)
    {
        *motor = (unsigned)s_joints.active + 1U;
        *duty = s_joints.states[s_joints.active].duty;
        *is_pending = s_joints.states[s_joints.active].phase == JOINT_SETTLE;
    }
}

/** @brief 周期状态上报：POT、STATE、MSTATE、LSTATE、BOARD、KEY、JSTATE。 */
static void report_status(uint32_t now)
{
    char out[STATUS_LINE_MAX];
    size_t used = 0U;

    check_control();
    if (s_is_pot_valid)
    {
        append(out, sizeof(out), &used, "POT %lu",
               (unsigned long)HAL_GetTick());
        for (unsigned i = 0; i < BSP_ADC_CHANNEL_COUNT; ++i)
        {
            append(out, sizeof(out), &used, " %u", (unsigned)s_pot_cache[i]);
        }
        append(out, sizeof(out), &used, "\r\n");
    }
    else
    {
        append(out, sizeof(out), &used, "ADCERR %lu %d\r\n",
               (unsigned long)HAL_GetTick(), (int)BSP_ERR_HAL);
    }
    (void)send_line(out);
    check_control();

    unsigned motor;
    int duty;
    bool is_pending;
    active_motor(&motor, &duty, &is_pending);
    (void)snprintf(out, sizeof(out), "STATE %lu %u %d %u %u\r\n",
                   (unsigned long)HAL_GetTick(), motor, duty,
                   (unsigned)is_pending, (unsigned)s_control.locked);
    (void)send_line(out);
    check_control();

    int duties[MOTOR_COUNT];
    uint32_t remaining[MOTOR_COUNT];
    for (unsigned i = 0; i < MOTOR_COUNT; ++i)
    {
        duties[i] = s_control.multi_duty[i];
        remaining[i] = 0U;
        if (duties[i] && s_control.multi_duration[i])
        {
            uint32_t elapsed = now - s_control.multi_start[i];
            remaining[i] = elapsed < s_control.multi_duration[i]
                               ? s_control.multi_duration[i] - elapsed
                               : 0U;
        }
    }
    if (motor > 0U && motor <= MOTOR_COUNT)
    {
        duties[motor - 1U] = duty;
    }
    used = 0U;
    append(out, sizeof(out), &used, "MSTATE %lu", (unsigned long)now);
    for (unsigned i = 0; i < MOTOR_COUNT; ++i)
    {
        append(out, sizeof(out), &used, " %d", duties[i]);
    }
    for (unsigned i = 0; i < MOTOR_COUNT; ++i)
    {
        append(out, sizeof(out), &used, " %lu", (unsigned long)remaining[i]);
    }
    append(out, sizeof(out), &used, " %u %u\r\n", s_control.multi_pending,
           (unsigned)s_control.locked);
    (void)send_line(out);
    check_control();

    unsigned lights[BSP_LIGHT_COUNT];
    memcpy(lights, s_lamp_duty, sizeof(lights));
    if (s_board.kind == BOARD_LAMP && s_board.phase == BOARD_DRIVE)
    {
        lights[s_board.index] = (unsigned)s_board.value;
    }
    used = 0U;
    append(out, sizeof(out), &used, "LSTATE %lu", (unsigned long)now);
    for (unsigned i = 0; i < BSP_LIGHT_COUNT; ++i)
    {
        append(out, sizeof(out), &used, " %u", lights[i]);
    }
    append(out, sizeof(out), &used, " %u\r\n",
           (unsigned)bsp_linux_uart_stream_is_active());
    (void)send_line(out);
    check_control();

    (void)snprintf(out, sizeof(out), "BOARD %lu %s %s\r\n",
                   (unsigned long)HAL_GetTick(),
                   s_board.phase == BOARD_IDLE ? "-" : s_board.name,
                   debug_board_phase(&s_board));
    (void)send_line(out);
    check_control();

    (void)snprintf(out, sizeof(out), "KEY %lu %u %lu\r\n",
                   (unsigned long)HAL_GetTick(), (unsigned)s_is_home_pressed,
                   (unsigned long)s_last_home_count);
    (void)send_line(out);
    check_control();

    joint_report(HAL_GetTick(), false);
}

/**
 * @brief JTRACE 打开时，每个新采样上报一行焦点关节的数据：
 *        JT 采样时刻 M编号 读数 极差 角度 目标 占空比 制动 阶段。
 *
 * @note 占空比取 H 桥最近一次设定值，RUN、MSET 开环驱动时也有效；
 *       115200 波特率下一行约 4ms，与周期状态合计约占带宽七成。
 */
static void report_trace(void)
{
    if (!s_is_trace_on || !s_joints.valid ||
        s_joints.sample_ms == s_trace_sample_ms)
    {
        return;
    }
    s_trace_sample_ms = s_joints.sample_ms;
    const joint_state_t *s = &s_joints.states[s_joint_focus];
    int16_t duty = 0;
    (void)bsp_motor_hbridge_get_duty(
        bsp_motor_handle((bsp_motor_id_t)s_joint_focus), &duty);
    bool is_braking =
        s_joints.active == (int)s_joint_focus && s_joints.braking;
    char out[96];
    (void)snprintf(out, sizeof(out), "JT %lu M%u %u %u %ld %ld %d %u %s\r\n",
                   (unsigned long)s_joints.sample_ms, s_joint_focus + 1U,
                   (unsigned)s->raw, (unsigned)s->spread,
                   s->calibrated ? (long)s->angle : 0L, (long)s->target,
                   (int)duty, (unsigned)is_braking, joint_phase_name(s->phase));
    (void)send_line(out);
    check_control();
}

/* -------------------------------------------------------------------------- */
/* 对外接口                                                                    */
/* -------------------------------------------------------------------------- */

void debug_console_init(void)
{
    if (bsp_motor_init() != BSP_OK || bsp_key_init() != BSP_OK)
    {
        Error_Handler();
    }
    s_is_light_ok = bsp_light_init() == BSP_OK;
    s_is_buzzer_ok = bsp_buzzer_init() == BSP_OK;
    s_is_power_ok = bsp_linux_power_init() == BSP_OK;

    board_hw_t board_hw = {board_apply, outputs_off, reply_line};
    debug_board_init(&s_board, board_hw);
    joint_hw_t joint_hw = {joint_drive, reply_line, joint_brake};
    joint_init(&s_joints, joint_hw);
    /* 在串口开始接收、看门狗启动之前加载：可能要擦除备用扇区，阻塞约 2 秒。 */
    unsigned restored = load_calibration();
    s_is_linux_ok = bsp_linux_uart_init() == BSP_OK;
    debug_hw_t hw = {
        .stop = stop_all,
        .run = run_motor,
        .reply = reply_line,
        .test = start_test,
        .confirm = confirm_test,
        .joint = joint_command,
        .manual = manual_control,
        .aux = aux_command,
    };
    debug_control_init(&s_control, hw);
    s_is_adc_ok = bsp_adc_init() == BSP_OK;

    if (bsp_debug_uart_init() != BSP_OK)
    {
        Error_Handler();
    }
    s_is_initialized = true;
    (void)send_line("READY BOARD_DEBUG 2\r\n");
    if (!s_is_cal_store_ok)
    {
        (void)send_line("EVENT CAL_STORE_ERROR\r\n");
    }
    if (restored > 0U)
    {
        report_calibrations();
    }
}

void debug_console_update(void)
{
    if (!s_is_initialized)
    {
        return;
    }
    check_control();
    linux_update();
    check_console_rx();
    process_console_rx();
    check_control();

    report_trace();

    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - s_last_status_ms) >= STATUS_PERIOD_MS)
    {
        s_last_status_ms = now;
        report_status(now);
    }
}
