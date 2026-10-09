# AItaid 板卡—MCU 通信协议

文档版本：2.0  
协议版本：`0x02`

SDK 0.3.0 保持本文0x02基本控制帧兼容。主机现在区分ACK_RECEIVED（接受）与APPLIED（关联的新状态中目标和实态均匹配），STOP取消旧motion排队/重试并优先发送；新模式、关节等能力默认禁用。扩展命令、主机提交结果、状态时间/陈旧语义、生命周期安全和固件待验收说明见[MCU扩展协议与安全控制](MCU扩展协议与安全控制.md)。该文档的新扩展不是当前实物能力声明。

> `0x02` 是不兼容升级：Android 不再发送 M1～M4 电机位图，而只发送停止、前进、后退、左转、右转等宏观运动指令。MCU 负责把运动意图转换为实际电机组合。协议 `0x01` 的 `SET_MOTORS` 不得与本版本混用。

## 1. 适用范围

本协议用于 RK3566 Android 板卡通过 USB 转 TTL 串口模块控制下位机 MCU，当前包括：

- 灯光总开关和亮度（0～100%）；
- 停止、前进、后退、左转、右转；
- 实际动作、目标动作、速度和故障状态回读；
- 状态查询、ACK、CRC、超时重试和断线安全停止。

Android 是通信主机，MCU 是从机。Android 只表达设备级运动意图，不依赖轮子数量、接线顺序、转向结构或电机驱动实现。

## 2. 物理连接

```text
RK3566 USB Host ─ USB转TTL模块 ─ MCU UART
                                  TX  ←→ RX
                                  RX  ←→ TX
                                  GND ─── GND
```

- 推荐使用 3.3 V TTL，必须与 MCU IO 电平一致，不能接 RS-232 电平。
- 模块 TX 接 MCU RX，模块 RX 接 MCU TX，双方 GND 共地。
- MCU 独立供电时，不连接 USB 转串口模块的 VCC，避免反向供电。
- 电脑模拟 MCU 时需要两只交叉连接的 USB 转 TTL 模块，或一块能枚举为 USB CDC Device 的 MCU 开发板。电脑和板卡的两个 USB Host 口不能直接模拟串口。

## 3. 串口参数

| 参数 | 值 |
|---|---:|
| 波特率 | 115200 baud |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验位 | None |
| 流控 | None |
| 多字节整数 | 小端序 |

Android 支持常见 CDC/ACM、FTDI、PL2303、CP210x、CH340/CH341 USB 串口芯片。

## 4. 帧格式

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | SOF1 | 固定 `0xA5` |
| 1 | 1 | SOF2 | 固定 `0x5A` |
| 2 | 1 | VERSION | 本版本为 `0x02` |
| 3 | 1 | FLAGS | 标志位 |
| 4 | 1 | SEQ | 帧序号，0～255 循环 |
| 5 | 1 | CMD | 命令字 |
| 6 | 2 | LENGTH | Payload 长度，小端 |
| 8 | N | PAYLOAD | 命令数据，当前最大 64 字节 |
| 8+N | 2 | CRC16 | CRC16/MODBUS，小端 |

CRC 计算范围从 `VERSION` 到 Payload 最后一个字节，不包括帧头和 CRC 自身。参数：初值 `0xFFFF`、反射多项式 `0xA001`、结果不异或。

### 4.1 FLAGS

| 位 | 名称 | 说明 |
|---:|---|---|
| bit0 | RESPONSE | 该帧是响应 |
| bit1 | ACK_REQUIRED | 接收方必须返回 ACK |
| bit2 | ERROR | 响应表示错误 |
| bit3～7 | RESERVED | 发送方写 0，接收方忽略 |

## 5. 命令定义

### 5.1 `0x01 PING`

链路检测命令，Payload 预留为 Android 运行时间 `uint32`。当前 Android 使用周期性 `GET_STATUS` 维持链路，不额外发送 PING。

### 5.2 `0x10 SET_LIGHT`

Android → MCU，FLAGS 包含 `ACK_REQUIRED`，Payload 固定 4 字节。

| 偏移 | 字段 | 说明 |
|---:|---|---|
| 0 | LIGHT_FLAGS | bit0：灯光打开；其他位写 0 |
| 1 | BRIGHTNESS | 0～100，百分比 |
| 2 | RESERVED_FLAGS | 写 0 |
| 3 | RESERVED | 写 0 |

关闭灯光时 MCU 可以保留亮度值，下一次打开继续使用。亮度为 0 和灯光关闭是两个独立状态。

### 5.3 `0x11 SET_MOTION`

Android → MCU，FLAGS 包含 `ACK_REQUIRED`，Payload 固定 4 字节。

| 偏移 | 字段 | 说明 |
|---:|---|---|
| 0 | MOTION | 宏观运动指令，见下表 |
| 1 | SPEED | 0～100，运动速度百分比 |
| 2 | CONTROL_FLAGS | 当前写 0，预留 |
| 3 | RESERVED | 写 0 |

| MOTION | 名称 | 语义 |
|---:|---|---|
| `0x00` | STOP | 停止所有运动输出；SPEED 必须为 0 |
| `0x01` | FORWARD | 整机向前运动 |
| `0x02` | BACKWARD | 整机向后运动 |
| `0x03` | TURN_LEFT | 整机原地或按底盘能力向左转 |
| `0x04` | TURN_RIGHT | 整机原地或按底盘能力向右转 |
| `0x05`～`0xFF` | RESERVED | 当前必须返回参数错误 |

当前 Android 对非停止动作发送 SPEED=`100`，接口已保留后续速度调节能力。MCU 必须原子地应用一个动作，并自行完成各电机方向、PWM、刹车和互锁控制。Android 不发送任何单电机启停位。

STOP 的安全优先级最高：MCU 收到合法 STOP 后应先停止运动输出，再返回 ACK；不得等待其他队列任务完成。

### 5.4 `0x20 GET_STATUS`

Android → MCU，Payload 长度为 0。MCU 返回 `STATUS`，无需另发 ACK。Android 正常连接后每 2 秒查询一次。

### 5.5 `0x21 STATUS`

MCU → Android，FLAGS 包含 `RESPONSE`，Payload 固定 16 字节。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | LIGHT_FLAGS | bit0：实际灯光开关 |
| 1 | 1 | BRIGHTNESS | 实际亮度 0～100 |
| 2 | 1 | MOTION | 当前实际动作 |
| 3 | 1 | TARGET_MOTION | MCU 最近接受的目标动作 |
| 4 | 1 | SPEED | 当前实际速度 0～100 |
| 5 | 1 | TARGET_SPEED | 目标速度 0～100 |
| 6 | 1 | FAULT_FLAGS | 整机运动故障位；0 表示无故障 |
| 7 | 1 | STATUS_FLAGS | 状态扩展位，当前写 0 |
| 8 | 4 | UPTIME_SECONDS | MCU 运行秒数，`uint32` |
| 12 | 2 | PROTOCOL_ERROR_COUNT | 协议错误累计数，`uint16` |
| 14 | 2 | RESERVED | 写 0 |

UI 以实际 `MOTION` 和 `SPEED` 为准。当实际动作和目标动作不同，UI 会同时显示两者。电机级故障诊断属于 MCU 内部实现，如需上传可汇总到 `FAULT_FLAGS`，不恢复单电机控制接口。

### 5.6 `0x7F ACK`

MCU → Android，FLAGS 包含 `RESPONSE`；错误时同时包含 `ERROR`。Payload 固定 4 字节。

| 偏移 | 字段 | 说明 |
|---:|---|---|
| 0 | ACKED_CMD | 被确认的命令字 |
| 1 | RESULT | 结果码 |
| 2 | DETAIL | 命令相关错误详情，成功为 0 |
| 3 | RESERVED | 写 0 |

| RESULT | 含义 |
|---:|---|
| `0x00` | 成功 |
| `0x01` | 未知命令 |
| `0x02` | Payload 长度错误 |
| `0x03` | 参数值错误 |
| `0x04` | MCU 忙 |
| `0x05` | 硬件故障 |
| `0xFF` | MCU 内部错误 |

## 6. 应答、重试和安全策略

- Android 对控制帧设置 `ACK_REQUIRED`。
- ACK 使用与请求相同的 SEQ。
- Android 等待 ACK 500 ms，超时最多重发 2 次，一条控制指令最多发送 3 次。
- MCU 按 `(SEQ, CMD)` 识别重复帧：可以重发 ACK，但不能重复产生非幂等副作用。
- Android 每 2 秒发送 `GET_STATUS`；串口读写异常后每 3 秒尝试重新连接。
- MCU 连续 5 秒没有收到任何合法 Android 帧时，必须把实际动作和目标动作置为 STOP、速度置 0。灯光可以保持最近状态。
- CRC、长度、版本或参数不合法的帧不得改变任何硬件输出。

## 7. 完整报文示例

打开灯光并设为 50%，SEQ=`0x01`：

```text
A5 5A 02 02 01 10 04 00 01 32 00 00 B9 17
```

以 75% 速度前进，SEQ=`0x02`：

```text
A5 5A 02 02 02 11 04 00 01 4B 00 00 38 1B
```

停止，SEQ=`0x03`：

```text
A5 5A 02 02 03 11 04 00 00 00 00 00 88 3D
```

查询状态，SEQ=`0x04`：

```text
A5 5A 02 00 04 20 00 00 01 03
```

确认第一条灯光指令：

```text
A5 5A 02 01 01 7F 04 00 10 00 00 00 96 12
```

## 8. 接收状态机要求

1. 搜索连续帧头 `A5 5A`，丢弃此前噪声。
2. 至少收到 8 字节后读取 LENGTH。
3. LENGTH 大于 64 时视为非法，继续搜索下一帧头。
4. 等待完整的 `10 + LENGTH` 字节。
5. 校验 CRC；失败时从当前帧头后一字节继续同步。
6. 校验 VERSION、CMD、长度、动作编码、速度和保留位。
7. 原子地应用控制，再返回 ACK；状态查询返回 STATUS。

## 9. 测试步骤

1. 交叉连接两只 3.3 V USB 转 TTL 模块，或连接支持 USB CDC Device 的 MCU 开发板。
2. PC 安装模拟器依赖：

   ```powershell
   python -m pip install -r mcu-tools\requirements.txt
   python -m serial.tools.list_ports
   ```

3. 启动电脑端 MCU 模拟器：

   ```powershell
   python mcu-tools\mcu_simulator.py --port COM7
   ```

4. 在板卡 UI 点击“刷新设备”，首次连接时允许 USB 权限。
5. 连接成功后测试灯光、停止、前进、后退、左转、右转，并检查 STATUS 回读。
6. 拔出串口模块，确认控件禁用；重新插入或点击“刷新设备”，确认自动恢复。
7. 运行协议测试：

   ```powershell
   python -m unittest discover -s mcu-tools -p "test_*.py"
   .\gradlew.bat :app:testDebugUnitTest
   ```

## 10. MCU 最小实现清单

- 115200/8N1 UART 和 RX 环形缓冲；
- 帧头、长度、协议版本和 CRC16/MODBUS 校验；
- `SET_LIGHT`、`SET_MOTION`、`GET_STATUS`、`ACK`、`STATUS`；
- 宏观动作到实际电机组合的唯一映射；
- STOP 优先处理、5 秒通信超时安全停止；
- 按 `(SEQ, CMD)` 去重；
- 保存协议错误计数并通过 STATUS 上报。
