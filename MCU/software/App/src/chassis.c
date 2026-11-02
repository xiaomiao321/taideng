/**
 * @file chassis.c
 * @brief 底盘动作映射实现。
 */

#include "chassis.h"

#include "app_config.h"
#include "mcu_protocol.h"
#include "motor_limits.h"

void chassis_init(chassis_t *c, bool (*drive)(unsigned motor, int duty))
{
    c->drive = drive;
    c->motion = MCU_PROTOCOL_MOTION_STOP;
    c->speed = 0U;
    c->fault = false;
}

int chassis_duty(uint8_t speed)
{
    int duty = (int)speed * 10;
    if (duty == 0)
    {
        return 0;
    }
    if (duty < MOTOR_DUTY_MIN)
    {
        return MOTOR_DUTY_MIN;
    }
    return (duty > MOTOR_DUTY_LARGE_MAX) ? MOTOR_DUTY_LARGE_MAX : duty;
}

/** @brief 两路都设为 0；两路都会尝试，返回是否都成功。 */
static bool coast(chassis_t *c)
{
    bool left = c->drive(CHASSIS_LEFT_MOTOR, 0);
    bool right = c->drive(CHASSIS_RIGHT_MOTOR, 0);
    return left && right;
}

bool chassis_apply(chassis_t *c, uint8_t motion, uint8_t speed)
{
    if (motion == MCU_PROTOCOL_MOTION_STOP)
    {
        return chassis_stop(c);
    }

    int duty = chassis_duty(speed);
    int left = duty;
    int right = duty;
    switch (motion)
    {
        case MCU_PROTOCOL_MOTION_BACKWARD:
            left = -duty;
            right = -duty;
            break;
        case MCU_PROTOCOL_MOTION_TURN_LEFT:
            left = -duty;
            break;
        case MCU_PROTOCOL_MOTION_TURN_RIGHT:
            right = -duty;
            break;
        default: /* FORWARD */
            break;
    }

    if (!c->drive(CHASSIS_LEFT_MOTOR, left * CHASSIS_LEFT_SIGN) ||
        !c->drive(CHASSIS_RIGHT_MOTOR, right * CHASSIS_RIGHT_SIGN))
    {
        /* 不留一边转一边停的状态。 */
        (void)coast(c);
        c->motion = MCU_PROTOCOL_MOTION_STOP;
        c->speed = 0U;
        c->fault = true;
        return false;
    }
    c->motion = motion;
    c->speed = speed;
    return true;
}

bool chassis_stop(chassis_t *c)
{
    c->motion = MCU_PROTOCOL_MOTION_STOP;
    c->speed = 0U;
    return coast(c);
}

void chassis_clear_fault(chassis_t *c)
{
    if (c->fault)
    {
        c->fault = !chassis_stop(c);
    }
}

uint8_t chassis_fault_flags(const chassis_t *c)
{
    return c->fault ? CHASSIS_FAULT : 0U;
}
