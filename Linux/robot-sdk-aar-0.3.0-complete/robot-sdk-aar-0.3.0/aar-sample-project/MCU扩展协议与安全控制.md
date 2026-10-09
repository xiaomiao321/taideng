# MCU 扩展协议与 SDK 0.3.0 安全控制

这是**等待 MCU 固件实现和实物验收的新规范**，不是当前实物已经支持的功能清单。SDK、PC 模拟器和单元测试实现了主机侧契约；未验证真实 MCU 的灯光模式、关节、HOME、分区灯、蜂鸣器、指示灯或软关机。SDK 不做蓝牙连接、服务端业务或自主动作决策。

## 能力启用与兼容性

原协议 `0x02` 的帧头、CRC16 Modbus、四字节 SET_LIGHT/SET_MOTION 和 16 字节 STATUS 均保持不变。默认 `McuCapabilities.LEGACY_V2` 只声明基本灯光开关/亮度和 STOP/FORWARD/BACKWARD/TURN_LEFT/TURN_RIGHT。所有扩展能力默认空或 false。未声明支持的扩展调用返回 `UNSUPPORTED`，不编码或发送扩展控制帧。

为避免未知命令在旧固件上的副作用，本版本**不发送自动能力探测或协商帧**。集成方只能在确认固件实现了本文 extension-v1 后，在构造控制器时显式传入经验证的 `McuCapabilities`。`firmwareDescriptorId` 必须非空；它是集成方的验证标识，并非 SDK 自动认证或硬件身份绑定。描述符应由受信任的固件配置/部署记录提供，不能把未经验证的业务输入直接转换为能力。

可选字段：`lightModes: Set<Int>`、`joints: List<McuJointCapability>`、`lightZoneIds: Set<Int>`、`buzzerPatternIds: Set<Int>`、`indicatorIds: Set<Int>`、`supportsSoftShutdown: Boolean`。所有 ID 都为固件定义的 0..255；SDK 不赋予某个模式 ID 固定名称。关节必须给出 `jointId`、`minPositionMilliDegrees`、`maxPositionMilliDegrees`、`maxSpeedMilliDegreesPerSecond`；范围/速度来自固件和机构实测，不能采用文档示例的 0–270°。只有声明 `homePositionMilliDegrees` 的关节才允许 HOME，此位置须在该关节范围内。

以下仅为 PC 模拟器固定测试夹具，不能用于真实板卡：

```json
{
  "firmwareDescriptorId": "aitaid-simulator-extension-v1",
  "protocolVersion": 2,
  "lightModes": [7],
  "joints": [{
    "jointId": 3,
    "minPositionMilliDegrees": -45000,
    "maxPositionMilliDegrees": 80000,
    "maxSpeedMilliDegreesPerSecond": 20000,
    "homePositionMilliDegrees": 0
  }],
  "lightZoneIds": [1], "buzzerPatternIds": [0, 1], "indicatorIds": [2],
  "supportsSoftShutdown": true
}
```

当前 SDK 接收 Kotlin 对象；上面的 JSON 是字段示意，不是一个已经实现的 JSON 解析入口。`protocolVersion` 必须为 2，extension-v1 通过显式描述符选择，不占用或修改旧版本的保留位。

`HOME_ARM`是主机发出的回原点动作命令，**不等于硬件HOME按键输入事件，也不证明校准已完成**。当前0x02/extension-v1没有约定physical HOME输入位、校准完成标志或相关上报事件。`McuJointSnapshot.physicalHomeInput`和`calibrationState`预留类型始终为UNKNOWN；SDK不伪造此类数据。需提供真实输入/校准信息时应先扩展固件协议。

## 扩展命令保留分配

所有帧仍使用 `A5 5A 02 FLAGS SEQ CMD LEN_LO LEN_HI PAYLOAD CRC_LO CRC_HI`。多字节有符号数均为二进制补码 i32、小端。每个控制帧要求 ACK (`FLAGS=0x02`)，每个保留字节必须为 0，固件对非零保留字节返回 INVALID_VALUE。

| CMD | 名称 | Payload（按偏移） |
| --- | --- | --- |
| 0x30 | SET_LIGHT_MODE | 0:模式 ID u8，1:亮度 0..100，2..3:保留；长度4 |
| 0x31 | SET_ARM_POSITION | 0:关节ID，1..3:保留，4..7:目标位置 milli-degree i32，8..11:正速度 milli-degree/s i32；长度12 |
| 0x32 | GET_EXTENDED_STATUS | 0:被查询控制命令，1:selector，2..3:保留；长度4，不要求ACK |
| 0x33 | EXTENDED_STATUS | 0:控制命令，1:selector，2:故障位，3:保留，4..7:actualValue，8..11:targetValue，12..15:actualSpeed，16..19:targetSpeed；长度20，RESPONSE标志 |
| 0x34 | HOME_ARM | 0:关节ID，1..3:保留；长度4 |
| 0x35 | SET_LIGHT_ZONE | 0:分区ID，1:开关0/1，2:亮度0..100，3:保留；长度4 |
| 0x36 | SET_BUZZER | 0:图案ID，1:音量0..100，2..3:保留；长度4 |
| 0x37 | SET_INDICATOR | 0:指示灯ID，1:开关0/1，2..3:保留；长度4 |
| 0x38 | SOFT_SHUTDOWN | 固定字节 `53 44 01 00`；长度4；固件须先停止所有执行器再执行电源管理 |

ACK 保持原 0x7F 格式：payload `acknowledgedCommand resultCode detail 00`，回显控制帧 sequence。GET_EXTENDED_STATUS 返回 EXTENDED_STATUS 并回显**查询帧** sequence，不回显旧控制帧 sequence。旧固件不支持扩展时应按原规范拒绝未知命令；SDK 默认不会依赖这一行为。

selector 是模式ID、关节ID、分区ID、蜂鸣图案ID或指示灯ID，与控制 payload[0] 相同。状态字段语义如下：

| 查询控制命令 | actualValue / targetValue | actualSpeed / targetSpeed | APPLIED条件 |
| --- | --- | --- | --- |
| SET_LIGHT_MODE | 实际/目标模式ID | 实际/目标亮度百分比 | 模式与亮度均等于提交值，故障位0 |
| SET_ARM_POSITION | 实际/目标关节位置 milli-degree | 实际/目标速度 milli-degree/s | 两位置均等于提交值，两速度均0，故障位0 |
| HOME_ARM | 实际/目标关节位置 milli-degree | 实际/目标速度 milli-degree/s | 两位置均等于已声明HOME位置，两速度均0，故障位0 |
| SET_LIGHT_ZONE | 实际/目标开关0/1 | 实际/目标亮度百分比 | 开关和亮度均等于提交值，故障位0 |
| SET_BUZZER | 实际/目标图案ID | 实际/目标音量百分比 | 图案与音量均等于提交值，故障位0 |
| SET_INDICATOR | 实际/目标开关0/1 | 均0 | 两开关均等于提交值，故障位0 |

关节位置采用严格 milli-degree 相等判定；固件应报告经过标定、量化到该单位的位置。若机构需要误差容限，必须先制定新的显式能力/协议约定；当前 SDK 不臆造容限。蜂鸣模式的APPLIED仅表示观察到该输出模式，不能证明用户听到了声音。SOFT_SHUTDOWN不执行状态APPLIED判定：ACK仅证明固件接受请求，断开不能证明电源已关闭；随后结果为DISCONNECTED或STATE_TIMEOUT，集成方必须从独立电源观测确认真正关机。

## 提交、结果与状态关联

读状态无需先驱动执行器：`requestExtendedStatus(commandType, selector)`只发送GET_EXTENDED_STATUS，返回`McuStatusQuerySubmission(sequence,command,selector,accepted,outcome)`。它仅检查已验证能力/参数/连接，不受动作的新鲜状态门控限制，也不会发送任何SET/HOME命令；accepted仅表示查询已排队。ARM_POSITION按关节ID查询角度，LIGHT_ZONE按分区ID查询灯路。LIGHT_MODE、ARM_HOME、BUZZER、INDICATOR同样可纯查询；基本灯光/运动仍用requestStatus，软关机不可查询为APPLIED。

`jointSnapshots()`列出描述符内全部关节；尚无回读或断开清理后的位置/速度/故障/时间为null、stale=true。收到与查询sequence及控制类型/selector一致的合法响应后更新数值，stale随单调时间动态计算；`extendedStatusSnapshot(commandType,selector)`提供其他扩展灯路/模式/蜂鸣/指示灯的缓存和动态陈旧标记。只读查询也可确认它请求前已收到ACK的同一待确认命令。模拟器--extensions在任何动作之前便提供测试关节的初态；真实固件须同样支持无动作回读。

原 `setLight(enabled, brightnessPercent)` / `setMotion(motion, speedPercent)` 源码调用仍可忽略返回值。现在返回 `McuCommandSubmission(commandId, sequence, command, accepted, outcome)`；`commandId` 是本次控制器生命周期内单调递增的主机ID，sequence是8位线协议ID。`accepted=true`表示主机接受排队，不能显示“动作成功”。非法参数不会再静默夹取到范围内，而是返回 INVALID_ARGUMENT。

`onCommandResult` 返回原 `McuCommandResult` 并增加 `commandId` 和 `receivedAtElapsedMs`。回调投递主线程；主线程上的提交调用可先登记commandId，再处理回调。ACK_RECEIVED是非终态，其余结果都是终态。旧 ACKNOWLEDGED 枚举保留兼容，但新版不再发出。

结果：ACK_RECEIVED、APPLIED、REJECTED（带MCU resultCode/detail）、TIMEOUT（ACK失联）、STATE_TIMEOUT（接受后未在期限内观察到应用）、STALE_STATUS、NOT_CONNECTED、WRITE_ERROR、DISCONNECTED、CLOSED、INVALID_ARGUMENT、UNSUPPORTED、SUPERSEDED、BUSY（普通sequence资源暂不可用）。

只有发送成功、收到成功ACK之后发出的状态查询，其对应的RESPONSE才用于确认APPLIED。基本motion的**实际motion、目标motion、实际speed、目标speed都须匹配**；灯光实际开关/亮度须匹配；故障位须0。查询前收到的状态、未经关联的主动上报或迟到状态不能确认新命令。

`McuStatus` 和 `McuExtendedStatus` 包含 `receivedAtElapsedMs`、`stale`、`relatedCommandIds`；时间使用Android单调时钟，不是UTC或MCU uptime。查询响应超过 `statusStaleAfterMs` 标为陈旧，并将关联待确认命令结束为STALE_STATUS。未关联状态仍可用于显示，但stale=true。已发布对象的stale字段是接收时的快照；持续显示时应调用 `controller.isStatusStale()`判断基本状态是否随时间过期。断开清除状态。

8位sequence有天然的关联限制；SDK为完成的命令/查询保留 `statusStaleAfterMs + stateTimeoutMs` 隔离期以避免近期迟到包套用到新命令，且保留sequence=0专供STOP。无限迟到/重放包无法由原0x02协议绝对识别；若固件链路可能无限缓存，必须在固件增加会话身份后另行升级协议。

## STOP、生命周期与重试

非STOP运动、关节position、HOME和软关机必须先有一个新鲜、关联的基本STATUS且faultFlags=0；此门控不可关闭。没有状态或状态过期返回STALE_STATUS，有故障返回REJECTED，均不发送控制帧。连接成功并不等于可安全动作。STOP始终绕过状态/故障门控；未连接和已关闭仍分别返回NOT_CONNECTED/CLOSED。门控使用MCU全局faultFlags，固件必须汇总关节与底盘故障到此位图。

`stopMotion()` / `pause()` 是显式STOP，不保存或自动恢复之前的运动。提交STOP时，同步取消所有旧motion待发送/重试/待确认对象，并增加motion epoch；STOP置于IO队列首。旧dispatch在真正写串口前再次检查pending和epoch，即使IO队列已有旧FORWARD也不会在STOP之后恢复运动。一个已经进入串口write的操作无法撤回，单次write结束后优先发送STOP。后续用户明确提交的新motion仍可运行。

同一执行器的新命令使旧命令SUPERSEDED：全局灯光与模式互相替代；同关节position/HOME互相替代；分区灯/指示灯按各自ID替代。不同关节可独立跟踪。取消、ACK成功、过期或关闭后的控制均不再重试。

扩展关节能力启用后，STOP 也同步取消所有旧 ARM_POSITION / HOME_ARM 待发送、重试及待确认命令，防止队列里的旧关节目标在 STOP 后重新发出。extension-v1 固件必须将宏观 STOP 实现为底盘和全部关节的停止并清除运动目标；如果实际固件仅停底盘，则不能声明兼容本文扩展安全契约。

`McuSafetyOptions` 默认 ACK 500ms、最多2次重试、应用观察期限3000ms、状态陈旧5000ms。可选 `motionTimeoutMs` 主机看门狗默认null；启用后期限到达只提交STOP，绝不生成其他动作。计时从首个motion写入开始，重试不延长；它不是实时系统保证。

`close()` 同步使所有旧命令CLOSED、停止调度，然后在串口线程尽力写STOP并关闭传输；CLOSED结果仍会投递。关闭后API返回CLOSED，晚到USB回调安全忽略，不向调用方抛RejectedExecutionException。物理断线、主机进程崩溃或供电故障下主机无法保证STOP送达，**MCU必须实现独立通信超时停车、执行器限位和电源关断安全逻辑**。

## 固件验收与模拟验证

模拟器默认旧0x02模式：`python mcu_simulator.py --port COM7`。仅明确传入`--extensions`才启用上面的测试夹具；`--ack-only`接受命令但保持旧实际状态，用于验证UI不会把ACK显示成执行成功。它不控制真实电机，不能替代板卡验收。

`python -m unittest discover -s mcu-tools`覆盖帧/扩展门控与模拟行为；SDK纯JVM单测覆盖CRC分包恢复、ACK和APPLIED分离、拒绝/超时/故障状态、STOP取消旧发送及重试、状态关联/陈旧、断开/关闭、能力与关节范围、主机看门狗。实物待验收：对应固件命令与CRC、ID与机构范围、HOME标定、实态来源、独立停车限时、断线/故障停车及关机电源观察。本次未连接真实MCU、未驱动执行器；Android板卡检查见随包《SDK0.3.0验证记录.md》。
