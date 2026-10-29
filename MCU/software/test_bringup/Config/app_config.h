/**
 * @file app_config.h
 * @brief 全部可调参数集中在这里：运行模式、电机、关节控制、采样与安全时限。
 *
 * 修改后重新编译烧录即可生效。这里只放“调参会动”的宏；协议格式、标定数据
 * 格式、Flash 地址、缓冲区大小和引脚映射不在此列，改动它们需要同时改代码。
 *
 * 本文件不依赖 HAL，可被 BSP、App 和 PC 端 HostTest 共同包含。
 * 上位机 Tools/motor_debug/motor_limits.py 镜像了占空比范围，改这里时同步。
 */

#ifndef AITAID_APP_CONFIG_H
#define AITAID_APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* 运行模式                                                                    */
/* -------------------------------------------------------------------------- */

#define APP_MODE_DEBUG (0) /**< USART3 上位机 / 终端，开放全部功能。 */
#define APP_MODE_BOARD (1) /**< 按 HOME 启动全板测试。 */
#define APP_MODE_ANDROID                                                       \
    (2) /**< USART1 二进制 Android 协议，USART3 只出日志。 */

/** @brief 固件入口；编译时也可用 -DAPP_MODE=... 覆盖。 */
#ifndef APP_MODE
#define APP_MODE APP_MODE_DEBUG
#endif

/**
 * @brief 独立看门狗标称超时，单位毫秒。
 *
 * 主循环单次最长阻塞约 100ms（串口限时发送），LSI 最快时实际超时约为标称值
 * 的 0.68 倍，仍有充足余量。BOARD 模式的测试用例会阻塞等待人工确认，不启用。
 */
#define APP_WATCHDOG_TIMEOUT_MS (1000U)

/* -------------------------------------------------------------------------- */
/* 电机输出                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 允许输出的最小占空比绝对值，千分比。
 *
 * 滑行 PWM 下低于约 50% 电机转不动；刹车 PWM 下能转的占空比更低，
 * 调试时需要扫低占空比，所以这里只挡掉明显无意义的值。
 */
#define MOTOR_DUTY_MIN (100)

/**
 * @brief 全部电机的占空比硬上限，千分比，暂定 100%。
 *
 * 开环 RUN/MSET、板级测试、关节开关式和比例式的输出都不会超过它；
 * 标定里填的占空比高于它时，输出按它截断，标定本身仍然有效。
 * 上位机 motor_limits.py 的 DUTY_LIMIT 需同步修改。
 */
#ifndef MOTOR_DUTY_LIMIT
#define MOTOR_DUTY_LIMIT (1000)
#endif

/**
 * @brief 上电默认的电机 PWM 频率（kHz，1、2、4、5、10、20 之一）与衰减方式。
 *
 * 2026-10-24 实测 M1：20kHz 滑行、5kHz 滑行、20kHz 刹车在 40% 下都转不动，
 * 5kHz 刹车 20%～30% 即可稳定转动，断电后滑行也小得多。
 */
#define BSP_MOTOR_PWM_DEFAULT_KHZ (5U)
#define BSP_MOTOR_PWM_DEFAULT_SLOW_DECAY (true)

/** @brief 调试终端里电机从停止到按新占空比启动之间的等待，单位毫秒。 */
#define DEBUG_CONTROL_SETTLE_MS (100U)

/** @brief 板级测试中单个电机项目的最长运行时间，单位毫秒。 */
#define DEBUG_BOARD_MAX_MOTOR_DURATION_MS (5000U)

/* -------------------------------------------------------------------------- */
/* 电位器与采样                                                                */
/* -------------------------------------------------------------------------- */

/** @brief 电位器采样周期，单位毫秒。 */
#define POT_SAMPLE_PERIOD_MS (10U)

/** @brief 每个采样周期连续扫描的次数，去掉最大最小各四分之一后取平均。 */
#define POT_OVERSAMPLE (16U)

/** @brief 电位器读数距上次采样超过该时间即视为失效，单位毫秒。 */
#define JOINT_SENSOR_STALE_MS (50U)

/** @brief 电位器标称电气行程，毫度。 */
#define JOINT_POT_TRAVEL_MDEG (270000)

/** @brief 电位器阻值未知时采用的标称值，欧姆。 */
#define JOINT_POT_DEFAULT_OHMS (45000U)

/** @brief 电位器两端各串的固定电阻（3V3-1k-[电位器]-1k-GND），欧姆。 */
#define JOINT_POT_END_OHMS (1000U)

/* -------------------------------------------------------------------------- */
/* 关节闭环                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 没保存过控制参数时的默认值：比例式，每度 1%，起转 30%，制动 50ms。
 *
 * 2026-10-24 在 5kHz 刹车 PWM 下实测 M1：3°～30° 的定位 1～1.5 秒到位，
 * 过冲不超过 2.4°。已保存到 Flash 的参数优先。
 */
#define JOINT_DEFAULT_MODE (JOINT_MODE_P)
#define JOINT_DEFAULT_KP (10U)
#define JOINT_DEFAULT_DUTY_MIN (300U)
#define JOINT_DEFAULT_BRAKE_MS (50U)

/** @brief 比例系数上限，千分比/度。 */
#define JOINT_KP_MAX (1000U)

/** @brief 制动时长上限，单位毫秒；不超过 JOINT_REACHED_HOLD_MS。 */
#define JOINT_BRAKE_MAX_MS (200U)

/** @brief 到达容差的允许范围，毫度。 */
#define JOINT_TOLERANCE_MIN (100U)
#define JOINT_TOLERANCE_MAX (10000U)

/** @brief 单次运动超时的允许范围，单位毫秒。 */
#define JOINT_MOVE_TIMEOUT_MIN_MS (500U)
#define JOINT_MOVE_TIMEOUT_MAX_MS (60000U)

/** @brief 在容差内连续保持该时间才判定到达，单位毫秒。 */
#define JOINT_REACHED_HOLD_MS (200U)

/** @brief 误差至少减小这么多才算有进展，毫度。 */
#define JOINT_PROGRESS_STEP (500)

/** @brief 超过该时间没有进展判定为停滞，单位毫秒。 */
#define JOINT_STALL_MS (2000U)

/** @brief 误差比初始值多出 2 倍容差加该余量即判定方向反了，毫度。 */
#define JOINT_DIRECTION_MARGIN (1000U)

/** @brief 换向前停顿时间，单位毫秒。 */
#define JOINT_REVERSE_SETTLE_MS (100U)

/** @brief 开关式：误差小于 5 倍容差加该余量时改为点动，毫度。 */
#define JOINT_NEAR_MARGIN (5000U)

/** @brief 开关式点动脉冲：驱动段与停顿段时长，单位毫秒。 */
#define JOINT_PULSE_ON_MS (30U)
#define JOINT_PULSE_OFF_MS (100U)

/**
 * @brief 转速估计：取最近 JOINT_SPEED_WINDOW 次采样（10ms 一次，约 200ms），
 *        最后 JOINT_SPEED_EDGE 次的平均角度减最早 JOINT_SPEED_EDGE 次的平均
 *        角度，再除以两组的平均时间差。
 *
 * 静止噪声约 0.18° 时估计值的噪声约 0.75°/s（按样本独立估算，三点中值滤波
 * 使样本相关，实际会大一些，上板后再调）。
 */
#define JOINT_SPEED_WINDOW (21U)
#define JOINT_SPEED_EDGE (4U)

/**
 * @brief 零速判定的迟滞，毫度/秒：静止时超过 MOVE 才算开始转动，转动时低于
 *        ZERO 才报 0，避免估计值在阈值附近来回跳。
 */
#define JOINT_SPEED_ZERO_MDEG_S (2000)
#define JOINT_SPEED_MOVE_MDEG_S (4000)

/* -------------------------------------------------------------------------- */
/* 底盘                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 左右履带对应的电机下标（M7 为 6，M8 为 7）与正方向。
 *
 * 方向为 1 表示该电机正转时整机前进，-1 表示反转时前进；装机后接线变了只改
 * 这里。2026-10-29 确认：左 M7、右 M8，两路都是正转前进。
 */
#define CHASSIS_LEFT_MOTOR (6U)
#define CHASSIS_RIGHT_MOTOR (7U)
#define CHASSIS_LEFT_SIGN (1)
#define CHASSIS_RIGHT_SIGN (1)

/* -------------------------------------------------------------------------- */
/* 通信与安全时限                                                              */
/* -------------------------------------------------------------------------- */

/** @brief 调试终端心跳超时：超过该时间没收到 PING 即停止输出并锁定，毫秒。 */
#define DEBUG_CONTROL_HEARTBEAT_TIMEOUT_MS (1000U)

/** @brief 调试终端周期状态上报的间隔，单位毫秒。 */
#define STATUS_REPORT_PERIOD_MS (100U)

/** @brief Android 协议失联保护：超过该时间没收到合法帧即停止全部电机，毫秒。 */
#define MCU_COMMAND_LINK_TIMEOUT_MS (5000U)

/**
 * @brief Android SET_ARM_POSITION 的固件内部单次运动期限，单位毫秒。
 *
 * 超时置故障位 bit1；应长于 SDK 的关节观察期限（默认 10 秒）。
 */
#define MCU_ARM_MOVE_TIMEOUT_MS (15000U)

/**
 * @brief 各关节 HOME_ARM 的目标角度，毫度，依次为 M1～M6。
 *
 * MCU_ARM_HOME_NONE 表示该关节没有 HOME，HOME_ARM 回未知命令；装上机构实测后
 * 再填，并与 App 端能力描述里的 homePositionMilliDegrees 保持一致。
 */
#define MCU_ARM_HOME_NONE (INT32_MIN)
#define MCU_ARM_HOME_MDEG                                                      \
    {                                                                          \
        MCU_ARM_HOME_NONE, MCU_ARM_HOME_NONE, MCU_ARM_HOME_NONE,               \
            MCU_ARM_HOME_NONE, MCU_ARM_HOME_NONE, MCU_ARM_HOME_NONE            \
    }

/** @brief HOME 键消抖时间，单位毫秒。 */
#define BSP_KEY_DEBOUNCE_MS (20U)

#endif /* AITAID_APP_CONFIG_H */
