/**
 ******************************************************************************
 * @file    angle_position_test.c
 * @brief   角度闭环定位联调案例实现 (两段逼近 + 制动停稳 + 卡死看门狗)
 *
 * 设计要点:
 *   1. 非阻塞: Update() 只做"读一次反馈 + 算一次指令", 无任何延时;
 *      速度指令只在变化时下发 (RZ7899_Start 软启动 / Stop(BRAKE) 急停),
 *      避免高频重复调用把斜坡重置 (RZ7899 库本身有"做减法"保护, 这里再加一层);
 *   2. 迟滞: 运行时误差 <= tol 就停; 停机后要偏出 tol+2 才重新启动,
 *      防止刹停瞬间在容差边界来回抖 (chatter);
 *   3. 卡死看门狗: 电机"应该在靠近目标"期间, 误差必须持续变小 (>=2° 的进展),
 *      否则累计 stall_ms 后判定 顶死/机械卡死/方向反了 -> 停机锁定 fault;
 *   4. 全部整数运算, 无浮点.
 ******************************************************************************
 */

#include "angle_position_test.h"

/* 说明: 公开接口的完整文档 (参数/返回值/用法) 在 angle_position_test.h 中. */

/** @brief 默认测试序列 (°): 可用 AnglePositionTest_SetSequence() 替换 */
static const uint16_t angle_pos_test_default_seq[] = {30U,  60U,  90U, 120U,
                                                      150U, 200U, 260U};

/* -------------------------------------------------------------------------- */
/* 公开接口实现 */
/* -------------------------------------------------------------------------- */

bool AnglePositionTest_Init(AnglePositionTest_Handle *test,
                            RZ7899_Handle *motor, PotAngle_Handle *pot)
{
    if ((test == NULL) || (motor == NULL) || (pot == NULL))
    {
        return false;
    }

    test->motor = motor;
    test->pot = pot;

    /* 默认参数 (可用头文件宏改默认值, 或运行时直接改句柄字段) */
    test->seq = angle_pos_test_default_seq;
    test->seq_count = (uint16_t)(sizeof(angle_pos_test_default_seq) /
                                 sizeof(angle_pos_test_default_seq[0]));
    test->tol_deg = ANGLE_POS_TEST_TOL_DEG;
    test->near_deg = ANGLE_POS_TEST_NEAR_DEG;
    test->speed_far = ANGLE_POS_TEST_SPEED_FAR;
    test->speed_near = ANGLE_POS_TEST_SPEED_NEAR;
    test->dir_sign = ANGLE_POS_TEST_DIR_SIGN;
    test->stall_ms = ANGLE_POS_TEST_STALL_MS;
    test->dwell_ms = ANGLE_POS_TEST_DWELL_MS;

    AnglePositionTest_Restart(test);

    return true;
}

void AnglePositionTest_SetSequence(AnglePositionTest_Handle *test,
                                   const uint16_t *seq, uint16_t seq_count)
{
    if ((test == NULL) || (seq == NULL) || (seq_count == 0U))
    {
        return;
    }

    test->seq = seq;
    test->seq_count = seq_count;

    AnglePositionTest_Restart(test); /* 新序列从头开始跑 */
}

void AnglePositionTest_Restart(AnglePositionTest_Handle *test)
{
    if (test == NULL)
    {
        return;
    }

    if (test->motor != NULL)
    {
        RZ7899_Stop(test->motor, RZ7899_STOP_BRAKE); /* 先停住再说 */
    }

    test->index = 0U;
    test->done = 0U;
    test->fault = 0U;
    test->step = 0U;
    test->target_deg = 0U;
    test->err_deg = 0;
    test->cmd = 0;
    test->dwell_cnt = 0U;
    test->stall_cnt = 0U;
    test->best_err = 0;
}

void AnglePositionTest_Update(AnglePositionTest_Handle *test)
{
    uint8_t done;
    int16_t target;
    int32_t err;
    int32_t abs_err;
    int32_t tol;
    int32_t restart_thr;
    int16_t desired;

    if ((test == NULL) || (test->motor == NULL) || (test->pot == NULL) ||
        (test->seq == NULL) || (test->seq_count == 0U))
    {
        return;
    }

    /* ---- 1. 刷新角度反馈 (低延迟采样链: 8 次平均, IIR 默认关闭) ---- */
    PotAngle_Update(test->pot);

    /* ---- 2. 当前目标: 序列跑完后停在最后一个角度继续调节 ---- */
    done = (test->index >= test->seq_count) ? 1U : 0U;
    target = (int16_t)test->seq[(done != 0U) ? (uint32_t)(test->seq_count - 1U)
                                             : (uint32_t)test->index];

    err = (int32_t)target - (int32_t)test->pot->angle_deg;
    abs_err = (err < 0) ? -err : err;
    tol = (int32_t)test->tol_deg;

    /* 已停机时用更宽阈值才重新启动: 形成迟滞, 防止刹停瞬间来回抖 */
    restart_thr = (test->cmd == 0) ? (tol + 2) : tol;

    /* 发布观测值 (Ozone Watch) */
    test->target_deg = (uint16_t)target;
    test->step =
        (done != 0U) ? (uint8_t)test->seq_count : (uint8_t)(test->index + 1U);
    test->done = done;
    test->err_deg = (int16_t)err;

    /* ---- 3. 期望速度: 到位=0; 否则按距离远近选速度, 方向由误差符号决定 ----
     */
    if (abs_err <= restart_thr)
    {
        desired = 0; /* 到位 */
    }
    else
    {
        int16_t mag = (abs_err <= (int32_t)test->near_deg) ? test->speed_near
                                                           : test->speed_far;
        desired = (int16_t)(((err > 0) ? mag : -mag) * test->dir_sign);
    }

    /* ---- 4. 进展看门狗: 误差必须持续变小, 否则判定异常 ---- */
    if (desired == 0)
    {
        test->best_err = INT32_MAX; /* 停机: 重置进展记录 */
        test->stall_cnt = 0U;
    }
    else if (test->cmd == 0)
    {
        test->best_err = abs_err; /* 本次启动第一拍: 记录基线 */
        test->stall_cnt = 0U;
    }
    else if (abs_err <= test->best_err - 2)
    {
        test->best_err = abs_err; /* 有进展 (又靠近 >=2°): 重新计时 */
        test->stall_cnt = 0U;
    }
    else if (test->stall_cnt < test->stall_ms)
    {
        test->stall_cnt++;
        if (test->stall_cnt >= test->stall_ms)
        {
            test->fault = 1U; /* 拉不近: 顶死/卡死/方向反了 */
        }
    }

    if ((test->fault != 0U) && (desired != 0))
    {
        desired = 0; /* 故障锁定: 停机; AnglePositionTest_Restart() 可恢复 */
    }

    /* ---- 5. 到位停稳计时: 停稳且在容差带内 -> 到时自动进入下一个目标 ---- */
    if ((test->fault == 0U) && (desired == 0))
    {
        if (test->dwell_cnt < test->dwell_ms)
        {
            test->dwell_cnt++;
        }
        if ((test->dwell_cnt >= test->dwell_ms) && (done == 0U))
        {
            test->index++; /* 下一步 (最后一个完成时 index 达到序列长度) */
            test->dwell_cnt = 0U;
        }
    }
    else
    {
        test->dwell_cnt = 0U;
    }

    /* ---- 6. 只在指令变化时下发 (Start 软启动; 到点立即制动) ---- */
    if (desired != test->cmd)
    {
        if (desired == 0)
        {
            RZ7899_Stop(test->motor, RZ7899_STOP_BRAKE);
        }
        else
        {
            RZ7899_Start(test->motor, desired);
        }
        test->cmd = desired;
    }
}
