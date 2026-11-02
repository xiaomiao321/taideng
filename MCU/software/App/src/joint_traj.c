/**
 * @file joint_traj.c
 * @brief 关节五次多项式轨迹的规划与求值。
 */

#include "joint_traj.h"

#include <math.h>

void joint_traj_plan(joint_traj_t *t, joint_traj_point_t start, float p1,
                     float vmax, float amax, uint32_t min_ms)
{
    float distance = fabsf(p1 - start.p);
    float by_speed = JOINT_TRAJ_PEAK_SPEED_GAIN * distance / vmax;
    float by_accel = sqrtf(JOINT_TRAJ_PEAK_ACCEL_GAIN * distance / amax);
    float seconds = by_speed > by_accel ? by_speed : by_accel;
    uint32_t ms = (uint32_t)ceilf(seconds * 1000.0f);
    t->duration_ms = ms > min_ms ? ms : min_ms;

    /* 边界条件：起点 (p0, v0, a0)，终点 (p1, 0, 0)。 */
    float T = (float)t->duration_ms / 1000.0f;
    float D = p1 - start.p;
    float vT = start.v * T;
    float aT2 = start.a * T * T;
    t->p0 = start.p;
    t->v0 = start.v;
    t->a0 = start.a;
    t->p1 = p1;
    t->c3 = (20.0f * D - 12.0f * vT - 3.0f * aT2) / (2.0f * T * T * T);
    t->c4 = (-30.0f * D + 16.0f * vT + 3.0f * aT2) / (2.0f * T * T * T * T);
    t->c5 = (12.0f * D - 6.0f * vT - aT2) / (2.0f * T * T * T * T * T);
}

joint_traj_point_t joint_traj_eval(const joint_traj_t *t, float elapsed_ms)
{
    joint_traj_point_t r = {t->p1, 0.0f, 0.0f};
    if (elapsed_ms >= (float)t->duration_ms)
    {
        return r;
    }
    float x = elapsed_ms > 0.0f ? elapsed_ms / 1000.0f : 0.0f;
    float a2 = t->a0 / 2.0f;
    r.p = t->p0 +
          x * (t->v0 + x * (a2 + x * (t->c3 + x * (t->c4 + x * t->c5))));
    r.v = t->v0 + x * (t->a0 + x * (3.0f * t->c3 +
                                    x * (4.0f * t->c4 + x * 5.0f * t->c5)));
    r.a = t->a0 + x * (6.0f * t->c3 + x * (12.0f * t->c4 + x * 20.0f * t->c5));
    return r;
}
