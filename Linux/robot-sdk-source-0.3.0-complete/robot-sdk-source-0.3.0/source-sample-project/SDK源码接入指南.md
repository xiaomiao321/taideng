# RK3566 Robot SDK 0.3.0源码接入指南

本指南随robot-sdk-source-0.3.0独立提供，目标RK3566 / Android11 / arm64-v8a。此包含Robot SDK Kotlin/JNI可编辑源码、模型和RKNN Runtime，sample真实依赖project(':robot-sdk')，不引用预编译Robot SDK AAR。sherpa供应方语音实现仍为AAR，不宣称交付第三方实现源码。

第一次在自己的App中调用SDK，请先读 [面向App开发者的源码教程](APP开发者教程_源码.md)：从可编译的最小坐姿页面开始，再接权限、生命周期、语音、拍题、MCU和数据传输。本文负责环境、API与部署参考，两者用途不同。

## 结构、平台与独立构建

~~~text
robot-sdk-source-0.3.0/
  robot-sdk-source-sample-0.3.0-debug.apk
  robot-sdk-source-sample-0.3.0-project.zip
  sherpa-onnx-v1.13.6-arm64-rknn-no-runtime.aar
  SDK源码接入指南.md / SDK数据与蓝牙对接.md
  MCU通信协议.md / MCU扩展协议与安全控制.md / SHA256.txt
  source-sample-project/
    gradlew.bat / gradlew / gradle/wrapper/
    settings.gradle.kts / build.gradle.kts / gradle.properties
    sample/build.gradle / sample/src/main/
    robot-sdk/build.gradle / consumer-rules.pro
    robot-sdk/libs/sherpa-onnx-v1.13.6-arm64-rknn-no-runtime.aar
    robot-sdk/src/main/java/          # SDK Kotlin
    robot-sdk/src/main/assets/models/ # 姿态与ASR模型
    robot-sdk/src/main/cpp/           # CMake/JNI/include
    robot-sdk/src/main/cpp/rknn/arm64-v8a/librknnrt.so
    robot-sdk/src/test/               # JVM合同测试
~~~

交付工程不依赖原仓库app/、根libs或其他预编译Robot SDK。打包时复制共享模型/JNI到SDK自身src/main，模型/CMake路径都以此module为准。网络只用于构建依赖下载，示例不调用付费解题服务。

准备JDK25、SDK Platform37、NDK27.2.12479018、CMake3.22.1、AGP9.3.1、Wrapper9.5.0、minSdk29、arm64-v8a。在Android Studio SDK Manager安装指定版本或运行：

~~~powershell
sdkmanager "platforms;android-37" "ndk;27.2.12479018" "cmake;3.22.1" "platform-tools"
Get-Content .\SHA256.txt
Get-FileHash .\robot-sdk-source-sample-0.3.0-project.zip -Algorithm SHA256
Expand-Archive .\robot-sdk-source-sample-0.3.0-project.zip -DestinationPath .\my-source-example
Set-Location .\my-source-example
.\gradlew.bat --version
.\gradlew.bat :robot-sdk:testDebugUnitTest :sample:assembleDebug
~~~

本机自行配置JDK/Android SDK路径（Android Studio或local.properties），这些机器设置不在包里也不再交付。首次需要Google Maven/Maven Central/JitPack，--offline只在缓存齐全时可用。输出sample/build/outputs/apk/debug/sample-debug.apk；可跑:robot-sdk:assembleRelease生成AAR，但sample始终project依赖。自带Wrapper是Windows bat及Unix脚本，JDK daemon toolchain固定25。

## 接入自己的App和修改SDK

settings包含:robot-sdk与宿主module，root声明com.android.library/application插件9.3.1；完整复制robot-sdk而非仅java。宿主：

~~~groovy
dependencies {
    implementation project(':robot-sdk')
    implementation files('../robot-sdk/libs/sherpa-onnx-v1.13.6-arm64-rknn-no-runtime.aar')
    implementation 'androidx.core:core-ktx:1.10.1'
    implementation 'com.github.mik3y:usb-serial-for-android:3.11.0'
}
~~~

宿主compileSdk37/minSdk29/arm64-v8a。SDK自身compileOnly files('libs/sherpa-onnx-v1.13.6-arm64-rknn-no-runtime.aar')，宿主承担运行时依赖；sourceSets.assets=src/main/assets、externalNativeBuild.cmake=src/main/cpp/CMakeLists.txt。不能改成../app、根libs或files('旧SDK.aar')绕过错误。

SDK接口在src/main/java/.../sdk，底层在hardware/camera、mcu、speech、posture；JNI是src/main/cpp/pose_jni.cpp，模型在src/main/assets/models。换模型须匹配RK3566编译目标、输入格式、输出解码和RKNN Runtime并重验。改源码后运行JVM合同测试、sample构建，勿只重命名.so模拟Runtime兼容。

正式宿主启用R8时不可丢robot-sdk/consumer-rules.pro及build.gradle的consumerProguardFiles配置，包括sherpa配置/JNI命名空间的keep规则；供应sherpa AAR规则不足以替代本SDK规则。sample仍为debug签名，正式签名/发布由宿主管理。

sample/src/main/.../sdksample/SampleActivity.kt是接入示例；res/layout/activity_sample.xml是调试UI。namespace沿用sdksample编译公共UI，applicationId为com.example.rk3566hardwarevalidator.sdksourcesample；manifest Activity全限定com.example.rk3566hardwarevalidator.sdksample.SampleActivity。独立工程UI已复制到自己的src/main，不再依赖仓库另一个sample。包内无.env/API Key/local.properties/build/.gradle/.cxx或个人IDE配置。

## 安装本包示例

先按本指南“ADB连接”取得$device，再安装已交付APK：

~~~powershell
adb -s $device install -r .\robot-sdk-source-sample-0.3.0-debug.apk
adb -s $device shell am start -n com.example.rk3566hardwarevalidator.sdksourcesample/com.example.rk3566hardwarevalidator.sdksample.SampleActivity
~~~

自行重编改用sample/build/outputs/apk/debug/sample-debug.apk；versionCode3、versionName0.3.0。可与AAR示例并存，测试时只把一款置前台。首次授权后分别选不同设备核对预览，默认不借坐姿镜头拍题，不连接蓝牙/云端拍题，模式/机械臂无capability明确禁用。退后台停止录音/相机并提交STOP。

不同构建机debug签名可能不同，安装签名冲突先评估数据迁移。APK升级不是整机固件刷写；板厂镜像/屏幕参数按其SOP。RobotDeviceInfo.capture(context).toJson()只读版本/板卡/API/ABI、权限、Bluetooth硬件flag及USB音频/VIDPID，不开启或连接蓝牙；源码构建通过不表示真实硬件验收。

## 权限、角色首配与两个相机会话

SDK manifest声明CAMERA、RECORD_AUDIO和可选camera/microphone/USB-host feature。宿主检查Merged Manifest并请求相机/录音运行时权限；串口授权由控制器触发系统弹窗。SDK公开调用和回调在主线程；推理在后台，宿主存储/网络/蓝牙不能同步阻塞每帧回调。

Camera2只枚举HAL暴露的设备。默认不按枚举次序、VID/PID、LENS_FACING、分辨率或能力指纹猜“坐姿/拍题”。没有经板厂验证的CameraIdentityProvider时先人工选择并核对预览，绑定为SESSION_BOUND，仅当前枚举会话有效；冷启动或重新枚举须重新确认。热插拔后相同ID不能证明同一物理设备。

以下为调用结构，两个ID必须来自本次列表与人工确认：

~~~kotlin
import com.example.rk3566hardwarevalidator.sdk.*
import com.example.rk3566hardwarevalidator.hardware.photo.PhotoCropRegion

val inventory = CameraInventory(this)
val devices = inventory.enumerate()
val roles = CameraRoleRegistry()
roles.refresh(devices) // 真正重新枚举才调用；会清临时绑定
val p = roles.bind(CameraRole.POSTURE, userConfirmedPostureId)
val q = roles.bind(CameraRole.QUESTION, userConfirmedQuestionId)
check(p.cameraId != null && q.cameraId != null && p.cameraId != q.cameraId)

val posture = PostureRecognitionClient(
    context = this,
    onObservation = { observation -> /* 关键点/骨架诊断 */ },
    config = CameraSessionConfig(cameraId = p.cameraId),
    onCameraEvent = { event -> /* 显示相机状态 */ },
    onBusinessResult = { result -> /* 结构化业务结果 */ }
)
val question = QuestionCameraClient(
    context = this,
    config = CameraSessionConfig(cameraId = q.cameraId, analysisEnabled = false),
    onEvent = { event -> /* 拍题自己的状态 */ }
)
// 分别把两个previewView加入可见容器；授权后：
posture.start()
question.start()
question.capturePhoto(PhotoCropRegion.FULL_FRAME) { result ->
    result.onSuccess { jpeg -> /* 宿主保存或按授权上传 */ }
          .onFailure { error -> /* 显示失败，不沿用旧照片 */ }
}
~~~

只有BOUND/SESSION_BOUND且cameraId有效可启动；其他状态UNBOUND/MISSING/AMBIGUOUS/CONFLICT必须提示。两个角色绑定同ID会拒绝并保留原绑定。仅一颗时另一角色不可用，不借坐姿设备拍题。CameraDescriptor给cameraId、facing、previewSizes/analysisSizes、fpsRanges、capabilityFingerprint和stableKey；能力指纹仅诊断。

经验证的物理身份provider可配SharedPreferencesCameraRoleBindingStore，仅verified stableKey持久化。USB deviceId、Camera2 ID、VID/PID或相同能力指纹不能充当稳定身份；默认公开Camera2没有通用UVC序列号映射。板厂provider须跨重启/插拔/交换端口验证；相同型号无序列号或重复stableKey明确报歧义。

CameraSessionConfig含cameraId、analysisEnabled、fps（默认8）、recoverOnReconnect（默认false）。姿态客户端强制分析，QuestionCameraClient强制无YUV/NPU。CameraEvent包括Opening/PreviewStarted/Missing/Busy/Disconnected/Available/Error/Stopped。Missing/Disconnected时停受影响角色并重新枚举确认，不自动恢复至同ID。selectCamera(id)明确选设备；retryCamera()重试当前设备；兼容switchCamera()只是轮换列表，不是角色绑定。

两个client拥有独立设备/会话，双USB并发取决于HAL/供电/总线带宽，必须实测。坐姿优先稳定640×480预览+YUV；无安全共用YUV时可仅有预览无姿态。Question JPEG取自己的TextureView当前预览，不是独立高分辨率静态流。region省略或FULL_FRAME取全画面；PhotoCropRegion()默认边界左0.06/上0.08/右0.94/下0.92，为显示预览相对坐标。纸张完整性、小字、反光、旋转裁切另测。

captureAnnotatedBitmap()为带骨架诊断图，预览与最近推理不保证同帧，用完须recycle Bitmap；业务上传不要用该图。并发拍照明确失败，stop/select/close后的旧请求不能回报成功。

## 坐姿业务结果、JSON及蓝牙边界

业务使用onBusinessResult、subscribeBusiness(listener)、latestBusinessSnapshot(maxAgeMs=2000)；onObservation保留诊断关键点。stop、切换、相机/模型失败清结果。快照为UNAVAILABLE/CURRENT/EXPIRED，EXPIRED即使带result也不是实时结论。

~~~kotlin
val subscription = posture.subscribeBusiness { result ->
    val json = result.toJson() // schemaVersion=1 / sdkVersion=0.3.0
    // 宿主决定后台存储/传输；SDK不连接蓝牙
}
val snapshot = posture.latestBusinessSnapshot(maxAgeMs = 2_000)
if (snapshot.availability == SnapshotAvailability.CURRENT) {
    val json = snapshot.toJson()
}
subscription.close()
~~~

PostureBusinessResult含sequence、observedAtUnixMs、observedAtElapsedMs、sourceFrameTimestampNanos、尺寸、state、personScore、inferenceTimeMs、checks与测量。NO_PERSON/INSUFFICIENT_KEYPOINTS/OBSERVING/PARTIAL都不能解释为正常；仅三个检查当前可见且均确认正常为NORMAL；一项稳定异常可为ABNORMAL，不表示其他检查可判定。每项NOT_VISIBLE/OBSERVING/NORMAL/ABNORMAL分别说明可见性与样本状态。

continuousAbnormalMs从第一次稳定异常开始，正常、遮挡、停止、切换或帧间隔超过2秒复位。Unix时间是处理时设备时钟；elapsed用于同次开机时长；相机原始纳秒标记CAMERA_UNSPECIFIED，不能直接转Unix。模型score不是临床坐姿置信度；疑似驼背为上半身几何代理，安装角度、光照、人群、PostureThresholds需产品标定。

运行期可于主线程调用posture.setThresholds(PostureThresholds(...))；新参数验证通过后重置旧业务结果和历史平滑状态，无需重开姿态模型。阈值变化后须重新观察，不沿用旧的“正常/异常”结论。

以下仅格式示例，非板卡测量：

~~~json
{"schemaVersion":1,"availability":"UNAVAILABLE","ageMs":null,"result":null}
~~~

示例复制JSON仅写剪贴板。SDK不扫描/配对/连接BLE/GATT，不做学生/任务/设备绑定、学习历史、云端同步、MTU分包、重传；不自动上传画面/原始音频。宿主另定传输合同、权限、采样频率和缓存，详见随包《SDK数据与蓝牙对接.md》。不要无鉴权透传手机原始串口字节至运动控制。

## USB麦克风持续识别

StreamingSpeechRecognizer异步加载模型，Idle可尝试start()；获得RECORD_AUDIO且仅前台启动。默认USB输入，不自动退板载麦克风。Listening.transcript是变化的临时文字，onFinalTranscript才是端点后的最终非空片段。

~~~kotlin
import com.example.rk3566hardwarevalidator.hardware.speech.*
var foreground = false
var ready = false
lateinit var speech: StreamingSpeechRecognizer
speech = StreamingSpeechRecognizer(
    context = this,
    onStateChanged = { state ->
        if (state is SpeechRecognitionState.Idle) {
            ready = true
            if (foreground && hasRecordAudioPermission() && !speech.isRecording) speech.start()
        }
    },
    onFinalTranscript = { final -> val json = final.toJson() }
)
// onResume: foreground=true，模型/权限就绪后start
// onPause: foreground=false，再speech.stop()
// onDestroy: speech.close()
~~~

最终片段有text、elapsedRealtimeMs、observedAtUnixMs；stop保留模型，close最终释放。快速启停不能产生两个录音流，旧会话不能交付成功片段。suppressInputFor(durationMillis)用于宿主提示音回声抑制。姿态/ASR共享RK3566单核NPU，并发负载、USB输入、电平、噪声、终句另验。

## MCU提交、回读、模式与机械臂

McuSerialController扫描USB串口，1152008N1、协议0x02。摄像头/麦克风不是MCU；PC Host不能一根USB线直接模拟下位机，需USB转TTL或CDC Device板。随包《MCU通信协议.md》《MCU扩展协议与安全控制.md》是合同和安全依据。

~~~kotlin
import com.example.rk3566hardwarevalidator.hardware.mcu.*
val mcu = McuSerialController(
    context = this,
    onConnectionStateChanged = { state -> /* 授权/连接/断线 */ },
    onStatusChanged = { state ->
        // 实际brightnessPercent/motion/faultFlags
        // receivedAtElapsedMs/stale/relatedCommandIds用于新鲜度和关联
    },
    onProtocolMessage = { message -> /* 可选展示诊断信息，参数必须传入 */ },
    onCommandResult = { result ->
        // commandId识别一次提交；ACK_RECEIVED仍处理中
        // APPLIED需新的关联状态匹配目标
    }
)
mcu.start()
val stop = mcu.stopMotion() // STOP始终可提交；未连接会明确失败
val light = mcu.setLight(true, 50)
if (!light.accepted) { /* 显示light.outcome */ }
// onPause: mcu.pause()取消旧motion并提交STOP
// onDestroy: mcu.close()
~~~

提交返回commandId/sequence/command/accepted/outcome。accepted是SDK受理，ACK_RECEIVED是MCU接收，APPLIED是新的关联回读匹配目标，非独立传感器认证；失败含REJECTED/TIMEOUT/STATE_TIMEOUT/STALE_STATUS/NOT_CONNECTED/WRITE_ERROR/DISCONNECTED/CLOSED/INVALID_ARGUMENT/UNSUPPORTED/SUPERSEDED/BUSY。宿主以commandId维护pending，普通按钮等终止结果解禁，STOP绕过门控。状态区显示实际回读而非按钮目标；isStatusStale()时不得报旧状态有效。

基础接口支持灯光开关/0..100亮度、宏观STOP/FORWARD/BACKWARD/TURN_LEFT/TURN_RIGHT，不是独立电机位。LEGACY_V2默认无模式/机械臂。setLightMode(modeId,brightnessPercent)及setArmPosition(jointId,positionMilliDegrees,speedMilliDegreesPerSecond)仅在经固件验证的McuCapabilities.lightModes/joints范围内可用，否则UNSUPPORTED、不发帧。不要为启用按钮臆造capability；其他可选扩展同样门控。

pause/stopMotion使旧运动等待/重试不能在STOP后发送；正在进行的单次串口IO无法撤销，随后发STOP。MCU须自行做失联停止、限位/互锁；Android退后台STOP不替代硬件保护。正式合同可配置McuSafetyOptions超时/重试/状态有效期。

非STOP运动、关节位置/HOME及软关机强制要求新鲜、关联的基础STATUS且faultFlags=0，提交和真正写入前均检查。纯查询用requestExtendedStatus(commandType, selector)，结果用extendedStatusSnapshot或jointSnapshots读取；检查stale/时间戳，HOME物理按键与校准状态目前为UNKNOWN，不猜测为已完成。

PreviewStarted与isPreviewActive要求新预览帧已到达，不能把会话配置完成当作拍照就绪；停止/切换/释放后旧截图不能返回成功。

## 生命周期与ADB连接

主线程构造并附预览，CAMERA授权且角色有效后start；ASR仅前台且模型/权限就绪start。onPause先foreground=false，再mcu.pause、相机stop、speech.stop；onDestroy取消subscription并close全部。close后不可重启。相机重试先stop等资源释放（示例约1秒）再start已确认设备，不能缺失改选另一ID。

PC与板卡接入可互通局域网，在Android“开发者选项→无线调试”获取IP、配对/连接端口：

~~~powershell
$boardIp = '填写板卡IP'
$pairPort = '填写配对端口'
$connectPort = '填写连接端口'
adb pair ($boardIp + ':' + $pairPort)
adb connect ($boardIp + ':' + $connectPort)
adb devices -l
$device = $boardIp + ':' + $connectPort
~~~

配对/连接端口不同，重启可能改变；USB ADB直接用adb devices列出的serial。多transport都加-s。按本指南前部对应APK安装命令部署；adb install是安装/升级App，不是固件烧写，镜像/屏幕刷写按板厂SOP。

## 验收和已知限制

核对SHA后验：角色冲突/缺失/热插拔后拒拍、重新确认、十次重试/进退前台；真人/无人/遮挡/结果过期；纸张取景/小字/反光；USB电平/临时文字/最终片段；真实MCU的STOP、ACK、关联回读、超时/断线、旧运动失效；合同确认后才验模式/机械臂。双摄+ASR并发、长期资源占用另测。

~~~powershell
adb -s $device logcat -d -s AndroidRuntime:E CameraService:E Camera3-Device:E McuSerial:I StreamingSpeech:I
~~~

本次开发未调用付费API、未驱动执行器。0.3.0源码与AAR实机、独立工程和R8检查详见随包《SDK0.3.0验证记录.md》。编译/JVM测试通过不代表真人准确率、语音终句、真实MCU或双摄并发现场通过；当前只枚举一路Camera2时无法证实双路并发。示例不调用搜题服务器，不宣称云端解题已验收。

debug签名/targetSdk30用于Android11板卡；其他SoC/HAL/Android、正式签名/商店及16KB原生页面要求须重新适配。故障优先查显式依赖/重复Runtime、相机权限/另App占用/HAL流组合、USB音频/端点、串口授权/CRC/回读、默认扩展capability不支持、无线连接端口和路由器隔离。
