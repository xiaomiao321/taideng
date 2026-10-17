#ifndef MOTOR_LIMITS_H
#define MOTOR_LIMITS_H
#include <stdbool.h>
/* Board policy, permille: M1..M6 small joints; M7/M8 large tracks. */
#define MOTOR_DUTY_MIN 500
#define MOTOR_DUTY_SMALL_MAX 1000
#define MOTOR_DUTY_LARGE_MAX 800
static inline bool motor_duty_valid(unsigned motor, int duty)
{
    if (motor < 1U || motor > 8U)
    {
        return false;
    }
    if (duty == 0)
    {
        return true; /* coast/stop is always available */
    }
    int max = motor <= 6U ? MOTOR_DUTY_SMALL_MAX : MOTOR_DUTY_LARGE_MAX;
    return (duty >= MOTOR_DUTY_MIN && duty <= max) ||
           (duty <= -MOTOR_DUTY_MIN && duty >= -max);
}
#endif
