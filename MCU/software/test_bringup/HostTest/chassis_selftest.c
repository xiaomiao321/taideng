#include "chassis.h"
#include "app_config.h"
#include "mcu_protocol.h"
#include "motor_limits.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int duties[MOTOR_COUNT];
static unsigned drive_calls;
/* 第几次调用开始失败，0 表示一直成功。 */
static unsigned fail_from;
static bool drive(unsigned motor, int duty)
{
    assert(motor < MOTOR_COUNT);
    ++drive_calls;
    if (fail_from != 0U && drive_calls >= fail_from)
    {
        return false;
    }
    duties[motor] = duty;
    return true;
}
static chassis_t fresh(void)
{
    chassis_t c;
    memset(duties, 0, sizeof(duties));
    drive_calls = 0;
    fail_from = 0;
    chassis_init(&c, drive);
    return c;
}
static int left(void)
{
    return duties[CHASSIS_LEFT_MOTOR] * CHASSIS_LEFT_SIGN;
}
static int right(void)
{
    return duties[CHASSIS_RIGHT_MOTOR] * CHASSIS_RIGHT_SIGN;
}
int main(void)
{
    /* 左 M7、右 M8，只动这两路。 */
    assert(CHASSIS_LEFT_MOTOR == 6U && CHASSIS_RIGHT_MOTOR == 7U);

    /* 速度百分比当占空比，低于起转下限抬上去，高于履带上限截断。 */
    assert(chassis_duty(0) == 0);
    assert(chassis_duty(1) == MOTOR_DUTY_MIN);
    assert(chassis_duty(50) == 500);
    assert(chassis_duty(100) == MOTOR_DUTY_LARGE_MAX);

    chassis_t c = fresh();
    assert(c.motion == MCU_PROTOCOL_MOTION_STOP && c.speed == 0);
    assert(drive_calls == 0);

    assert(chassis_apply(&c, MCU_PROTOCOL_MOTION_FORWARD, 50));
    assert(left() == 500 && right() == 500);
    assert(c.motion == MCU_PROTOCOL_MOTION_FORWARD && c.speed == 50);
    for (unsigned i = 0; i < MOTOR_SMALL_COUNT; ++i)
    {
        assert(duties[i] == 0);
    }

    assert(chassis_apply(&c, MCU_PROTOCOL_MOTION_BACKWARD, 60));
    assert(left() == -600 && right() == -600);

    /* 原地转：左转左轮后退、右轮前进。 */
    assert(chassis_apply(&c, MCU_PROTOCOL_MOTION_TURN_LEFT, 40));
    assert(left() == -400 && right() == 400);
    assert(chassis_apply(&c, MCU_PROTOCOL_MOTION_TURN_RIGHT, 40));
    assert(left() == 400 && right() == -400);

    /* 非 STOP 速度 0：两路滑行，状态仍记该动作。 */
    assert(chassis_apply(&c, MCU_PROTOCOL_MOTION_FORWARD, 0));
    assert(left() == 0 && right() == 0);
    assert(c.motion == MCU_PROTOCOL_MOTION_FORWARD && c.speed == 0);

    assert(chassis_apply(&c, MCU_PROTOCOL_MOTION_FORWARD, 30));
    assert(chassis_apply(&c, MCU_PROTOCOL_MOTION_STOP, 0));
    assert(left() == 0 && right() == 0);
    assert(c.motion == MCU_PROTOCOL_MOTION_STOP && c.speed == 0);
    assert(chassis_fault_flags(&c) == 0);

    /* 右轮设置失败：左轮也要停下，状态回 STOP，锁存故障。 */
    c = fresh();
    fail_from = 2;
    assert(!chassis_apply(&c, MCU_PROTOCOL_MOTION_FORWARD, 50));
    /* 之后的回调都失败，左轮的值停在 500；两路停车都尝试过。 */
    assert(left() == 500 && drive_calls == 4);
    assert(c.motion == MCU_PROTOCOL_MOTION_STOP && c.speed == 0);
    assert(chassis_fault_flags(&c) == CHASSIS_FAULT);

    /* 原因仍在，STOP 清不掉；驱动恢复后 STOP 才清除。 */
    chassis_clear_fault(&c);
    assert(chassis_fault_flags(&c) == CHASSIS_FAULT);
    fail_from = 0;
    chassis_clear_fault(&c);
    assert(chassis_fault_flags(&c) == 0);
    assert(left() == 0 && right() == 0);

    /* chassis_stop 本身不清故障，失联停车走这条路。 */
    c = fresh();
    fail_from = 1;
    assert(!chassis_apply(&c, MCU_PROTOCOL_MOTION_TURN_LEFT, 20));
    fail_from = 0;
    assert(chassis_stop(&c));
    assert(chassis_fault_flags(&c) == CHASSIS_FAULT);

    puts("chassis: duty mapping, forward/backward, spin turns, zero speed, "
         "stop, drive failure latching and fault clearing passed");
    return 0;
}
