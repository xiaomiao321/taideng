# Robot SDK 0.3.0 验证与交付边界

日期：2026-09-26。SDK及源码/AAR示例版本0.3.0（示例versionCode=3）；主验证App源码版本1.3.1（versionCode=5）。本记录区分已经执行的检查与必须由正式硬件/固件完成的验收，不把编译或模拟通过写成实物动作通过。

## 本次补齐的SDK能力

- 相机枚举、显式选择、坐姿/拍题角色、稳定身份提供器及缺失/冲突状态。无可靠物理身份时首次人工核对、刷新/冷启动重新确认；不按Camera2 ID、VID/PID或枚举顺序猜角色。
- 独立拍题相机、未标注JPEG及裁剪接口，坐姿预览/标注诊断图；进程内同相机租约、防止重复打开及旧会话回调。PreviewStarted/isPreviewActive须实际收到新预览纹理帧，避免配置完成后立即抓到尚未更新的空画面。
- 坐姿业务状态、逐项可判定性、时间戳、连续异常时长、CURRENT/EXPIRED/UNAVAILABLE快照、可取消订阅、JSON及可验证的动态阈值。
- 语音临时与最终文本分离、最终片段JSON及错误码；快速启停避免并行原生识别流，停止后的旧会话不交付成功文本。
- MCU命令ID、受理/ACK/实际状态匹配分离、关联查询与过期状态、超时和断线、STOP队列抢占与退后台停止。关节位置/HOME等扩展默认禁用，仅经确认的固件能力开放；STOP同时取消旧关节待发与重试目标。
- 灯光模式、关节位置/HOME、分区灯、蜂鸣器、指示灯、软关机接口和扩展规范、只读关节/扩展状态查询。软关机不因ACK或断线就报告物理关机成功；协议未提供的HOME物理按键/标定状态为UNKNOWN。
- 只读平台/权限/USB音频/Bluetooth feature信息；蓝牙仅交付数据和本地接口，不实现扫描、配对、GATT、绑定、缓存同步、学生任务或云端业务。
- 可选PhotoSearchClient的忙碌、取消及关闭旧请求保护；SDK不内置密钥、不托管解题服务器。

## 构建和自动测试

仓库工程运行：

```powershell
.\gradlew.bat :robot-sdk:testDebugUnitTest :robot-sdk:lintDebug :robot-sdk:assembleRelease `
  :robot-sdk-sample:assembleDebug :robot-sdk-source-sample:assembleDebug `
  :robot-sdk-sample:lintDebug :robot-sdk-source-sample:lintDebug :app:assembleDebug --console=plain
python -m unittest discover -s mcu-tools
```

结果：BUILD SUCCESSFUL；7组SDK JVM测试共44项，0失败/错误；Python MCU模拟测试11项通过。SDK及两款示例Lint无错误；仍有arm64-only/竖屏与ChromeOS兼容性建议、UseKtx建议和旧CameraCaptureSession API废弃提示，并不宣称支持ChromeOS或其他ABI。

单元测试覆盖：相机角色缺失/冲突/重复/稳定身份与租约、流组合、阈值校验、坐姿状态/时长复位、语音JSON/片段/电平，以及MCU帧、ACK与APPLIED、纯查询关联、范围/能力/故障门控、超时/陈旧、STOP取消底盘和关节旧命令、关闭/断线与看门狗。

另将两个交付模板各自生成到独立目录，离开原仓库依赖路径验证：

- 源码工程：`:robot-sdk:testDebugUnitTest :robot-sdk:lintDebug :sample:assembleDebug :sample:lintDebug`全部通过；同样44项JVM测试通过。SDK实现、模型、JNI和Runtime从自身模块解析，不依赖原工程app/src/main。
- AAR工程：`:sample:assembleDebug :sample:lintDebug`通过；额外在测试副本启用R8优化混淆，编译/Lint及真机启动通过。SDK consumer规则保留PoseRknn及sherpa配置/JNI类，避免第三方AAR空consumer规则导致反射/JNI失败。正式交付示例仍为普通debug签名构建。

## RK3566板卡实际检查

无线ADB连接同一局域网内板卡。Android11/API30、arm64-v8a、型号rk3566_r；RKNN Runtime2.3.2、板载驱动0.8.8。Camera2仅枚举ID0一路USB相机，USB麦克风为USB-Audio - USB Composite Device。未接真实MCU。

源码示例已安装、模型初始化并运行：CameraService显示640×480预览及640×480 YUV双流；连续10次受控相机重试后仍有输出、进程PID保持，日志实际出现11次连接/10次断开，排除仅点击无操作。业务JSON持续更新，当前不可见检查为NOT_VISIBLE、时长0、测量值null，没有把无人/不可判定当作正常。USB输入及语音模型初始化通过。

独立AAR示例启用R8后，使用临时instrumentation驱动界面调用检查，最后一次结果：

| 检查 | 实际结果 |
| --- | --- |
| 明确绑定拍题相机并等待首帧，抓取JPEG | 568×426、30306字节；没有上传 |
| 同一相机再绑定坐姿角色 | CONFLICT，拒绝重复角色 |
| 刷新后未重新绑定就拍照 | REFUSED，不借用坐姿设备 |
| 改绑坐姿相机，业务快照持续刷新 | CURRENT，sequence从1增加至8；NO_PERSON而非NORMAL；前轮也观察到INSUFFICIENT_KEYPOINTS |
| 坐姿标注诊断图 | 568×426 |
| USB录音输入 | 正确USB端点，现场电平26%；不代表终句/准确率验收 |
| 连续10次受控坐姿相机重试 | 10/10回到PreviewStarted/Running且业务CURRENT，instrumentation正常完成 |

上述临时测试只做摄像头、JSON和音频输入检查，不开蓝牙、不调用付费模型、不提交执行器动作；测试工具和R8试验配置不属于正式宿主业务。关键日志检查未发现本次运行的AndroidRuntime/JNI/native致命崩溃。该结论不代替长期压力测试。

## 尚需正式条件验收

1. 真正两路摄像头的身份来源、拔插/重启后自动匹配及HAL双路并发。目前只有一路，SDK提供可靠身份扩展点，不能保证任意两个相同无序列号USB相机自动区分。
2. 安装角度、旋转/裁切、真人姿态阈值与误报率。高低肩/低头/疑似驼背为上半身几何判断，不是医学诊断；截图为预览尺寸，不是高分辨率静态拍照，也不含自动透视矫正。
3. 安静/噪声环境的实际语音终句与识别准确率，以及双摄+ASR长期CPU/NPU占用。USB电平与模型初始化不能替代这些测试。
4. MCU固件确认扩展命令与物理映射、关节范围、HOME/校准、实际状态传感来源；STOP全机构语义、独立失联停车、限位、故障及关机电源观察。模拟器和SDK接口已准备，不表示MCU已烧入这些功能。
5. 正式宿主蓝牙传输/鉴权/分包/同步、云端服务、正式签名和部署；其他SoC、Android/HAL或16KB原生页面环境需单独适配。本次没有使用付费搜题API。

## 两份交付

`dist/robot-sdk-aar-0.3.0/`：Robot SDK AAR、语音依赖、AAR示例APK、独立工程ZIP/展开目录、AAR指南、数据说明、MCU两份协议、模拟器与SHA256清单。

`dist/robot-sdk-source-0.3.0/`：真正可编辑的Robot SDK与源码示例独立工程、模型/JNI/Runtime、语音第三方AAR、源码示例APK、源码指南以及相同的数据/协议/模拟与SHA清单。第三方sherpa实现源码不在本次交付中。

安装的是APK而不是AAR；无线ADB连接、构建、覆盖安装与板厂固件烧写边界分别见各自接入指南。既有0.2.x交付目录保留，0.3.0不覆盖它们。
