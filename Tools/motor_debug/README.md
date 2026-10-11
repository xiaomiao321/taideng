# 电机与电位器调试上位机

通过 USART3 调试串口控制 M1～M8，同时查看 POT1～POT6。MCU 烧录一次调试固件后，后续切换电机、调整方向、占空比和点动时长都在窗口完成。

## 使用

1. 在 `MCU/software/test_bringup` 构建 Debug 固件，按工程现有 F5 流程烧录。当前 `main.c` 已接入串口调试模式，上电全部电机停止。
2. USB 转串口使用 3.3V TTL：适配器 TX 接 PB11/USART3_RX，RX 接 PB10/USART3_TX，GND 共地。电机由板上的电机电源供电。
3. 关闭占用同一 COM 口的串口助手。运行 `dist/MotorDebug.exe`，或双击 `start.bat`（需要 Python 3.10 及以上和 Tkinter；首次安装 pyserial）。
4. 选择 COM 口，点击连接。程序会发送 PING 和 STOP，收到 MCU 应答后显示已连接；不会自动启动电机。
5. 默认 M1、80% 占空比、500ms 点动。点击正转/反转点动测试，或使用持续运行。滑块改变参数，点击运行才生效；切换电机选择会先发送全部停止。
6. 点击全部停止，按 ESC，或按板上的 HOME 停止。HOME/断线/串口错误后会锁定停止，松开 HOME 后点击“全部停止 / 解锁”，再重新发运行命令。

数值区显示六路原始 ADC 值（0～4095）和相对基准的变化量；首次采样自动设置基准，按钮可以重新设置。曲线保留最近 30 秒，可勾选需要显示的通道。电机与电位器的对应关系尚未标定，界面不会把某个 POT 预设为某台电机的角度。

上位机每 200ms 发送 PING；MCU 最后一次有效 PING 超过 1000ms 后停止并锁定。主动断开或关闭窗口会尽力发送 STOP；电缆拔出、进程退出时依靠 MCU 心跳超时停止。重新连接和恢复心跳不会自动恢复运行。

## 模拟模式

无需 MCU：

```powershell
python app.py --demo
# 或
.\dist\MotorDebug.exe --demo
```

模拟模式的电位器响应是生成的数据，只用于检查界面，不代表实际电机与电位器映射。

## 串口协议 v1

115200 / 8N1，无流控。ASCII 文本，每条以 LF 结束，兼容 CRLF。序号 0～65535。

| 请求 | 含义 |
|---|---|
| `PING 1` | 更新心跳；返回 `ACK 1 PING` |
| `RUN 2 M1 800 500` | M1 正转，800‰，运行 500ms |
| `RUN 3 M8 -800 0` | M8 反转，800‰，持续运行，仍需心跳 |
| `STOP 4 ALL` | 全部滑行停止；HOME 松开后清除停止锁定 |

RUN：电机 M1～M8，非零占空比 −1000～1000‰，时长 0～60000ms。运行前必须有最近 1 秒内的 PING，且不能处于锁定状态。新 RUN 先停全部电机，再等待 100ms 启动；点动时长从实际开始驱动时计算。

| 回复 / 上报 | 含义 |
|---|---|
| `ACK 2 QUEUED` | 接受运行请求，等待启动；实际指令状态由 STATE 上报 |
| `ACK 4 STOP` | 停止并解锁 |
| `ACK 4 STOP_LOCKED` | 已停止，HOME 仍有效，保持锁定 |
| `ERR 2 LOCKED` | 停止锁定，拒绝运行 |
| `ERR 2 NO_HEARTBEAT` | 无新鲜心跳，拒绝运行 |
| `ERR 2 PARAM` | 命令/参数错误 |
| `POT 12345 1000 1500 2000 2500 3000 3500` | MCU 毫秒时间戳 + 六路原始 ADC，每 100ms 上报 |
| `STATE 12345 1 800 0 0` | 时间戳、电机号（0=停止）、有符号指令占空比、是否等待启动、是否锁定 |
| `ADCERR 12345 -6` | 采样失败，本次不返回 POT；界面标记数据无效 |
| `EVENT DONE` | 点动完成 |
| `EVENT HOME` / `TIMEOUT` / `UART_ERROR` / `BAD_LINE` / `HARDWARE` | 停机原因 |

STATE 表示电机控制指令，不表示测得的转速或确认电机已停稳。六路 ADC 使用现有扫描 DMA 接口，单次等待上限 5ms；串口每次发送等待上限 15ms。HOME 在主循环检查，也检查中断累计次数。UART 接收使用中断和 256 字节环形缓冲；超长行、非法字符、接收溢出和串口错误会停止并锁定。停止属于滑行，不使用短路制动。

## 文件与检查

- `app.py`：桌面界面、串口工作线程和模拟模式。
- `protocol.py`：上位机文本协议和电位器数据模型。
- `../../MCU/software/test_bringup/App/src/debug_control.c`：独立于 HAL 的命令与电机计时状态。
- `../../MCU/software/test_bringup/App/src/debug_console.c`：USART3 接收、硬件接口和 ADC 上报。
- `../../MCU/software/test_bringup/HostTest/debug_control_selftest.c`：MCU 控制逻辑主机测试。

```powershell
python -m unittest -v
# 在 MCU/software/test_bringup 下：
gcc -std=c11 -Wall -Wextra -Werror -I App/inc App/src/debug_control.c HostTest/debug_control_selftest.c -o HostTest/selftest_debug_control.exe
.\HostTest\selftest_debug_control.exe
```

实板首次联调时依次确认：连接后停止、M1 点动方向、点动到时停止、六路 ADC 更新、HOME 停机，以及持续运行时拔掉串口后的超时停机。模拟模式和主机测试不替代这些实板检查。
