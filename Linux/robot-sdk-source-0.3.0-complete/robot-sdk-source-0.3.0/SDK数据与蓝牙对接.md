# SDK 0.3.0 数据接口与蓝牙边界

本 SDK 只提供本地数据与操作接口，不扫描、开启或连接蓝牙，不负责配对、设备/学生绑定、云端同步、历史数据库、重传队列或手机端业务。对方 App 可把下面的 JSON 放入自行实现的 BLE/GATT 或其他传输层；JSON 不是蓝牙分包协议。

## 坐姿结果

`PostureRecognitionClient(..., onBusinessResult = { result -> ... })` 持续主线程回调，也可用 `subscribeBusiness(listener)` 注册并在不需要时 `subscription.close()` 取消。`latestBusinessSnapshot(maxAgeMs = 2000)` 返回明确的 `UNAVAILABLE/CURRENT/EXPIRED`；不要直接把过期 `result` 当实时结论。停止、切换、相机失败、模型失败均清空最近结果。

`PostureBusinessResult.toJson()` 和 `PostureBusinessSnapshot.toJson()` 不依赖蓝牙实现，schemaVersion=1。JSON 包含 SDK 版本、单实例递增 sequence、`observedAtUnixMs`、`observedAtElapsedMs`、帧尺寸、原始相机时间戳、状态、人体模型分数、推理时间、每项结论/连续异常时长、角度和比例测量。

| state | 消费语义 |
|---|---|
| NO_PERSON | 未检出人体；不是正常坐姿 |
| INSUFFICIENT_KEYPOINTS | 有人体但所有检查都不可判定 |
| OBSERVING | 有可见检查，但样本尚不足 |
| PARTIAL | 仅部分检查确认正常，其他仍不可判定/观察中 |
| NORMAL | 三项检查当前可见且均已确认正常 |
| ABNORMAL | 至少一项当前可见、已准备好的检查异常；不代表其他检查全可用 |

每项检查为 `NOT_VISIBLE/OBSERVING/NORMAL/ABNORMAL`，分别针对 `UNEVEN_SHOULDERS/HEAD_DOWN/SLOUCHING`。先看每项 state，再使用异常集合，不能复用平滑器中暂存的、当前已被遮挡的旧异常。`continuousAbnormalMs` 从第一次稳定异常结果开始，正常、遮挡、停止、切换或帧间隔超过 2 秒立即复位；它不是历史累计时长，也不计入断线时间。

`observedAtUnixMs` 是处理该结果时的设备时间，须由 App 管理时钟校准；单调的 `observedAtElapsedMs` 只用于同一次开机内计算时长和有效期。`sourceFrameTimestampNanos` 保留 Camera HAL 原值，标记 `CAMERA_UNSPECIFIED`，不能未经 HAL 验证就与 Unix/开机时钟直接比较。模型人体/关键点 score 不是临床校准的“坐姿置信度”；疑似驼背是正面上半身几何代理，不是医学诊断。

```kotlin
val subscription = client.subscribeBusiness { result ->
    val payload = result.toJson().toByteArray(Charsets.UTF_8)
    // App 自行持久化或按其 BLE MTU/消息ID/校验协议分包发送
}
val snapshot = client.latestBusinessSnapshot()
if (snapshot.availability == SnapshotAvailability.CURRENT) {
    val json = snapshot.toJson()
}
subscription.close()
```

`onObservation` 保留关键点/原始坐姿规则结果，适合诊断与骨架展示；业务接入优先用上述映射。阈值可通过构造参数 `thresholds: PostureThresholds` 显式配置，也可在主线程调用 `setThresholds(thresholds)` 验证并应用新阈值（重置旧结果/平滑历史，不重开模型）。宿主必须对实际安装角度、光照和人群做标定，SDK 不自动学习个人基线。

## 语音、图片与 MCU

`RobotDeviceInfo.capture(context).toJson()` 可读取 SDK/系统版本、ABI、运行时权限、USB 设备 VID/PID 和 USB 输入/输出音频端点，以及系统声明的 Bluetooth Classic/BLE 支持标志。此操作不申请 USB 授权、不读序列号、不打开蓝牙。feature flag 是固件声明，不证明无线电已启用或手机已连接；所有 sessionDeviceId 只用于当前枚举诊断，不是设备绑定身份。

- `StreamingSpeechRecognizer.onStateChanged` 给模型/音频状态和实时临时文字；`onFinalTranscript` 才是端点确认后的非空最终片段。`SpeechFinalTranscript.toJson()` 给文字与两类时间戳。默认 USB 音频输入、RKNN NPU 模型；两种识别仍共享 RK3566 的单核 NPU。停止后旧录音会话不能再交付成功片段，快速启停不会并行创建两个识别流。
- `QuestionCameraClient` 返回未标注 JPEG；它与姿态 client 必须绑定不同相机。当前截图来自预览纹理，不是高分辨率静态流。图片、上传策略和解题结果存储由 App 决定。
- 可选 `PhotoSearchClient.solve(file, endpoint)` 返回本次是否受理，异步输出题目、答案、步骤和错误；`cancel/close` 丢弃旧回调。取消不保证云端已发生的调用/计费撤销。正式 App 只允许 HTTPS，HTTP 仅调试 APK；SDK 不内置模型 API key。
- MCU 控制返回 `McuCommandSubmission.commandId`；回调区分 `ACK_RECEIVED` 与 `APPLIED`，后者仅说明新鲜关联的 MCU 状态回读匹配目标，不是独立传感器认证。范围、能力门控、安全与扩展字段详见两份 MCU 文档。手机收到控制请求后，应由 App 校验授权/前台状态，再调用 SDK，禁止把手机原始串口字节直接透传。

## 对接与验收责任

SDK manifest 声明 INTERNET；生产环境仍应由宿主配置 HTTPS、鉴权和 Network Security Config。调试 HTTP 还需宿主的调试网络策略允许该服务地址，仅 `debuggable=true` 不会自动覆盖 Android 的明文网络限制。两套示例默认只验证本地图片获取，不向付费服务提交请求；正式解题服务器沿用主工程 solve-server 或由服务端团队部署。

App 决定传哪些数据、采样频率、BLE 分包、隐私授权和缓存容量；默认不应上传相机画面、原始音频或完整转写。学生/任务 ID、学习记录、统计报表、设备绑定不进入 SDK 硬件层。SDK 的回调在主线程，App 的网络、存储、蓝牙写入应移到其后台工作线程，不能在每帧回调内同步阻塞。

双摄像头身份匹配依赖经过验证的 `CameraIdentityProvider`；默认 Camera2 无通用 USB 序列号映射，无 provider 时每次重新枚举/冷启动要求人工确认。硬件只有一路时另一角色必须保持缺失，不会顶替。双路并发、长期识别准确率和真实 MCU 扩展动作必须在正式硬件/固件上验收。
