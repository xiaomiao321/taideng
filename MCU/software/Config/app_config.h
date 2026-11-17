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
/** @brief USART1 二进制 Android 协议，USART3 只出日志；取值 2 沿用旧编号。 */
#define APP_MODE_ANDROID (2)

/** @brief 固件入口；编译时也可用 -DAPP_MODE=... 覆盖。 */
#ifndef APP_MODE
#define APP_MODE APP_MODE_DEBUG
#endif

/**
 * @brief 独立看门狗标称超时，单位毫秒。
 *
 * 主循环单次最长阻塞约 100ms（串口限时发送），LSI 最快时实际超时约为标称值
 * 的 0.68 倍，仍有充足余量。
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
 * 开环 MSET、板级测试、关节开关式和比例式的输出都不会超过它；
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

/**
 * @brief 每个采样周期连续扫描的轮数，去掉最大最小各四分之一后取平均。
 *
 * 2026-11-17 在样机上测得：关节静止时六路读数各自跳动约 2.5% 到 3%，相邻通道
 * 同向、隔开几路的通道反向，是 ADC 参考电压上几十 kHz 的纹波被依次采样的结果。
 * 硬件不改，就靠多采样平均：ADC 连续转换，一轮六路约 27µs，64 轮约 1.75ms，
 * 均匀铺满几十个纹波周期。原来是 16 轮，每轮由软件单独启动，间隔不均匀。
 * 最多 BSP_ADC_BURST_MAX_SCANS 轮；轮数乘 27µs 是每 10ms 里主循环被采样占用的
 * 时间。
 */
#define POT_OVERSAMPLE (64U)

/** @brief 电位器读数距上次采样超过该时间即视为失效，单位毫秒。 */
#define JOINT_SENSOR_STALE_MS (50U)

/**
 * @brief 电位器读数“抖得离谱”的阈值与连续次数（问题清单 B4）。
 *
 * spread 是单次 10ms 采样里 16 次过采样去掉两端各 1/4 后的极差，正常只有几个
 * 计数。碳膜磨损、滑片接触不良或断线时会突然跳到几十上百个计数；连续
 * JOINT_POT_SPREAD_SAMPLES 次超过 JOINT_POT_SPREAD_MAX 就锁存 SENSOR 故障并
 * 停下该关节，等读数恢复正常后由 joint_clear_faults() 自动清掉。只检查已标定的
 * 关节：没接电位器的通道输入悬空，读数本来就乱跳。
 *
 * 200 计数约合 13°（270° 行程对应 4095 计数），远大于正常噪声，又能在读数
 * 明显不可信时及时停下。现场标定后可按实测噪声收紧。
 */
#define JOINT_POT_SPREAD_MAX (200U)
#define JOINT_POT_SPREAD_SAMPLES (3U)

/**
 * @brief ADC 排名顺序到关节下标的映射（问题清单 B1）。
 *
 * Core/Src/adc.c 的扫描顺序是 rank1..6 = ADC_CHANNEL_12、11、10、13、14、15，
 * 即 PC0、PC1、PC2、PC3、PC4、PC5；而 Core/Inc/main.h 的网表把 PC0 标成
 * POT3、PC2 标成 POT1。两边都以“1”为起点，于是排第 1 的 M1 可能读到 M3 的
 * 电位器。哪一方是对的只能上板验证：手动转某一个关节，看调试终端的 POT 行里
 * 是哪一路在变（步骤见 docs/通讯与采样问题清单.md 的 B1 条）。
 *
 * 默认按顺序一致处理；实测确认 M1/M3 对调后，把下面改成 {2U, 1U, 0U, 3U, 4U, 5U}。
 * 改动只影响采样与关节的对应关系，不影响标定数据本身。
 */
#ifndef JOINT_ADC_ORDER
#define JOINT_ADC_ORDER {0U, 1U, 2U, 3U, 4U, 5U}
#endif

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
 * @brief 没保存过控制参数时的默认值：比例积分式。
 *
 * 参数只是整定起点：M1 的 5kHz 刹车 PWM 旧数据（3°～30° 约 1～1.5 秒、
 * 过冲不超过 2.4°）来自比例式，不能直接当作 PI 的验收数据。已保存到
 * Flash 的参数优先。
 */
#define JOINT_DEFAULT_MODE (JOINT_MODE_PI)
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

/* -------------------------------------------------------------------------- */
/* 比例积分式控制（JTUNE 模式 3）                                               */
/* -------------------------------------------------------------------------- */

/*
 * 下面全是整定起点，不是实测结论。每度误差的稳态占空比由 kp 和 ki 共同决定，
 * 起转占空比 duty_min 是静摩擦前馈。上板后按“先方向、再 kp、再 ki、最后
 * 斜坡与制动”的顺序整定，方法见 docs/PI_改造说明.md。
 */

/** @brief 误差大于慢速段门限时每度误差增加的占空比，千分比/度。 */
#define JOINT_PI_KP_FAST (40U)
/** @brief 慢速段每度误差增加的占空比，千分比/度；压低以避免末端来回。 */
#define JOINT_PI_KP_SLOW (15U)
/** @brief 积分增益，千分比/(度·秒)。 */
#define JOINT_PI_KI (8U)
/** @brief 积分项限幅，千分比；不小于起转占空比与动摩擦的差值。 */
#define JOINT_PI_I_MAX (200U)
/** @brief 起转占空比的默认值，千分比；逐关节表会覆盖它。 */
#define JOINT_PI_DUTY_MIN (300U)
/** @brief 最高占空比的默认值，千分比；逐关节表会覆盖它。 */
#define JOINT_PI_DUTY_MAX (700U)
/** @brief 慢速段门限，毫度；误差小于它时改用 kp_slow 并压低限幅。 */
#define JOINT_PI_SLOW_BAND (8000U)
/** @brief 已起步后的维持占空比占 duty_min 的百分比。 */
#define JOINT_PI_RUN_PCT (80U)
/** @brief 输出斜坡，千分比/秒；换向和起步时限制占空比变化率。 */
#define JOINT_PI_SLEW_PERMILLE_S (3000U)
/** @brief 每次起步（含换向后重新起步）加 duty_min 前馈的时长，毫秒。 */
#define JOINT_PI_KICK_MS (150U)
/** @brief 自适应制动的进入门限转速，毫度/秒；低于它只靠斜坡和 PI。 */
#define JOINT_PI_BRAKE_SPEED (3000)
/** @brief 自适应制动的距离余量，毫度。 */
#define JOINT_PI_BRAKE_MARGIN (300U)
/**
 * @brief 估算刹车距离时，在制动时长之外再加的滑行时间，毫秒。
 *
 * 提前刹车的距离按“转速 ×（制动时长 + 本项）+ 余量”估算。2026-11-17 在样机
 * M1 上实测：每秒 20 到 56 度时刹车后只再走 0.3° 到 2.2°，相当于 15 到 45ms，
 * 蜗轮减速箱自锁、几乎不滑行，所以取 0，只按制动时长（默认 50ms）估算。
 * 原来取 100，估出来的距离是实际的三倍多，关节总是早早刹住、停在近侧。
 * 各关节的差别用 JTUNE 的制动时长调。
 */
#define JOINT_PI_COAST_MS (0U)
/** @brief 慢速段的最高占空比占该方向 duty_max 的百分比。 */
#define JOINT_PI_SLOW_CAP_PCT (60U)
/** @brief 判到达后继续监视漂移的时长，毫秒；超时不再纠正。 */
#define JOINT_PI_HOLD_MS (1000U)
/** @brief 漂移超过容差加该值才重新纠正，毫度；避免在容差边界抖动。 */
#define JOINT_PI_HOLD_HYS (500)
/**
 * @brief 起步或换向后，读数变化超过这个量才认为连杆真的带着电位器动起来了，毫度。
 *
 * 在此之前占空比固定为该方向的起转值，既不因为误差大而往上加，也不受慢速段
 * 限幅的约束往下压。2026-11-17 在样机 M3 上实测：电位器和
 * 连杆之间有约 10° 的旷量，换向后连杆走完这一段读数才变；原来这段时间里占空比
 * 被比例项加到 65%，读数开始变时连杆已是每秒 142 度。取值要大于静止噪声
 * （约 0.4°）。
 */
#define JOINT_PI_BLIND_EXIT (800)

/**
 * @brief 已进入容差、等待判定到位期间，容差临时放宽的量，毫度。
 *
 * 略大于静止噪声的峰峰值即可。
 */
#define JOINT_PI_ARRIVE_HYS (400)

/**
 * @brief 各关节提前刹车时是否多留一个容差，依次为 M1～M6，1 表示多留。
 *
 * 多留时瞄准的是容差近侧的边缘，总是停得偏早，但几乎不会冲过头；不多留时瞄准
 * 目标本身，更准，偶尔会冲过头再反向修正。M3、M6 带重力负载，往下很快，
 * 冲过头后的反向修正又落在电位器的旷量里，所以多留。
 */
#define JOINT_PI_BRAKE_EARLY                                                   \
    {                                                                          \
        0U, 0U, 1U, 0U, 0U, 1U                                                 \
    }

/** @brief 起步后只在该时间内判方向错误，毫秒；短行程高速过冲不再误报。 */
#define JOINT_PI_DIR_CHECK_MS (1000U)

/**
 * @brief 旧记录（版本 2～4）里的比例式参数是否自动迁移成 PI。
 *
 * 迁移只沿用 kp 与 duty_min，ki、限幅和分方向差异必须重新整定；设为 0 时
 * 旧记录保持原控制方式，可用 JTUNE 手动切换。
 */
#define JOINT_PI_MIGRATE_LEGACY (1)

/**
 * @brief 各关节 PI 参数的整定起点，依次为 M1～M6，每个关节两个方向：
 *        [0] 角度增大方向，[1] 角度减小方向。
 *
 * M3、M6 是负载较大的关节，先给不对称的占空比与增益，方向与数值都要在
 * 台架上确认（哪一侧举升、哪一侧下放取决于装配，不能从代码推断）。
 */
#define JOINT_PI_KP_FAST_TABLE                                                 \
    {                                                                          \
        {40U, 40U}, {40U, 40U}, {60U, 30U}, {40U, 40U}, {40U, 40U}, {60U, 30U}  \
    }
#define JOINT_PI_KP_SLOW_TABLE                                                 \
    {                                                                          \
        {15U, 15U}, {15U, 15U}, {25U, 10U}, {15U, 15U}, {15U, 15U}, {25U, 10U}  \
    }
#define JOINT_PI_KI_TABLE                                                      \
    {                                                                          \
        {8U, 8U}, {8U, 8U}, {12U, 6U}, {8U, 8U}, {8U, 8U}, {12U, 6U}            \
    }
#define JOINT_PI_I_MAX_TABLE                                                   \
    {                                                                          \
        {200U, 200U}, {200U, 200U}, {260U, 160U}, {200U, 200U},                \
        {200U, 200U}, {260U, 160U}                                             \
    }
#define JOINT_PI_DUTY_MIN_TABLE                                                \
    {                                                                          \
        {300U, 300U}, {300U, 300U}, {380U, 250U}, {300U, 300U},                \
        {300U, 300U}, {380U, 250U}                                             \
    }
#define JOINT_PI_DUTY_MAX_TABLE                                                \
    {                                                                          \
        {700U, 700U}, {700U, 700U}, {800U, 600U}, {700U, 700U},                \
        {700U, 700U}, {800U, 600U}                                             \
    }

/* 轨迹式控制（JTUNE 模式 2）。以下数值都是占位值，要在实物上辨识后再改。 */

/** @brief 轨迹速度上限，毫度/秒；SDK 给的速度更小时用 SDK 的。占位值。 */
#define JOINT_TRAJ_VMAX_MDEG_S (30000)

/** @brief 轨迹加速度上限，毫度/秒²。占位值。 */
#define JOINT_TRAJ_AMAX_MDEG_S2 (60000)

/** @brief 单段轨迹的最短时长，单位毫秒。 */
#define JOINT_TRAJ_MIN_MS (300U)

/**
 * @brief 各关节的速度前馈，千分比/(度/秒)，依次为 M1～M6。
 *
 * 等于稳态时“占空比减动摩擦占空比”除以转速，用几档恒定占空比测稳态转速
 * 得到。占位值。
 */
#define JOINT_TRAJ_KV                                                          \
    {                                                                          \
        10U, 10U, 10U, 10U, 10U, 10U                                           \
    }

/** @brief 加速度前馈，千分比/(度/秒²)。占位值，先不用。 */
#define JOINT_TRAJ_KA (0)

/** @brief PI 反馈的积分系数，千分比/(度·秒)；比例系数用 JTUNE 的 kp。占位值。 */
#define JOINT_TRAJ_KI (50)

/** @brief 积分项的限幅，千分比。 */
#define JOINT_TRAJ_I_MAX (150)

/** @brief 静止起步时预加的占空比比起转占空比低这么多，千分比。占位值。 */
#define JOINT_TRAJ_PRELOAD_MARGIN (30)

/** @brief 动摩擦占空比占起转占空比的百分比。占位值。 */
#define JOINT_TRAJ_KINETIC_PCT (80)

/** @brief 摩擦补偿从预加量过渡到动摩擦量所用的参考速度，毫度/秒。占位值。 */
#define JOINT_TRAJ_FRICTION_SPEED (5000)

/** @brief 跟踪误差超过该值持续 JOINT_STALL_MS 判定停滞，毫度。 */
#define JOINT_TRACK_ERROR_MAX (10000)

/**
 * @brief 各关节规定的逼近方向的上电默认值，依次为 M1～M6：+1 最后一段角度
 *        增大，即从角度小的一侧到达目标；-1 最后一段角度减小；0 不限定。
 *
 * 三种控制方式都适用。应选与标定时相同的方向，或逆着重力、负载的方向。
 * 调试命令 JDIR 可以逐关节修改，并随标定保存到 Flash，Flash 里有记录时以
 * 记录为准。
 */
#define JOINT_APPROACH_DIR                                                     \
    {                                                                          \
        0, 0, 0, 0, 0, 0                                                       \
    }

/** @brief 各关节背隙的上电默认值，毫度。占位值，可用 JDIR 修改。 */
#define JOINT_BACKLASH_MDEG                                                    \
    {                                                                          \
        1000, 1000, 1000, 1000, 1000, 1000                                     \
    }

/**
 * @brief 各关节反向到达时越过目标的量的上电默认值，毫度。占位值，可用 JDIR
 *        修改。比例式和开关式到中间点附近一个容差内就折返，实际越过目标约为
 *        超调量减容差，所以至少取背隙加容差。
 */
#define JOINT_OVERSHOOT_MDEG                                                   \
    {                                                                          \
        3000, 3000, 3000, 3000, 3000, 3000                                     \
    }

/** @brief 单向逼近回程段的速度上限，毫度/秒，压低齿面重新接触时的冲击。占位值。 */
#define JOINT_RETURN_SPEED_MDEG_S (5000)

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

/**
 * @brief 调试终端周期状态上报的间隔，单位毫秒。
 *
 * 保持 100ms：非焦点的空闲关节每六个周期才轮到一次 JSTATE，上位机按 1 秒判断
 * 状态是否新鲜，周期加到 200ms 会让它们时有时无。带宽已由 460800 和发送队列
 * 解决，不需要靠降低周期来省。
 */
#define STATUS_REPORT_PERIOD_MS (100U)

/**
 * @brief JTRACE 相邻两行的最小间隔，单位毫秒。
 *
 * 采样 10ms 一次，取 10 即每个采样都上报，上位机的阶跃指标按这个间隔计算。
 * 需要把调试串口降回 115200 时可以改成 20，带宽减半，指标相应变粗。
 */
#define JOINT_TRACE_MIN_PERIOD_MS (10U)

/** @brief Android 协议失联保护：空闲时超过该时间没收到合法帧即停止全部电机，毫秒。 */
#define MCU_COMMAND_LINK_TIMEOUT_MS (5000U)

/**
 * @brief 运动期间的 Android 失联保护时限，单位毫秒（问题清单 A4）。
 *
 * 空闲时 5 秒失联只意味着没有新指令；一旦有电机在动，同样的 5 秒就变成最多
 * 5 秒的失控运动。所以运动期间改用更短的时限：主机（Android 侧）在关节动作、
 * 底盘运动期间必须至少每该时长发一帧，PING 或 GET_STATUS 都算。
 *
 * 目前与空闲时限相同，即这一档还没有启用：SDK 文档约定主机每 2 秒查询一次
 * 状态、下位机 5 秒无帧才停，运动中缩短到 1.5 秒会把正常运动中的底盘停掉。
 * 要缩短必须先和安卓端约定运动期间的发帧间隔，再把这里改成略大于它两倍的值。
 * 触发时停机并置 STATUS.status_flags 的 LINK_LOST 位。
 */
#define MCU_COMMAND_MOTION_LINK_TIMEOUT_MS (5000U)

/**
 * @brief Android SET_ARM_POSITION 的固件内部单次运动期限，单位毫秒。
 *
 * 超时置故障位 bit1；应长于 SDK 的关节观察期限（默认 10 秒）。
 */
#define MCU_ARM_MOVE_TIMEOUT_MS (15000U)

/**
 * @brief 各关节 HOME_ARM 的目标角度，毫度，依次为 M1～M6。
 *
 * 2026-11-01 定为电位器行程中点 135°，前提是关节按电位器转角 0～270° 标定；
 * 135° 不在某关节允许范围内时 HOME_ARM 回 INVALID_VALUE，不会运动。
 * MCU_ARM_HOME_NONE 表示该关节没有 HOME，HOME_ARM 回未知命令。
 * 须与 App 端能力描述里的 homePositionMilliDegrees 保持一致。
 */
#define MCU_ARM_HOME_NONE (INT32_MIN)
#define MCU_ARM_HOME_MDEG                                                      \
    {                                                                          \
        135000, 135000, 135000, 135000, 135000, 135000                         \
    }

/**
 * @brief Android SET_BUZZER 的发声频率，单位 Hz；蜂鸣器在约 2.7kHz 最响。
 */
#define MCU_BUZZER_FREQ_HZ (2700U)

/**
 * @brief SET_BUZZER 的图案表，按图案 ID 0～3 依次给出 {响毫秒, 停毫秒}。
 *
 * 0 关，1 长鸣，2 慢响（响 200 停 800），3 快响（响 100 停 100）。响为 0 表示
 * 不响，停为 0 表示一直响。图案一直保持到下一条 SET_BUZZER，失联停机时回到 0。
 * 须与 App 端能力描述里的 buzzerPatternIds 保持一致。
 */
#define MCU_BUZZER_PATTERNS                                                    \
    {                                                                          \
        {0U, 0U}, {1U, 0U}, {200U, 800U}, {100U, 100U}                         \
    }

/**
 * @brief 调试串口 USART3 的波特率，调试终端和日志共用。
 *
 * 六个关节同时运动再加 10ms 一行的数据记录，每秒约 11KB，115200 下刚好占满，
 * 所以默认用 460800。上位机连接时要选同样的波特率；改回 115200 可以配合旧版
 * 上位机，但数据记录和状态行会开始被丢弃。USART3 挂在 42MHz 的 APB1 上，
 * 460800 的实际误差约 0.16%。
 */
#define DEBUG_UART_BAUD (460800U)

/**
 * @brief 调试串口发送队列的字节数，必须是 2 的幂。
 *
 * 发送在中断里进行，主循环只把整行文本放进队列。4096 字节在 460800 下约 90ms
 * 发完，够放下一次周期状态加若干条回复。
 */
#define DEBUG_UART_TX_QUEUE_SIZE (4096U)

/**
 * @brief 发送队列留给回复的字节数。
 *
 * 周期状态和数据记录只在写入后还剩这么多空间时才入队，否则整行丢弃并计数；
 * 命令的回复和日志可以用到最后一个字节。
 */
#define DEBUG_UART_TX_REPLY_RESERVE (1024U)

/**
 * @brief 发送队列在回复之外再留给周期状态的字节数。
 *
 * 10ms 一行的数据记录量最大，队列紧张时先丢它：它要在写入后还剩"回复预留加
 * 本项"的空间时才入队。这样上位机赖以判断连接状态的周期状态不会被它挤掉。
 */
#define DEBUG_UART_TX_STATUS_RESERVE (1024U)

/** @brief HOME 键消抖时间，单位毫秒。 */
#define BSP_KEY_DEBOUNCE_MS (20U)

/**
 * @brief HOME 键状态快照的上报周期，单位毫秒。
 *
 * 主机超过 5 秒收不到报告就把按键状态当作未知，周期必须明显短于它。
 */
#define HOME_KEY_SNAPSHOT_PERIOD_MS (1000U)

#endif /* AITAID_APP_CONFIG_H */
