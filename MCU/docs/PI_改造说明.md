# M1–M6 关节 PI 控制改造说明

本文档对应覆盖层 `taideng_pi/`。覆盖层按仓库原目录结构存放：`taideng_pi/software/` 覆盖到仓库的 `software/`，`taideng_pi/Tools/motor_debug/` 覆盖到仓库的 `Tools/motor_debug/`。所有代码引用格式为 `文件名:行号`；标“改造前”的行号指原仓库版本，标“现”的行号指覆盖层内的版本。

## 1. 改动概述

### 1.1 文件清单

| 路径（相对仓库根） | 作用 |
| --- | --- |
| `software/App/inc/joint_control.h` | `joint_pi_t`、`JOINT_MODE_PI`、`JOINT_HOLD`、v5 标定格式、`joint_set_pi()` / `joint_pi_defaults()` 声明 |
| `software/Config/app_config.h` | 默认模式改 PI；PI 全局常量与 M1–M6 分方向参数表 |
| `software/App/src/joint_control.c` | PI 控制律、自适应制动、HOLD 漂移监视、v5 导入导出、旧记录迁移 |
| `software/App/src/joint_service.c` | 启动时依次尝试 v5/v4/v3/v2 记录，读出旧记录后自动升级保存 |
| `software/App/src/debug_console.c` | 新增 `JPI`（读/写分方向 PI 参数）与 `JBL`（写实测背隙）命令，`JPI` 行随 `JGET` 上报 |
| `software/build_debug.sh` | 覆盖源码后编译调试版（`APP_MODE=0`） |
| `software/build_android.sh` | 覆盖源码后编译安卓版（`APP_MODE=2`） |
| `Tools/motor_debug/protocol.py` | 支持 `JPI` / `JBL`，`JT` / `JSTATE` 接受 `HOLD` 阶段 |
| `Tools/motor_debug/pi_tune.py` | 配套调试程序：阶跃采集与指标、回程误差测量、参数下发、无硬件自检 |
| `docs/PI_改造说明.md` | 本文档 |

### 1.2 行为变化一览

- 默认控制方式由比例式改为比例积分式（`Config/app_config.h:110`），比例式、开关式、轨迹式的代码保留，可用 `JTUNE` 切回。
- 占空比不再阶跃：起步 150 ms 加起转前馈，之后按 3000‰/s 斜坡逼近目标值（`joint_control.c:1080` 起 `pi_apply()`）。
- 制动点不再固定：按实测转速预估滑行距离，提前制动（`joint_control.c:1200` 起 `tick_pi()`）。
- 判 `REACHED` 后进入 `HOLD` 阶段，继续监视 1 s，漂出容差会重新纠正（`joint_control.c:1247` 起 `tick_hold()`）。
- 短行程不再误报 `DIRECTION`：只在起步后 1 s 内判方向（`joint_control.c:1384-1387`）。
- 标定记录升到 v5，仍能读 v2/v3/v4；v4 及更旧的记录若为比例式，可自动迁移成 PI（见第 8 节）。

## 2. 致命问题与修复对照

| # | 问题（改造前位置） | 影响 | 修复 | 优先级 |
| --- | --- | --- | --- | --- |
| 1 | 占空比阶跃、末端 30% 下限恒速爬行（改造前 `joint_control.c:1046-1060`） | 末端速度不收敛，是过冲主因 | `pi_apply()`：起转前馈 + 分方向 PI + 3000‰/s 斜坡；慢速段改用 `kp_slow` 并把限幅压到 60% | P0 |
| 2 | 制动点固定 `|e|≤tolerance`、`brake_ms` 单值（改造前 `:986-1006`） | 高速必过冲、低速提前停 | `tick_pi()`：按 `|v|×(brake_ms+100ms)` 加余量预估滑行距离，提前制动；制动后清积分 | P0 |
| 3 | 过冲后 bang-bang 反向（改造前 `:1024-1044`） | 容差带内来回 2–3 次 | 提前制动 + 慢速段降增益/降限幅；换向仍保留 100 ms 停顿与制动 | P1 |
| 4 | 判 `REACHED` 后不再纠正（改造前 `:281-302` `finish()` 直接 coast） | 漂出容差无人管 | `HOLD` 阶段监视 `HOLD_MS=1000`，漂出 `tol+HOLD_HYS` 用限幅 PI 拉回，不重复发 `JRESULT` | P1 |
| 5 | 短行程 `DIRECTION` 判据过低（改造前 `:1014-1018`，3° 行程阈值仅 8°） | 高速过冲误报故障并停机 | PI 下只在 `now-start_ms≤DIR_CHECK_MS(1000)` 内判方向（`joint_control.c:1384-1387`） | P1 |
| 6 | 回程误差无实测、`approach` 默认全关（`app_config.h:201-220`） | 双向定位差一个背隙 | 新增 `JBL` 写入实测背隙；`pi_tune.py backlash` 自动测量；是否启用单向逼近由整定决定 | P2 |
| 7 | 积分饱和无处理（新增风险） | 长时间大误差后超调 | 条件积分：输出饱和且误差同向时回退本拍积分；`i_max` 逐方向限幅 | P0 |

补充修复（审查阶段发现的实现缺陷，改造中顺手修掉）：

- `joint_cal_import()` 原来先读 `blob[0]/blob[1]` 再检查 `size`，短包会越界读；现在先判 `size<4`（`joint_control.c:1607` 起）。
- 运动中断（`joint_cancel`）原来只处理 `active`，`HOLD` 阶段无法取消；现在同时清 `holding`（`joint_control.c:364`）。
- `joint_clear_faults`、标定、限位设置等入口统一用 `is_busy()`，避免 `HOLD` 期间改参数（`joint_control.c:327`、`:359`）。

## 3. PI 控制设计

### 3.1 控制律

对关节 i，误差 `e = goal - angle`（毫度），物理方向 `dir = sign(e)`，参数索引 `idx = (dir × cal.direction > 0) ? 0 : 1`（0 = 角度增大方向，1 = 角度减小方向），`d = |e|`。

```
前馈 ff   = kick 窗口内 ? duty_min[idx] : duty_min[idx] × run_pct / 100
比例 kp   = d > slow_band ? kp_fast[idx] : kp_slow[idx]
积分 I    = clamp(I + ki[idx] × e × dt / 1000, ±i_max[idx])
合力 u    = dir × ff + kp × e / 1000 + I
```

约束与后处理（均在 `pi_apply()`，`joint_control.c:1080`）：

1. 限幅 `cap = min(pi.duty_max[idx], cal.duty, MOTOR_DUTY_SMALL_MAX)`；慢速段（`d ≤ slow_band`）再乘 `SLOW_CAP_PCT=60%`。
2. 条件积分：`|u| > cap` 且 `u` 与 `e` 同向时，撤销本拍积分累加。
3. 摩擦下限：远离目标（`d > tol+500`）且 `|u| < duty_min` 时，强制 `|u| = duty_min` 并冻结积分，防止积分把合力压到摩擦区以下导致停转和 `STALL`。
4. 斜坡：`duty_cmd` 每拍最多变化 `SLEW_PERMILLE_S × dt`（默认 3000‰/s，10 ms 一拍约 30‰），至少 1‰。
5. 输出 `duty = cal.direction × lround(u)`（`u` 已带符号，不能再乘 `dir`）；`|duty| < MOTOR_DUTY_MIN(100)` 时输出 0。

`dt` 取 `(now - tick_ms)/1000`，限制在 0–50 ms；`tick_ms` 只在进入 PI、松开制动、换向完成时重置，不能每拍重置（否则 `dt=0`，斜坡退化为每拍 1‰）。

### 3.2 时序

```
新命令 → SETTLE（100 ms 制动/静止等待）→ MOVING
   ├─ 距目标 > 制动距离：PI 驱动（斜坡）
   ├─ 进入制动距离且朝目标运动：brake()（brake_ms，默认 50 ms）
   ├─ 制动结束：滑行 COAST_MS=100 ms 的余量已计入制动距离，重新 PI
   └─ |e| ≤ tolerance 连续 200 ms → JRESULT REACHED → HOLD
HOLD：|e| ≤ tol+HOLD_HYS 输出 0；漂出则限幅 PI；1 s 后退出，phase=DONE
```

### 3.3 参数与整定起点

全局常量（`app_config.h:161-194`）：`KP_FAST=40`、`KP_SLOW=15`、`KI=8`、`I_MAX=200`、`DUTY_MIN=300`、`DUTY_MAX=700`、`SLOW_BAND=8000`、`RUN_PCT=80`、`SLEW=3000`、`KICK_MS=150`、`BRAKE_SPEED=3000`、`BRAKE_MARGIN=300`、`COAST_MS=100`、`SLOW_CAP_PCT=60`、`HOLD_MS=1000`、`HOLD_HYS=500`、`DIR_CHECK_MS=1000`。

逐关节表（`app_config.h:211-237`），格式 `{角度增大方向, 角度减小方向}`：

| 关节 | kp_fast | kp_slow | ki | i_max | duty_min | duty_max |
| --- | --- | --- | --- | --- | --- | --- |
| M1 | 40/40 | 15/15 | 8/8 | 200/200 | 300/300 | 700/700 |
| M2 | 40/40 | 15/15 | 8/8 | 200/200 | 300/300 | 700/700 |
| M3 | 60/30 | 25/10 | 12/6 | 260/160 | 380/250 | 800/600 |
| M4 | 40/40 | 15/15 | 8/8 | 200/200 | 300/300 | 700/700 |
| M5 | 40/40 | 15/15 | 8/8 | 200/200 | 300/300 | 700/700 |
| M6 | 60/30 | 25/10 | 12/6 | 260/160 | 380/250 | 800/600 |

单位：kp 为千分比/度，ki 为千分比/(度·秒)，i_max/duty_min/duty_max 为千分比（占空比）。

**这些全部是整定起点，不是实测结论。** M3/M6 的不对称只表达“举升侧需要更大占空比与增益”，哪一侧是举升取决于装配，必须在台架上确认后改表或下发 `JPI`。

### 3.4 推荐整定顺序

1. **方向与起转占空比**：手动发 `JMOVE` 小步，确认正占空比使角度增大；用逐级增加的固定占空比找每个方向的最小起转值，写入 `duty_min`（`JPI` 或改表）。
2. **kp_fast**：先 `ki=0`、`slow_band` 保持 8000，阶跃 10°–30°，从 20 起每档加 10，直到出现明显过冲再回退一档。目标：无持续振荡，过冲 < 1°。
3. **kp_slow**：观察末端 3° 内是否来回；来回则降 `kp_slow`（15 → 10 → 8），直到一次进入容差。
4. **ki**：从小值（4）加起，消除重力/摩擦造成的稳态偏差；加到稳态误差 < 0.5° 且不增加过冲为止；`i_max` 取“起转占空比 − 动摩擦占空比 + 50”量级。
5. **斜坡与制动**：定位时间偏长先加 `SLEW`（3000 → 5000），过冲变大再加 `BRAKE_MARGIN` 或降 `BRAKE_SPEED`；`brake_ms` 用实测：断电滑行时间。
6. **背隙**：`pi_tune.py backlash --center 135000 --delta 5000`，把差值用 `JBL` 写入；若回程误差 < 容差，可不启用单向逼近。

每步用 `pi_tune.py step --to <目标> --csv 记录.csv` 留档，记录 `JPI` 参数。

## 4. 调试版说明

- 编译：`./build_debug.sh /path/to/taideng`（或手工 `cmake --preset Debug && cmake --build --preset Debug`），`APP_MODE` 默认 0（`app_config.h:27-29`）。
- 调试台走 USART3（PB10/PB11，115200 8N1），提供 `JGET`/`JMOVE`/`JTUNE`/`JPI`/`JBL`/`JTRACE` 等命令。
- `JPI` 读：`JPI <t> <M>`，回一行 `JPI t M kpf0 kps0 ki0 im0 dmin0 dmax0 kpf1 kps1 ki1 im1 dmin1 dmax1 slow run flags`。
- `JPI` 写：`JPI <t> <M> <15 个非负整数>`，顺序同上；写入前校验 `duty_min ≥ 100`、`duty_min ≤ duty_max ≤ 1000`、`kp ≤ 1000`、`run_pct ∈ [10,100]`。
- `JBL <t> <M> <dir> <backlash>`：`dir=±1`，写 `approach=dir`、`backlash`、`overshoot=max(3×backlash, 3000)` 并保存。
- `JTRACE <t> <M> 1` 打开 10 ms 一行的 `JT` 流；关闭用 `JTRACE <t> <M> 0`。
- 调试版与安卓版共用同一套 PI 代码，不存在两套控制律。

## 5. 安卓版说明

- 编译：`./build_android.sh /path/to/taideng`，等价于仓库 README 的 `cmake --preset Debug -B build/Android "-DCMAKE_C_FLAGS=-DAPP_MODE=2"`。
- `APP_MODE=2` 时只初始化 `app_init()`，不启动 USART3 调试台（`Core/Src/main.c` 的 `#if APP_MODE`）；`JPI`/`JBL`/`JTRACE` 在安卓版不可用。
- USART1 二进制协议、SDK 命令语义、`JRESULT` 上报格式未改；关节 `phase` 多了 `HOLD`，安卓侧若按字符串解析需要容忍新值（数值枚举仍为 0–5，`HOLD=5`）。
- 标定流程：先用调试版完成标定与整定并保存，再烧安卓版；参数存在片内 Flash，不随固件切换丢失。
- 安卓版形态（是否作为独立工程、是否随核心板 OTA）仓库未定义，按“同一工程的 APP_MODE=2 构建”处理。

## 6. 调试程序说明

`Tools/motor_debug/pi_tune.py`，依赖 `pyserial==3.5`（`requirements.txt` 已含），运行时需要 Python ≥ 3.10（与仓库上位机一致；Python 3.7 会在导入 `joint_model.py` 时报 `int | None` 语法错）。

```bash
python pi_tune.py --port COM5 --joint 3 step --to 165000 --csv m3_step.csv
python pi_tune.py --port COM5 --joint 3 backlash --center 135000 --delta 5000
python pi_tune.py --port COM5 --joint 3 get
python pi_tune.py --port COM5 --joint 3 set-pi --values 60 25 12 260 380 800 30 10 6 160 250 600 8000 80 0
python pi_tune.py sim          # 无硬件，验证指标计算链路
```

`step` 打开 `JTRACE`、发 `JMOVE`、收 `JRESULT`，输出：起点/终点、峰值、过冲、稳定时间（进入 ±tolerance 并保持）、稳态误差、来回次数、峰值占空比、饱和样本数；`--csv` 保存 `tick,raw,spread,angle,target,duty,phase`。

`backlash` 从目标两侧各 `delta` 处逼近同一点，两次 `JSTATE` 落点之差即回程误差（含重复性），据结果用 `JBL` 写入。

## 7. 验证方法与验收

已完成（主机侧）：

- `gcc -std=c11 -Wall -Wextra -Wpedantic -O1` 编译 `joint_control.c`、`joint_traj.c` 无警告。
- 主机仿真（非交付物）：静摩擦 300‰、动摩擦 240‰ 的一阶对象，30° 阶跃无过冲、无来回，3° 阶跃 630 ms 稳定；到达点落在 2° 容差边缘（约 1.95°），与比例式行为一致，说明“居中”需要靠更小容差或 HOLD 继续纠偏，不能靠 PI 单独保证。
- `python -m py_compile protocol.py pi_tune.py` 通过；`pi_tune.py sim` 指标链路正常。

台架验收（待做，缺实测数据）：

1. M1–M6 每个方向 3°/10°/30° 阶跃各 5 次：无 `STALL`/`DIRECTION`/`TIMEOUT`，过冲 ≤ 1°，稳态误差 ≤ 0.5°，3° 稳定时间 ≤ 1.5 s。
2. 连续 20 次往返：`HOLD` 不反复抖动，最终落点分散 ≤ 1°。
3. 断使能/急停/HOME 键：`joint_cancel` 能退出 `HOLD`，电机 coast。
4. 5 s 链路超时、看门狗复位后参数仍在（v5 记录）。

## 8. 兼容性与回滚

### 8.1 标定记录

| 版本 | 每关节 | 总长 | 内容 |
| --- | --- | --- | --- |
| v2 | 28 B | 172 B | 标定 |
| v3 | 36 B | 220 B | 标定 + 控制参数 |
| v4 | 44 B | 268 B | 标定 + 控制参数 + 单向逼近 |
| v5 | 72 B | 436 B | v4 + PI 参数（两方向各 6×u16 + slow_band + run_pct + flags） |

- 启动时 `joint_service.c` 依次尝试 v5/v4/v3/v2；读到旧版本后按 v5 重新保存。
- `JOINT_PI_MIGRATE_LEGACY=1` 时，v2–v4 中 `mode==P` 的记录迁移为 PI：沿用旧 `kp` 与 `duty_min` 填两个方向，其余用默认表；`ki`、限幅、分方向差异必须重新整定。
- v5 记录以记录为准，不再迁移。
- `JTUNE` 仍可切回 `mode=0/1/2`，PI 参数原样保留。

### 8.2 回滚

- 固件回滚：烧回原固件即可；但**旧固件不认识 v5 记录**（长度校验失败会当作未标定），关节需要重新标定，或先备份 v5 blob 再写回。
- 参数回滚：`JPI` 逐项写回旧值；不要用 `JTUNE` 改 `mode` 代替参数回滚。
- 代码回滚：覆盖层是增量文件，用 git `checkout` 还原 `software/` 与 `Tools/motor_debug/` 即可。

## 9. 待确认事项

- (待补充：M3/M6 哪个方向是举升/下放) 表里的不对称是按“负载大的一侧给大增益”猜的，必须台架确认后调整。
- (待补充：各关节实测起转占空比、动摩擦占空比、断电滑行时间) 现有 `duty_min=300`、`brake_ms=50` 是旧数据，M3/M6 的 380/250 更是估计值。
- (待补充：真实负载与配重) 未测负载下的 `ki` 与 `i_max` 只能给起点。
- (待补充：安卓核心板是否解析 `phase` 字符串) 若解析，需要加入 `HOLD`，否则按数值 5 处理。
- (待补充：上位机 Python 版本与 pyserial 部署方式) 仓库要求 ≥3.10，现场机器需确认。
- (待确认：是否接受到达点落在容差边缘) 若要求居中，建议把 `tolerance` 降到 1°，或在 `HOLD` 内继续小步纠偏（会增加静止时的驱动次数与发热）。
