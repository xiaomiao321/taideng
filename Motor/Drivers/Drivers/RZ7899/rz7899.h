/**
  ******************************************************************************
  * @file    rz7899.h
  * @brief   RZ7899 H 桥电机驱动库 (双路 PWM 控制)
  *
  * 硬件连接 (以当前工程为例):
  *   TIM2_CH1 / PA0 -> FI (Pin 2, 正转输入)
  *   TIM2_CH2 / PA1 -> BI (Pin 1, 反转输入)
  *   电机接在 FO (Pin 5,6) 与 BO (Pin 7,8) 之间
  *   STM32 GND 与 RZ7899 GND 必须共地
  *
  * RZ7899 真值表 (FI, BI):
  *   (0,   0  ) -> FO/BO 高阻,      电机滑行停止
  *   (PWM, 0  ) -> 正转,            平均电压 ≈ 电机电源 × 占空比
  *   (0,   PWM) -> 反转
  *   (1,   1  ) -> FO/BO 同时拉低,  绕组短路, 快速制动
  *
  * 使用前提:
  *   1. 定时器必须已由 CubeMX 初始化 (MX_TIMx_Init), 两个通道都配置为
  *      PWM Generation, PWM mode 1 / Polarity High / 初始 Pulse = 0;
  *   2. 本库只写 CCR 比较寄存器, 不改动 PSC/ARR, PWM 频率由 CubeMX 决定;
  *   3. 库内所有函数均为非阻塞 (内部不含任何延时): 接口只"设定目标",
  *      换向死区、启动助推、斜坡均由 RZ7899_Update() 推进 —— Update 每调用
  *      一次 = 经过一个 PWM 周期, 必须放在定时器更新中断里调用
  *      (20kHz -> 每 50us 一次), 否则死区/助推/斜坡都不会推进;
  *   4. 时序参数以"时间 (ms)"给出 (启停斜坡; 可选助推), RZ7899_Init() 会从
  *      htim 自动算出实际 PWM 频率并换算成周期数; 运行期间不要修改定时器
  *      的 ARR, 否则占空比换算基准会失效.
  ******************************************************************************
  */

#ifndef RZ7899_H
#define RZ7899_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "stm32f1xx_hal.h" /* 更换芯片系列时改为对应的 stm32xxxx_hal.h */

/** @brief 速度刻度上限: 0 ~ RZ7899_SPEED_MAX 对应占空比 0.0% ~ 100.0% */
#define RZ7899_SPEED_MAX 1000

/* ---------------- 时间与 PWM 周期 ----------------
   时序参数按"时间 (ms)"定义, 周期数由驱动自动换算 ——
   RZ7899_Init() 从 htim 读出 PSC/ARR 与定时器时钟, 算出实际 PWM 频率:
      周期数 = 时间(ms) × 实际频率(Hz) / 1000
   所以改了 CubeMX 的 PSC/ARR 也不需要手动同步任何数字;
   启动/停止时间还可以在运行时用 RZ7899_SetRampTimes() 修改. */

/** @brief 换向死区周期数: 两路归零后等待的 PWM 周期数 (100 = 5ms @20kHz)
  * @note  等待电机绕组电流衰减, 避免"原方向残余电流 + 新方向电压"造成冲击.
  *        非阻塞: 由 RZ7899_Update() 逐周期计时, 等待期间两路保持 0%. */
#ifndef RZ7899_DIR_SWITCH_CYCLES
#define RZ7899_DIR_SWITCH_CYCLES 100U
#endif

/** @brief 启动助推周期数, 0 = 关闭 (1000~2000 = 50~100ms @20kHz)
  * @note  先满占空比冲一下再进入斜坡, 用于克服静摩擦/减速箱阻力/电刷死点. */
#ifndef RZ7899_START_KICK_CYCLES
#define RZ7899_START_KICK_CYCLES 0U
#endif

/** @brief 启动助推占空比刻度 (1000 = 100%) */
#ifndef RZ7899_START_KICK_DUTY
#define RZ7899_START_KICK_DUTY 1000U
#endif

/** @brief 启动斜坡时间 (ms) 默认值: 在这段时间内线性逼近目标占空比
  * @note  RZ7899_Init() 按实测 PWM 频率自动换算成周期数 (100ms @20kHz -> 2000);
  *        之后可用 RZ7899_SetRampTimes() 随时修改, 不必重新编译.
  *        调大启动更柔和 (限制启动电流/电源压降), 调小则启动更快. */
#ifndef RZ7899_START_RAMP_MS
#define RZ7899_START_RAMP_MS 100U
#endif

/** @brief 停止斜坡时间 (ms) 默认值: 软停止时用这段时间把占空比降到 0
  * @note  RZ7899_Init() 按实测 PWM 频率自动换算成周期数 (100ms @20kHz -> 2000);
  *        到达 0 后两路输出 0%, 进入滑行待机; 之后可用 RZ7899_SetRampTimes() 修改.
  *        需要立即停住时用 RZ7899_STOP_BRAKE (短路制动), 不走本斜坡. */
#ifndef RZ7899_STOP_RAMP_MS
#define RZ7899_STOP_RAMP_MS 100U
#endif

/* ---------------- 电机标定参数 (应用层使用; 库内部不引用) ----------------
   用法: 把对应电机的一组值传给 RZ7899_SetDutyLimits(), 换电机只改选择处:
       实际占空比 = min + (max - min) × |速度| / 1000
   下面的数值是实测结果, 请按标定结论更新. */

/** @brief 小型蜗杆减速直流电机 (已标定, 此前测试一直用它)
  * @note  实测: 50% 不能起转, 55% 可稳定起转 -> min 取 550 (留 5 个刻度余量);
  *        max 取 1000 (不额外限幅). */
#define RZ7899_MOTOR_WORM_MIN_DUTY 550U
#define RZ7899_MOTOR_WORM_MAX_DUTY 1000U

/** @brief 履带驱动直流电机 (已标定)
  * @note  实测: 约 540 可稳定起转 (从 550 继续下探所得) -> min 取 540;
  *        max 取 1000 (不额外限幅). 若个别情况下最低速启动吃力,
  *        可再上调 3~5 (如 545) 留余量. */
#define RZ7899_MOTOR_TRACK_MIN_DUTY 540U
#define RZ7899_MOTOR_TRACK_MAX_DUTY 1000U

/** @brief 停止方式 */
typedef enum
{
    RZ7899_STOP_COAST = 0, /**< 软停止: 先斜坡减速到 0, 再高阻滑行 (冲击小, 推荐) */
    RZ7899_STOP_BRAKE      /**< 短路制动: FI=1, BI=1, 电机绕组短路, 停止快 */
} RZ7899_StopMode;

/** @brief 当前旋转方向 */
typedef enum
{
    RZ7899_DIR_STOP = 0, /**< 停止 (滑行或制动) */
    RZ7899_DIR_FORWARD,  /**< 正转: FI=PWM, BI=0 */
    RZ7899_DIR_REVERSE   /**< 反转: FI=0, BI=PWM */
} RZ7899_Direction;

/** @brief 内部状态机状态 (由 RZ7899_Update 推进) */
typedef enum
{
    RZ7899_STATE_IDLE = 0, /**< 空闲 (已停止) */
    RZ7899_STATE_SWITCH,   /**< 换向死区等待: 两路保持 0%, 等绕组电流衰减 */
    RZ7899_STATE_KICK,     /**< 启动助推中 (可选) */
    RZ7899_STATE_RAMP,     /**< 斜坡中: 升/降速或软停止, 由 ramp_total 个周期内线性逼近目标 */
    RZ7899_STATE_RUN       /**< 已到达目标占空比 */
} RZ7899_State;

/**
  * @brief 电机实例句柄
  * @note  一个句柄对应一个电机 + 一个定时器; 同一定时器的两个通道驱动一个
  *        RZ7899; 多电机时每个电机使用独立句柄 (可共用或分用定时器).
  */
typedef struct
{
    TIM_HandleTypeDef *htim;  /**< 提供 PWM 的定时器句柄 (由调用者拥有) */
    uint32_t channel_forward; /**< 正转通道, 接 RZ7899 FI */
    uint32_t channel_reverse; /**< 反转通道, 接 RZ7899 BI */

    uint32_t period_counts; /**< 缓存 ARR + 1, 用于占空比换算 */
    uint32_t pwm_freq_hz;   /**< 实际 PWM 频率 (Hz), RZ7899_Init 从 htim 自动算出 */

    uint16_t min_duty; /**< 输出占空比下限刻度 (低速死区), 默认 0 */
    uint16_t max_duty; /**< 输出占空比上限刻度 (高速限幅), 默认 1000 */

    RZ7899_Direction direction; /**< 当前方向 */
    uint16_t speed;             /**< 速度指令刻度 0 ~ 1000 (限幅后, 目标值) */
    uint16_t duty;              /**< 当前实际输出占空比刻度 0 ~ 1000 (斜坡中会变化) */

    RZ7899_State state;         /**< 内部状态机状态 */
    uint32_t cycles;            /**< 本次斜坡计划已经过的 PWM 周期数 */
    uint16_t target_duty;       /**< 目标输出占空比刻度 (映射后) */
    uint16_t ramp_start_duty;   /**< 本次斜坡计划的起始占空比刻度 */
    uint32_t ramp_total;        /**< 本次斜坡计划的总周期数 (剩余预算 = ramp_total - cycles) */
    uint32_t ramp_start_cycles; /**< 启动斜坡周期数 (由启动时间按实测频率换算, 默认 100ms) */
    uint32_t ramp_stop_cycles;  /**< 停止斜坡周期数 (由停止时间按实测频率换算, 默认 100ms) */
    bool stopping;              /**< true=本次斜坡是软停止: 到达 0 后转滑行待机 */
    bool soft_start;            /**< true=RZ7899_Start(斜坡); false=RZ7899_SetSpeed(立即) */
} RZ7899_Handle;

/* -------------------------------------------------------------------------- */
/* 接口                                                                        */
/* -------------------------------------------------------------------------- */

/**
  * @brief  初始化驱动并启动两路 PWM 输出 (占空比 0%, 电机不动)
  * @param  motor           电机句柄
  * @param  htim            已初始化的定时器句柄 (如 &htim2)
  * @param  forward_channel 正转通道 (如 TIM_CHANNEL_1 -> FI)
  * @param  reverse_channel 反转通道 (如 TIM_CHANNEL_2 -> BI)
  * @retval true  成功
  * @retval false 参数非法或定时器尚未初始化
  * @note   内部会先把两路 CCR 清零再启动 PWM, 即使 CubeMX 初始 Pulse 被
  *         误设, 调用本函数瞬间电机也不会转动.
  * @note   本函数会从 htim 读出 PSC/ARR 与定时器时钟, 自动算出实际 PWM
  *         频率, 并按 RZ7899_START_RAMP_MS / RZ7899_STOP_RAMP_MS 把启停
  *         斜坡时间换算成周期数 —— 改 CubeMX 定时器参数后无需手动同步.
  */
bool RZ7899_Init(RZ7899_Handle *motor,
                 TIM_HandleTypeDef *htim,
                 uint32_t forward_channel,
                 uint32_t reverse_channel);

/**
  * @brief  设置输出占空比上下限 (低速死区 + 高速限幅)
  * @param  motor    电机句柄
  * @param  min_duty 占空比下限刻度 0 ~ 1000, 对应电机的"最小启动占空比"
  * @param  max_duty 占空比上限刻度 0 ~ 1000, 用于限制最高速/峰值电流
  * @retval true  成功
  * @retval false 参数非法 (min_duty > max_duty 或 max_duty > 1000)
  * @note   设置后指令刻度 1 ~ 1000 线性映射到 [min_duty, max_duty]:
  *         实际占空比 = min_duty + (max_duty - min_duty) * |speed| / 1000
  *         例 max=800 / min=100 时, 指令 200 -> 实际 24%, 指令 1000 -> 实际 80%.
  *         对之后的 RZ7899_SetSpeed()/RZ7899_Start() 速度指令生效; 制动(100%)不受该限制影响.
  */
bool RZ7899_SetDutyLimits(RZ7899_Handle *motor, uint16_t min_duty, uint16_t max_duty);

/**
  * @brief  运行时修改启动/停止斜坡时间 (单位: ms)  [非阻塞]
  * @param  motor    电机句柄 (需已 RZ7899_Init)
  * @param  start_ms 启动斜坡时间 (ms); 0 视为 1 个 PWM 周期
  * @param  stop_ms  停止斜坡时间 (ms); 0 视为 1 个 PWM 周期
  * @retval true  成功
  * @retval false 句柄无效
  * @note   按 RZ7899_Init 时实测的 PWM 频率换算, 立即适用于之后的斜坡;
  *         正在进行的斜坡不受影响. 例: RZ7899_SetRampTimes(&motor1, 50, 200);
  */
bool RZ7899_SetRampTimes(RZ7899_Handle *motor, uint16_t start_ms, uint16_t stop_ms);

/**
  * @brief  设置速度 (含方向), 负数反转  [非阻塞]
  * @param  motor 电机句柄
  * @param  speed +1 ~ +1000 正转, -1 ~ -1000 反转, 0 = 软停止 (斜坡降到 0)
  *               超出 ±1000 会被限幅
  * @note   实际输出占空比会先经过 min_duty ~ max_duty 映射 (见
  *         RZ7899_SetDutyLimits), 默认 min=0 / max=1000 时即 |speed| 本身.
  *         本函数只"下达指令", 内部不含任何延时:
  *           - 同方向: 立即写入新占空比;
  *           - 方向改变: 两路先归零, 之后由 RZ7899_Update() 计满
  *             RZ7899_DIR_SWITCH_CYCLES 个 PWM 周期, 再加载新方向占空比.
  */
void RZ7899_SetSpeed(RZ7899_Handle *motor, int16_t speed);

/**
  * @brief  平滑启动 (可选启动助推 + 占空比斜坡)  [非阻塞]
  * @param  motor 电机句柄
  * @param  speed ±1 ~ ±1000, 语义与 RZ7899_SetSpeed 相同 (0 = 软停止)
  * @note   与 RZ7899_SetSpeed 的区别:
  *           1. 可选"启动助推": 先满占空比冲 RZ7899_START_KICK_CYCLES 个周期,
  *              帮助电机越过静摩擦/减速箱阻力/电刷死点;
  *           2. "斜坡": 在启动斜坡时间 (RZ7899_START_RAMP_MS, 默认 100ms,
  *              由驱动按实测 PWM 频率换算成周期数) 内线性逼近目标占空比,
  *              限制启动电流, 避免电源塌压或过流保护.
  *         本函数只"下达指令", 内部不含任何延时; 实际动作 (死区/助推/斜坡)
  *         由 RZ7899_Update() 逐周期推进.
  * @note   连续指令 (斜坡进行中再次调用 / 斜坡中调用停止): 不重新计时,
  *         而是"做减法" — 剩余预算 = ramp_total - cycles, 从当前占空比在
  *         剩余预算内重新瞄准新目标. 例如第 200 周期时改目标: 仍用剩余
  *         1800 个周期完成, 总时长不会因频繁指令而无限延长.
  */
void RZ7899_Start(RZ7899_Handle *motor, int16_t speed);

/**
  * @brief  状态机推进一个 PWM 周期: 处理换向死区 / 启动助推 / 斜坡  [非阻塞]
  * @param  motor 电机句柄
  * @note   **每调用一次 = 经过一个 PWM 周期**, 应放在定时器更新中断里调用
  *         (例如 TIM2 更新中断: 20kHz -> 每 50us 一次).
  *         库内其他接口都不含延时, 只有本函数逐周期推进输出:
  *           - 换向死区等待 (RZ7899_DIR_SWITCH_CYCLES 个周期);
  *           - 启动助推      (RZ7899_START_KICK_CYCLES 个周期);
  *           - 斜坡逼近目标  (启动 RZ7899_START_RAMP_MS / 停止
  *             RZ7899_STOP_RAMP_MS 毫秒换算的周期数内到达; 中途改目标按剩余预算).
  */
void RZ7899_Update(RZ7899_Handle *motor);

/**
  * @brief  停止电机  [非阻塞: 滑行停止走斜坡, 由 RZ7899_Update 逐周期推进]
  * @param  motor 电机句柄
  * @param  mode  RZ7899_STOP_COAST 软停止: 斜坡减速到 0 后滑行 (推荐)
  *               RZ7899_STOP_BRAKE 急停: 立即短路制动, 不经过斜坡
  * @note   软停止: 从当前占空比在停止斜坡时间 (RZ7899_STOP_RAMP_MS, 默认 100ms,
  *         可用 RZ7899_SetRampTimes 修改) 内线性降到 0, 到达后输出 0%/0%;
  *         若斜坡正在进行, 复用其剩余预算 (总时长不翻倍).
  *         制动会在桥臂和电机内产生较大制动电流, 高速时建议只保持
  *         几十~几百 ms, 随后再调用 RZ7899_STOP_COAST 转入滑行待机.
  */
void RZ7899_Stop(RZ7899_Handle *motor, RZ7899_StopMode mode);

/**
  * @brief  反初始化: 占空比归零并关闭两路 PWM 输出
  * @param  motor 电机句柄
  * @note   只关闭通道输出, 不关闭定时器时钟, 也不改动 GPIO 配置.
  */
void RZ7899_DeInit(RZ7899_Handle *motor);

/**
  * @brief  读取当前方向
  * @param  motor 电机句柄 (可为 NULL)
  * @retval 当前方向; 句柄无效时返回 RZ7899_DIR_STOP
  */
RZ7899_Direction RZ7899_GetDirection(const RZ7899_Handle *motor);

/**
  * @brief  读取当前速度指令刻度
  * @param  motor 电机句柄 (可为 NULL)
  * @retval 0 ~ 1000; 句柄无效时返回 0
  * @note   返回的是"指令值"而非实测转速; 软停止斜坡进行中为 0,
  *         实际输出占空比在 duty 字段中可见 (正在下降).
  */
uint16_t RZ7899_GetSpeed(const RZ7899_Handle *motor);

#ifdef __cplusplus
}
#endif

#endif /* RZ7899_H */
