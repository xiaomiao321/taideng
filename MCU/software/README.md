# 主控板固件

阅读指引：第 1 到 4 节是上手必读，第 5 节是调电机的流程，第 6 节是安全规则，附录是命令速查和相关文档。

## 1. 概况

[STM32F407VGT6](https://www.st.com/en/microcontrollers-microprocessors/stm32f407vg.html) 主控板的固件，裸机主循环，不用 RTOS。板上有：

- 8 路直流电机，每路一片 [RZ7899](https://so.szlcsc.com/global.html?k=RZ7899) H 桥。M1 到 M6 是关节电机，各带一个电位器 POT1 到 POT6，1:1 装在关节轴上；M7、M8 是履带电机，左 M7 右 M8。
- 7 路灯板 PWM：上灯板 UP1 到 UP4，下灯板 DOWN1 到 DOWN3。
- HOME 键、蜂鸣器、4 个状态灯、Linux 核心板电源键脉冲。

串口：

| 串口 | 引脚 | 用途 |
|---|---|---|
| [USART3](Core/Src/usart.c) | PB10 TX，PB11 RX | 调试终端，接电脑上的上位机，115200 8N1 |
| [USART1](Core/Src/usart.c) | PA9 TX，PA10 RX | 和安卓核心板通信的二进制协议，115200 8N1 |

## 2. 构建与烧录

需要的工具：

| 工具 | 说明 |
|---|---|
| [STM32CubeCLT](https://www.st.com/en/development-tools/stm32cubeclt.html) | 编译器、CMake、Ninja 都在里面。装好后设置环境变量 `STM32CLT_PATH` 指向安装目录 |
| [xPack OpenOCD](https://github.com/xpack-dev-tools/openocd-xpack/releases) | 烧录和调试 |
| [ST-LINK/V2](https://www.st.com/en/development-tools/st-link-v2.html) | SWD 连接，OpenOCD 配置在 `.vscode/openocd.cfg` |
| [Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug) | VS Code 插件，按 F5 编译、烧录并进入调试 |

命令行编译，在本目录下执行：

```powershell
cmake --preset Debug
cmake --build --preset Debug
```

产物是 `build/Debug/test_bringup.elf`。不想编译的话，可以直接烧 Release 里编译好的 hex，步骤见 [烧录说明.md](烧录说明.md)。用 VS Code 打开本目录，按 F5 会自动编译并烧录。`.vscode/launch.json` 里 OpenOCD 的路径写死成了 `D:/xpack-openocd-0.12.0-7`，换电脑后要改 `serverpath` 和 `searchDir` 两处。

标定和控制参数存在片内 Flash 的扇区 10 和 11，重新烧录程序不会清掉。

## 3. 两种模式

编译时用 `APP_MODE` 选择，默认是调试模式，定义在 [Config/app_config.h](Config/app_config.h)。

| 模式 | 值 | 用途 |
|---|---|---|
| [APP_MODE_DEBUG](App/src/debug_console.c) | 0 | USART3 文本命令，配合上位机调电机、标定关节、做板级测试。开放全部功能 |
| [APP_MODE_ANDROID](App/src/app_main.c) | 2 | USART1 二进制协议，给最终机器人用；USART3 只输出日志 |

编译安卓模式：

```powershell
cmake --preset Debug -B build/Android "-DCMAKE_C_FLAGS=-DAPP_MODE=2"
cmake --build build/Android
```

## 4. 代码结构

| 目录或文件 | 内容 |
|---|---|
| [Config/app_config.h](Config/app_config.h) | 全部可调参数：PWM 频率、占空比上下限、关节控制默认参数、超时。调参先看这里 |
| [App/](App/src) | 应用层。`joint_control` 关节闭环，`joint_service` 电位器采样和标定存取，`debug_control` 调试命令解析和安全规则，`debug_console` 把调试命令接到硬件，`debug_board` 板级测试，`app_main` 安卓模式，`chassis` 履带 |
| [BSP/](BSP/Inc) | 外设驱动：电机 H 桥、灯光 PWM、ADC、按键、串口、看门狗、参数 Flash |
| [Protocol/](Protocol/Inc) | 安卓二进制协议的帧解析和命令层 |
| [Core/](Core/Src) | STM32CubeMX 生成的初始化代码，按 CubeMX 的 USER CODE 规则修改 |
| [Core/Src/hal_callback_dispatch.c](Core/Src/hal_callback_dispatch.c) | HAL 中断回调只在这里实现，按外设转发给各驱动 |
| [HostTest/](HostTest/run.ps1) | 在电脑上用 gcc 跑的单元测试，不需要板子 |
| [Ref/](Ref/Drivers/README.md) | 早期的参考驱动，不参与编译 |

主循环的流程：

1. 每 10ms 对六路电位器各采样 16 次，去掉最大和最小各四分之一后取平均，再做三点中值滤波。
2. 推进 6 个关节的闭环。
3. 处理串口命令；调试模式下每 100ms 上报一次状态。
4. 喂看门狗，超时 1 秒。

中断里只收字节、置标志，处理都放在主循环里。

单元测试：

```powershell
powershell -ExecutionPolicy Bypass -File HostTest\run.ps1
```

需要电脑上有 gcc，可以用 MinGW。如果 gcc 不在 PATH 里，先设置 `$env:MCU_HOST_GCC` 指向 gcc.exe。改了 App 或 Protocol 下的代码后先跑一遍。

## 5. 调电机

上位机在仓库的 [Tools/motor_debug](../../Tools/motor_debug/README.md) 目录。运行 `python app.py` 需要 Python 3.10 以上和 pyserial；不连板子可以用 `python app.py --demo` 先熟悉界面。USB 转串口接 USART3，连接后上位机会自动发心跳。界面上的电机页、关节页和终端页都能完成下面的操作，终端里输入 `help` 可以查看命令。

建议的顺序：

1. 开环试转。在电机页或终端用 `motor M1 30 500`，确认方向和起转占空比。上电默认 5kHz 刹车 PWM，实测 20% 到 30% 就能转。改 PWM 用 `pwm 5 brake`，要先停电机。
2. 标定关节。`jnom` 是理论标定：给一个参考点的 ADC 读数和角度、电位器方向、电机正转方向、占空比、容差、允许范围。也可以用 CAL 实测两个点。标定会自动存进 Flash。
3. 设允许范围。用 `jlim M1 10 170` 限制在机构实际能转的范围内，避免顶到机械限位。
4. 调控制参数。`tune M1 p 1 30 50 80` 的意思是比例式控制，每度误差加 1% 占空比，起转 30%，到位和换向时制动 50ms，最高 80%。这组参数也存进 Flash。
5. 定位测试。`angle M1 90` 让关节转到 90°。
6. 记录数据。`trace M1 on` 每 10ms 上报一次读数、角度、占空比和参考位置，上位机自动存成 records 目录下的 CSV，用来分析过冲、抖动和跟踪误差。

没保存过参数的关节，默认用比例式控制，每度 1%，起转 30%，制动 50ms。这组数是 2026-10-24 在 M1 上实测的：3° 到 30° 的定位 1 到 1.5 秒到位，过冲不超过 2.4°。其他关节还没逐个调过。

### 轨迹式控制（试验中）

比例式和开关式在启动、换向和到位时占空比都会突变，到位时还会短路制动，电机和减速箱受冲击。轨迹式用 `tune M1 traj 5 25 80` 打开，意思是反馈每度 5%，实测起转占空比 25%，最高 80%。它的做法是：

- 规划一条五次多项式轨迹，起点和终点的速度、加速度都为 0，全程位置、速度、加速度连续；运动中换目标时从当前参考状态接着规划。
- 输出 = 速度前馈 + 摩擦前馈 + 对参考位置的 PI 反馈。摩擦前馈按参考速度算，起步时先预加到略低于起转占空比，再平滑过渡到动摩擦占空比。
- 到位时不制动，进入容差后输出 0。
- 可以规定逼近方向：反向到达时先越过目标一段，再低速回到目标，让减速箱总是同一侧齿面受力。方向应选逆着重力或负载的方向。

速度和加速度上限、速度前馈、积分系数、摩擦参数、各关节的逼近方向、背隙和超调量都在 [Config/app_config.h](Config/app_config.h) 的“轨迹式控制”一节，目前全是占位值，要在实物上辨识后修改，改完重新编译烧录。逼近方向默认不启用。用 `trace` 记录时，对比参考位置和实测角度就能看出跟踪误差。

在电脑上的仿真里（摩擦加背隙模型）它能平滑到位，但还没有在实物上验证过，默认控制方式仍是比例式。

关节运动中如果出现传感器失效、超时、越限、反向或停滞，固件会停机并报故障。判定阈值在 `app_config.h` 的关节闭环一节。

## 6. 安全规则

- 心跳：调试模式下 1 秒内没收到 PING，就停掉全部输出并锁定。安卓模式下 5 秒没收到合法帧，就停掉全部电机。
- HOME 键：按下立即停机。调试模式下会锁定，松开 HOME 后发 STOP ALL 才能解锁。
- 占空比：每路电机都受 `MOTOR_DUTY_MIN` 和各自上限约束，关节电机最高 100%，履带电机最高 80%。上位机的 `motor_limits.py` 镜像了这组限制，改的时候两边同步。
- 换向：电机启动或换向前先停 100ms。
- 改标定或控制参数时先停全部电机再写 Flash。擦除一个扇区最长约 2 秒，期间主循环会阻塞，所以不要在电机运动时写 Flash。

## 附录 A：调试命令速查

USART3 文本命令，每行一条，格式是命令名、序号、参数。上位机会自动加序号；手动发送时序号可以填任意 0 到 65535 的数。占空比单位是千分比，角度单位是毫度。

| 命令 | 参数 | 作用 |
|---|---|---|
| [PING](App/src/debug_control.c) | 无 | 心跳 |
| [STOP](App/src/debug_control.c) | ALL | 停止全部输出；HOME 已松开时同时解锁 |
| [INFO](App/src/debug_control.c) | 无 | 查询固件支持的功能 |
| [MSET](App/src/debug_control.c) | 掩码，8 个占空比，8 个时长 | 掩码 bit0 对应 M1，只改选中的电机；时长 0 表示一直转 |
| [MOFF](App/src/debug_control.c) | 无 | 停止全部手动电机 |
| [MPWM](App/src/debug_console.c) | kHz，0 或 1 | 设置 PWM 频率和衰减方式，1 为刹车；不带参数时是查询 |
| [LSET](App/src/debug_console.c) | 7 个占空比 | 设置 7 路灯光 |
| [JNOM](App/src/debug_console.c) | Mx 和 9 个参数 | 理论标定，参数顺序见 `debug_console.c` |
| [CAL](App/src/debug_console.c) | Mx 和 7 个参数 | 两点实测标定 |
| [JLIM](App/src/debug_console.c) | Mx，下限，上限 | 设置允许运动范围 |
| [JTUNE](App/src/debug_console.c) | Mx，模式，kp，起转占空比，制动毫秒，可选最高占空比 | 设置控制参数，模式 0 开关式、1 比例式、2 轨迹式 |
| [JMOVE](App/src/debug_console.c) | Mx，目标角度，超时毫秒 | 关节定位 |
| [JGET](App/src/debug_console.c) | Mx | 查询标定和状态 |
| [JTRACE](App/src/debug_console.c) | Mx，0 或 1 | 开关每 10ms 一行的数据记录，末尾一项是参考位置 |
| [TEST](App/src/debug_board.c) | 项目名，参数，时长 | 板级测试，共 29 项 |
| [CONFIRM](App/src/debug_board.c) | PASS 或 FAIL | 人工确认板级测试结果 |

固件会周期上报 POT、STATE、MSTATE、LSTATE、BOARD、KEY、JSTATE 这几种状态行，格式见上位机 README。

## 附录 B：相关文档

| 文档 | 内容 |
|---|---|
| [上位机 README](../../Tools/motor_debug/README.md) | 上位机各页面的用法、文本协议的完整格式 |
| [固件编码规范](MCU_固件编码规范.md) | 命名、注释和分层约定 |
| [仓库 README](../../README.md) | 整个项目的概况 |
