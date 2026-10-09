# 面向 App 开发者的 Robot SDK 0.3.0 教程：AAR 接入

适用：已有或新建机器人端 Android App，希望引用SDK二进制并完成自己的页面。学习路线：接入依赖 → 运行自己的最小坐姿页 → 接语音/双摄/MCU → 对接你方服务器与蓝牙 → 联调验收。本文可随AAR交付包独立阅读，不要求先看源码版教程。

## 1. 先跑参考示例，再接自己的 App

解压本包 `robot-sdk-aar-sample-0.3.0-project.zip`，用Android Studio打开或运行 `.\gradlew.bat :sample:assembleDebug`。安装后手动选择角色并检查预览/JSON。完整环境和板卡连接见本包 [SDK_AAR接入指南.md](SDK_AAR接入指南.md)。

本交付工程配置JDK25、AGP9.3.1、Gradle9.5.0、compileSdk37、minSdk29、targetSdk30、arm64-v8a；AAR宿主不重新编译JNI，无需NDK/CMake。AGP9工程自带Kotlin支持；现有工程使用其他AGP/Kotlin时先核对兼容性，不盲目同时添加两种Kotlin插件。正式App的targetSdk/签名/发布政策由你方决定，示例不是商店发布配置。

下面以宿主模块名app说明。复制本包两个AAR至 `app/libs/`：

```text
app/libs/robot-sdk-0.3.0.aar
app/libs/sherpa-onnx-v1.13.6-arm64-rknn-no-runtime.aar
```

## 2. 设置依赖并先解决编译问题

项目 `settings.gradle.kts` 的依赖仓库应包含：

```kotlin
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven(url = "https://jitpack.io")
    }
}
```

宿主 `app/build.gradle.kts` 合并以下配置；已有android/dependencies块时合并，不重复整个块：

```kotlin
android {
    compileSdk = 37
    defaultConfig {
        minSdk = 29
        ndk { abiFilters += "arm64-v8a" }
    }
}
dependencies {
    implementation(files("libs/robot-sdk-0.3.0.aar"))
    implementation(files("libs/sherpa-onnx-v1.13.6-arm64-rknn-no-runtime.aar"))
    implementation("androidx.core:core-ktx:1.10.1")
    implementation("com.github.mik3y:usb-serial-for-android:3.11.0")
}
```

两个AAR都需要；本地AAR没有Maven依赖传递元数据。不能只复制Robot SDK AAR，也不要另带一份不同的librknnrt.so。R8必须保留SDK AAR的consumer规则；不要重打包丢掉PoseRknn/sherpa JNI keep。供应sherpa是第三方AAR，SDK二进制本身已包含模型与姿态原生库。

先 `.\gradlew.bat :app:assembleDebug`。能编译后再做下面的页面；仅写UI和业务不用改SDK、Camera2或JNI。需要修改SDK实现时使用源码版，而不是反编译AAR。

## 3. 开发分工：先把硬件当作本地服务

本教程面向机器人端 Android App 开发者，不是给终端用户看的操作说明。SDK 负责采集、模型推理与串口协议；你方负责页面、权限、生命周期、业务身份、记录、网络和蓝牙传输。最终发布的是你方 APK，不是本示例 APK。

| 你要做的页面/业务 | 使用入口 | 不要做的事 |
| --- | --- | --- |
| 坐姿预览和结论 | PostureRecognitionClient、previewView、latestBusinessSnapshot | 再开一条 Camera2 或把无人当作正常 |
| 拍题实时画面和 JPEG | QuestionCameraClient、capturePhoto | 缺拍题镜头时偷偷借坐姿镜头 |
| USB 语音文字 | StreamingSpeechRecognizer | 临时文字每次变化就当作一条最终指令 |
| 灯光、宏观运动、扩展外设 | McuSerialController | 绕过 SDK 透传 PWM/GPIO，或把 ACK 当执行成功 |
| 手机端/服务端同步 | toJson() + 你方传输层 | 认为 JSON 已经实现 BLE/GATT 或可靠同步 |
| 权限、Wi-Fi、OTA、账号和学生任务 | 你方 App/平台业务 | 认为这些都包含在本 SDK 中 |

建议一个明确的页面/前台功能所有者持有 SDK 实例。ViewModel 只保存不可变展示数据，不把 Activity、previewView 或带页面 Context 的 SDK client 放进长期全局单例。Fragment 对应页面销毁时清理视图与实例；多页共享采集时，应由同一个更上层所有者统一启停，子页面不各自重复创建模型。

SDK 相机接口须在主线程调用，结果和状态回调也在主线程。文件、数据库、蓝牙和网络写入放后台；SDK 已做采集/推理后台处理，不要为了“异步”把相机 start/stop 搬到后台线程。

## 4. 第一个里程碑：自己的 App 显示坐姿

先只接坐姿，不接串口、蓝牙或付费搜题。下面是完整的最小 Activity：人工选择相机、预览、结构化结论、过期检测、授权及前后台释放。它无需额外 layout XML。

将代码保存为宿主模块 `src/main/java/com/example/robotapp/PostureDemoActivity.kt`。这里的包名只是教程示例，可按你方工程修改。先保留 SDK 的原包名和 consumer 混淆规则，不要重命名 JNI 类。

```kotlin
package com.example.robotapp

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.ViewGroup
import android.widget.*
import com.example.rk3566hardwarevalidator.sdk.*

class PostureDemoActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private val roles = CameraRoleRegistry()
    private lateinit var inventory: CameraInventory
    private lateinit var selection: Spinner
    private lateinit var previewHost: FrameLayout
    private lateinit var cameraText: TextView
    private lateinit var resultText: TextView
    private var devices: List<CameraDescriptor> = emptyList()
    private var posture: PostureRecognitionClient? = null
    private var foreground = false
    private var askedPermission = false

    private val refreshResult = object : Runnable {
        override fun run() {
            if (!foreground) return
            renderResult()
            main.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        inventory = CameraInventory(this)
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(12), dp(12), dp(12))
        }
        setContentView(ScrollView(this).apply { addView(column) })
        cameraText = TextView(this).also { column.addView(it) }
        resultText = TextView(this).also { column.addView(it) }
        selection = Spinner(this).also { column.addView(it) }
        Button(this).also {
            it.text = "刷新设备列表（重新确认角色）"
            it.setOnClickListener { if (hasCameraPermission()) refreshDevices() else askCameraPermission() }
            column.addView(it)
        }
        Button(this).also {
            it.text = "确认坐姿镜头并开始"
            it.setOnClickListener { confirmPostureCamera() }
            column.addView(it)
        }
        previewHost = FrameLayout(this)
        column.addView(previewHost, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(300)))
    }

    private fun refreshDevices() {
        // 新一轮枚举先释放旧角色，不保留旧画面对应的“正常”结论。
        posture?.close()
        posture = null
        previewHost.removeAllViews()
        devices = runCatching { inventory.enumerate() }.getOrElse {
            cameraText.text = "枚举失败：" + it.message
            emptyList()
        }
        roles.refresh(devices) // 会话人工绑定会失效，这是设计约定。
        selection.adapter = ArrayAdapter(this,
            android.R.layout.simple_spinner_dropdown_item,
            listOf("请选择镜头") + devices.map { "Camera " + it.cameraId })
        if (devices.isEmpty()) cameraText.text = "没有 Camera2 设备，请检查连接与权限"
        renderResult()
    }

    private fun confirmPostureCamera() {
        if (!hasCameraPermission()) { askCameraPermission(); return }
        val device = devices.getOrNull(selection.selectedItemPosition - 1)
        if (device == null) { cameraText.text = "请先选择并人工核对镜头"; return }
        val binding = roles.bind(CameraRole.POSTURE, device.cameraId)
        if (binding.status != CameraRoleStatus.BOUND &&
            binding.status != CameraRoleStatus.SESSION_BOUND) {
            cameraText.text = binding.message
            return
        }
        posture?.close()
        previewHost.removeAllViews()
        val client = PostureRecognitionClient(
            context = this,
            onObservation = { /* 关键点只用于诊断，业务显示用下面的快照。 */ },
            onStateChanged = { cameraText.text = it.toString() },
            config = CameraSessionConfig(cameraId = binding.cameraId, fps = 8),
            onCameraEvent = { event ->
                cameraText.text = event.toString()
                if (event is CameraEvent.Missing || event is CameraEvent.Disconnected) {
                    roles.unbind(CameraRole.POSTURE)
                    posture?.stop()
                    cameraText.text = "镜头已断开，请刷新后重新确认"
                }
            },
            onBusinessResult = { renderResult() }
        )
        posture = client
        previewHost.addView(client.previewView, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        if (foreground) client.start()
    }

    private fun renderResult() {
        val snapshot = posture?.latestBusinessSnapshot(maxAgeMs = 2_000)
        if (snapshot == null || snapshot.availability != SnapshotAvailability.CURRENT) {
            resultText.text = when (snapshot?.availability) {
                SnapshotAvailability.EXPIRED -> "结果过期，不能作为当前坐姿"
                else -> "尚无有效坐姿结果"
            }
            return
        }
        val result = snapshot.result ?: return
        val label = when (result.state) {
            PostureResultState.NO_PERSON -> "未检测到人"
            PostureResultState.INSUFFICIENT_KEYPOINTS -> "关键点不足，请调整位置"
            PostureResultState.OBSERVING -> "正在观察"
            PostureResultState.PARTIAL -> "部分项目可判定"
            PostureResultState.NORMAL -> "当前可判定项目全部正常"
            PostureResultState.ABNORMAL -> "发现坐姿异常"
        }
        resultText.text = label + "\n" + result.checks.joinToString("\n") {
            "${it.issue}：${it.state}，连续异常 ${it.continuousAbnormalMs} ms"
        }
    }

    override fun onResume() {
        super.onResume()
        foreground = true
        if (hasCameraPermission()) {
            if (devices.isEmpty()) refreshDevices()
            if (roles.resolve(CameraRole.POSTURE).cameraId != null) posture?.start()
        } else if (!askedPermission) askCameraPermission()
        main.removeCallbacks(refreshResult)
        main.post(refreshResult)
    }

    private fun hasCameraPermission() =
        checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    private fun askCameraPermission() {
        askedPermission = true
        requestPermissions(arrayOf(Manifest.permission.CAMERA), 1001)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 1001 && foreground && hasCameraPermission()) refreshDevices()
    }

    override fun onPause() {
        foreground = false
        main.removeCallbacksAndMessages(null)
        posture?.stop()
        resultText.text = "页面暂停，没有实时结论"
        super.onPause()
    }

    override fun onDestroy() {
        posture?.close()
        posture = null
        previewHost.removeAllViews()
        super.onDestroy()
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
}
```

宿主 manifest 注册页面；你方只有一个入口时可把下面 Activity 设为启动入口，不要同时保留两个未规划的 LAUNCHER：

```xml
<activity
    android:name="com.example.robotapp.PostureDemoActivity"
    android:exported="true">
    <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
    </intent-filter>
</activity>
```

检查宿主 Merged Manifest 包含 CAMERA。SDK 已声明 CAMERA、RECORD_AUDIO、INTERNET 和可选硬件 feature；运行时授权依然是宿主责任，只有此坐姿页面时不必申请录音权限。拒绝后显示可操作的重试/设置入口，不在每次 onResume 强制弹窗。

预期行为：列表不默认绑定第一颗相机；手动确认后出现画面和结果；无人时不是“正常”；按 Home 或切换 App 后停止，返回前台重新开始；拔掉镜头后要求重新确认。示例固定 300dp 高度仅为教学，正式 UI 按屏幕尺寸和取景比例设计，确认缩放/裁切和安装方向。

`previewView` 是 SDK 已创建的 View，不要再开 Camera2，也不要将同一个 View 同时添加到两个父容器。Compose 工程用自己的 AndroidView 容器承载它，并由单一生命周期所有者启停；不要在每次重组 new client。

## 5. 坐姿页面升级：分开显示状态与结果

相机状态、模型状态、业务结论应使用独立 UiState/控件，避免某个 LoadingModel/Running 回调覆盖坐姿结论。最小页面把硬件消息集中在一个 TextView，正式产品可进一步拆分。

业务主入口是 `onBusinessResult` 或 `subscribeBusiness`；不要为同一份数据重复订阅两次保存。其他组件订阅时：

```kotlin
val subscription = postureClient.subscribeBusiness { result ->
    val json = result.toJson()
    // 只更新UI/投递后台队列；不在这里同步写数据库或蓝牙。
}
// 组件销毁时取消；不要积累旧页面监听器。
subscription.close()
```

上段及后续片段的 `postureClient`、TextView、按钮、队列等由你方页面持有；它们是模块级接法，不是另一份完整 Activity。实际对象名可参考交付工程 `sample/src/main/java/com/example/rk3566hardwarevalidator/sdksample/SampleActivity.kt`。

- 先判断快照 CURRENT，再判断整体 state，最后展示每项 check.state。
- NOT_VISIBLE、OBSERVING、PARTIAL、NO_PERSON、INSUFFICIENT_KEYPOINTS 不能改成“正常”。
- 定时读取有效期是为了在无新回调时让旧结论消失，不是驱动推理；不要用 UI 定时器重复 start。
- continuousAbnormalMs 是本次连续异常，不是当天累计；遮挡/正常/停止/长帧间隔会复位。
- sequence 只在单个 client 内递增，不是跨实例/跨重启全局消息ID；App 自己定义会话ID与去重规则。
- elapsed 时间只用于同次开机；Unix 时间取决于设备时钟校准。相机原始纳秒不能直接转换为 Unix 时间。
- 规则阈值可通过 setThresholds 更新，但必须验证现场角度和人群；模型 score 不是医学“正确坐姿概率”。

提示音属于 App：仅对当前有效、持续异常结果按冷却间隔提示；不要每帧播放。无人/不可判定/过期停止提示。音响、Android 音频路由及提示内容由你方实现；播放时可调用 speech.suppressInputFor(1000) 抑制自身提示音的一段输入，时长须按现场验证，不代表完整回声消除。

## 6. 加入常开 USB 语音：临时文字和最终语句分两条链路

先申请 RECORD_AUDIO，再启用已加载的模型。下面的字段和初始化逻辑加入你的页面所有者；`partialText/finalText/audioStateText` 是你方 TextView：

```kotlin
import com.example.rk3566hardwarevalidator.hardware.speech.*

private var speech: StreamingSpeechRecognizer? = null
private var speechReady = false

private fun createSpeech() {
    speech = StreamingSpeechRecognizer(
        context = this,
        onStateChanged = { state ->
            when (state) {
                SpeechRecognitionState.LoadingModel -> {
                    speechReady = false
                    audioStateText.text = "模型加载中"
                }
                is SpeechRecognitionState.Idle -> {
                    speechReady = true
                    maybeStartSpeech()
                }
                is SpeechRecognitionState.Listening -> {
                    partialText.text = state.transcript
                    audioStateText.text = "${state.inputDevice} · ${state.inputLevelPercent}%"
                }
                is SpeechRecognitionState.Error -> {
                    audioStateText.text = "${state.code}：${state.message}"
                }
            }
        },
        onFinalTranscript = { result ->
            finalText.text = result.text
            // result.toJson()可进入你方后台存储/传输队列。
            // 真正执行控制还需App授权、明确用户意图和MCU安全门控。
        }
    )
}

private fun maybeStartSpeech() {
    val granted = checkSelfPermission(Manifest.permission.RECORD_AUDIO) ==
        PackageManager.PERMISSION_GRANTED
    val client = speech ?: return
    if (foreground && speechReady && granted && !client.isRecording) client.start()
}
```

构造器会异步加载语音模型；createSpeech 在所有者初始化时执行一次，不放在周期性刷新里。onResume 和权限回调调用 maybeStartSpeech；onPause 先 foreground=false，再 speech?.stop；最终销毁 speech?.close。没有 USB 麦克风会明确报 INPUT_UNAVAILABLE，不自动换输入设备。

临时文字持续覆盖一个“正在说话”区域；最终片段才加入对话/记录列表。不要对临时文字直接下发电机动作，不要把所有最终语句自动当作用户授权。录音止停后不应沿用旧的“监听中”标志；回前台也不自动恢复上一条运动。

## 7. 两颗摄像头：设置页确认角色，拍题页拿自己的画面

用同一 CameraRoleRegistry 管理两个角色。先停止/释放受影响 client，再真正重新枚举与 refresh，随后让用户核对取景并 bind。正常页面刷新数据不能每秒调用 registry.refresh，它会清除人工会话绑定。

```kotlin
val questionBinding = registry.bind(CameraRole.QUESTION, userConfirmedQuestionId)
val postureBinding = registry.resolve(CameraRole.POSTURE)
val usable = questionBinding.status == CameraRoleStatus.BOUND ||
    questionBinding.status == CameraRoleStatus.SESSION_BOUND
if (usable && questionBinding.cameraId != postureBinding.cameraId) {
    val question = QuestionCameraClient(
        context = this,
        config = CameraSessionConfig(
            cameraId = questionBinding.cameraId,
            analysisEnabled = false
        ),
        onEvent = { event -> /* 更新拍题相机状态；断开后停用并重新确认 */ }
    )
    questionPreviewHost.addView(question.previewView)
    // 由所有者保存question；CAMERA授权且页面/功能前台后：
    question.start()
}
```

上段 `registry` 是管理两角色的同一实例；`userConfirmedQuestionId` 必须来自当前枚举。只有一颗相机时，这个教程不同时启用坐姿和拍题；可在测试设置中先解绑坐姿，再人工改绑拍题验证截图，不能声称验证了双路并发。

Camera2 ID、VID/PID、相同能力指纹不是跨重启物理身份。默认每次冷启动/重新枚举重新确认；想自动匹配需板厂/宿主实现并验证 CameraIdentityProvider，配 SharedPreferencesCameraRoleBindingStore 保存可靠 stableKey。两相机缺身份时不能靠猜测保证不反。

等 PreviewStarted 或 isPreviewActive 后再允许拍照：

```kotlin
import com.example.rk3566hardwarevalidator.hardware.photo.PhotoCropRegion

captureButton.setOnClickListener {
    if (!question.isPreviewActive) {
        photoMessage.text = "拍题镜头尚未就绪"
        return@setOnClickListener
    }
    captureButton.isEnabled = false
    question.capturePhoto(PhotoCropRegion.FULL_FRAME) { outcome ->
        captureButton.isEnabled = true
        outcome.onSuccess { jpeg ->
            // 后台保存/显示/上传；JPEG未叠加骨架或取题框。
        }.onFailure { error ->
            photoMessage.text = "拍照失败：" + error.message
            // 不展示上一次照片为“本次拍照成功”。
        }
    }
}
```

question/captureButton/photoMessage 由当前拍题页持有；异步回调写页面前还应检查页面是否已销毁。裁剪区域是显示预览相对坐标，具体用 PhotoCropRegion；选框可以使用 QuestionCropOverlayView，但样式和手势业务由 App 管理。预览 JPEG 不是高分辨率静态照片，也没有自动透视矫正，倾斜纸张/小字要实拍验收。

`captureAnnotatedBitmap` 只供坐姿诊断，可能不是同一推理帧，不能作为搜题上传照片。Bitmap 更新展示或页面销毁时按所有权释放，勿把同一 Bitmap 回收后继续给 ImageView 用。

## 8. 搜题服务器：先展示截图，再由用户提交

SDK 的 PhotoSearchClient 是可选上传客户端，不是模型服务。主工程 solve-server 是另一项部署；你方需要服务器地址、鉴权合同、超时与图片留存政策。API Key 放服务器，不能放 APK/SDK。

```kotlin
import com.example.rk3566hardwarevalidator.hardware.photo.*

val search = PhotoSearchClient(this) { state ->
    when (state) {
        PhotoSearchState.Idle -> { /* 清理忙碌状态 */ }
        is PhotoSearchState.Uploading -> { /* 显示上传中 */ }
        PhotoSearchState.Solving -> { /* 显示等待服务器 */ }
        is PhotoSearchState.Success -> {
            val answer = state.solution.answer
            val steps = state.solution.steps
            // 将题目、答案、步骤和warning映射至App页面。
        }
        is PhotoSearchState.Error -> { /* 显示state.message，允许重新提交 */ }
    }
}
// readyJpegFile是App已在后台写好的非空JPEG文件。
// serverEndpoint是你方配置的服务地址，并非模型厂商带key的URL。
// 只在用户明确点击提交后调用；false表示忙碌或已关闭。
submitPhotoButton.setOnClickListener {
    val accepted = search.solve(readyJpegFile, serverEndpoint)
    if (!accepted) { /* 不重复显示为已上传 */ }
}
```

离开当前拍题流程调用 cancel，最终所有者销毁调用 close。取消不保证已发生的服务器请求或计费撤销。把 JPEG 写文件放后台，并用 App 的页面会话/请求代次阻止离开页面后的旧文件任务触发新上传；onResume不能自动重传旧题。

当前客户端直接 POST 原始 JPEG，Content-Type=image/jpeg，不是 multipart；单图上限8MiB，响应上限1MiB。服务器返回 success、question、answer、steps、knowledge_points 等，协议格式示例：

```json
{"success":true,"question":"题目正文","answer":"答案","steps":["步骤一"],"knowledge_points":["知识点"],"warning":""}
```

正式版本只允许 HTTPS。调试 HTTP 还需要宿主自己的 debug 网络安全策略，不能只靠 debuggable；不全局放开 Release 明文请求。当前 PhotoSearchClient 没有可配置的鉴权请求头接口，如服务需要登录 token/定制请求头，使用你方网络层或在源码版增加相应接口，不能把未经鉴权的测试服务当正式业务。

## 9. MCU 页面：先显示真实状态，再提交操作

导入 com.example.rk3566hardwarevalidator.hardware.mcu.*。以下初始化在页面所有者中只执行一次，TextView/按钮是 App 自己的控件。默认 LEGACY_V2 不开放模式/关节等扩展：

```kotlin
import com.example.rk3566hardwarevalidator.hardware.mcu.*

val pendingCommandIds = mutableSetOf<Long>()
var applyingActualStatus = false
var mcuConnected = false
var latestFaultFlags: Int? = null
val mcu = McuSerialController(
    context = this,
    onConnectionStateChanged = { state ->
        connectionLabel.text = state.toString()
        mcuConnected = state is McuConnectionState.Connected
        if (!mcuConnected) { pendingCommandIds.clear(); latestFaultFlags = null }
        // 断线后清理旧的“已完成/有效实际状态”；禁用普通操作。
    },
    onStatusChanged = { actual ->
        latestFaultFlags = actual.faultFlags
        applyingActualStatus = true
        lightSwitch.isChecked = actual.lightEnabled
        brightnessSlider.progress = actual.brightnessPercent
        applyingActualStatus = false
        actualLabel.text = "实际亮度 ${actual.brightnessPercent}%；故障 ${actual.faultFlags}"
    },
    onProtocolMessage = { message -> /* 仅诊断，正式UI可隐藏协议日志 */ },
    onCommandResult = { result ->
        if (result.outcome != McuCommandOutcome.ACK_RECEIVED &&
            result.outcome != McuCommandOutcome.ACKNOWLEDGED) {
            pendingCommandIds.remove(result.commandId)
        }
        commandLabel.text = "#${result.commandId}：${result.outcome}"
        // ACK_RECEIVED仍处理中；APPLIED为新的关联状态匹配目标。
        // 普通按钮等待终止结果再解禁，STOP不等待。
    }
)
mcu.start() // 探测USB-UART，系统USB授权与CAMERA/录音授权是三件不同的事。

fun maySubmitNormalCommand() = mcuConnected && pendingCommandIds.isEmpty() &&
    !mcu.isStatusStale() && latestFaultFlags == 0

fun remember(submission: McuCommandSubmission) {
    if (submission.accepted) pendingCommandIds.add(submission.commandId)
    else commandLabel.text = "未受理：${submission.outcome}"
}
applyLightButton.setOnClickListener {
    if (!maySubmitNormalCommand()) {
        commandLabel.text = "未连接/处理中/状态过期或故障，不能提交"
        return@setOnClickListener
    }
    remember(mcu.setLight(true, brightnessSlider.progress))
}
offButton.setOnClickListener {
    if (maySubmitNormalCommand()) remember(mcu.setLight(false, 0))
}
stopButton.isEnabled = true
stopButton.setOnClickListener { remember(mcu.stopMotion()) }
refreshSerialButton.setOnClickListener { mcu.refreshDevices() }
```

上段是接线片段，正式页面必须补充控制门控：普通控件要求已连接、没有冲突的待完成命令、状态未过期且faultFlags=0；不能在点亮按钮时立即把实际亮度改成目标。回读设置开关/滑条时用 applyingActualStatus 阻止监听器再次发控制命令；亮度拖动结束或经节流后才提交，不每个像素都写串口。

片段已在提交入口检查门控；正式UI还应定时按同样条件刷新按钮可用状态。灯光关闭是否作为故障下的特例须按双方安全合同设计，不能因本例普通按钮被禁用就阻塞STOP。

宏观动作是 setMotion(McuMotion.FORWARD/BACKWARD/TURN_LEFT/TURN_RIGHT, speedPercent)。只在经过授权的用户动作回调中提交，现场确认机构安全再测；不要在 onCreate/onResume/重连回调里自动移动。STOP按钮始终可点，SDK断线/关闭时仍会返回明确失败，按钮可点不代表能够送达硬件。

accepted是SDK受理；ACK_RECEIVED只是MCU接收；APPLIED要求新的关联、无故障、匹配目标的实际状态。即使APPLIED，也不是独立传感器认证。App按 commandId 关联控件/操作，别只用8位sequence当长期身份；TIMEOUT/STATE_TIMEOUT/STALE_STATUS等都要显示并停止等待，禁止UI猜成功。

定时刷新新鲜度：没有新状态回调也要检查 mcu.isStatusStale()，让“在线/可操作”状态失效。onPause先 foreground=false，然后 mcu.pause()，再停相机和语音；onDestroy关闭MCU。STOP取消旧运动/关节队列与重试，MCU仍必须有独立失联停车、限位和电源保护。

扩展能力只由已验证固件描述开放。不要编造 McuCapabilities 来“解锁”按钮；ARM_POSITION/HOME等需明确关节ID、毫度范围和速度。requestExtendedStatus是只读查询，jointSnapshots/extendedStatusSnapshot仍要检查stale。HOME物理按键、标定状态目前UNKNOWN；软关机不能凭ACK/断线显示“已关机”。完整帧合同见随包 MCU通信协议.md、MCU扩展协议与安全控制.md。

PC Host与板卡 Host不能用普通一根USB线直接模拟串口对端。协议模拟需USB转TTL/匹配电平的UART或CDC Device；注意TX/RX/GND与TTL/RS232不可混用，按硬件设计确认。mcu-tools是软件模拟夹具，不能代替真实固件安全验收。

## 10. 蓝牙只做数据交接：传输层属于你方 App

SDK不扫描/连接/配对蓝牙、不实现GATT/MTU分包/手机设备绑定。提供JSON、结构化对象、订阅与本地控制接口。只读 RobotDeviceInfo.capture(context).toJson() 的 Bluetooth feature flag 也不证明已连接手机。

你方可定义异步队列接口，例如：

```kotlin
interface RobotDataSink {
    // 约定此方法只入有界队列，不同步阻塞SDK回调。
    fun enqueue(type: String, payload: String)
}
val subscription = postureClient.subscribeBusiness { result ->
    sink.enqueue("POSTURE", result.toJson())
}
// 在onFinalTranscript中：sink.enqueue("SPEECH_FINAL", result.toJson())
// 销毁订阅所有者：subscription.close()
```

传输层另定设备/会话身份、schema兼容、发送频率、MTU分包、消息ID、校验、断线缓存、重传和隐私授权。持续坐姿数据可做采样/状态变化聚合，不无限积累每帧；语音最终片段与临时文字分开。App自己的学生/任务ID不要塞进SDK硬件层。

手机控制消息必须先完成鉴权、用户意图和动作白名单校验，再调用mcu操作并回传commandId关联结果；不能接到一个未经校验的蓝牙包就透传运动字节。

## 11. 页面生命周期检查表

| 时机 | App的动作 |
| --- | --- |
| 所有者初始化 | 创建实例一次、安装回调、附预览View；不自动启动运动/上传 |
| 前台/授权成功 | foreground=true，已确认相机角色start；语音模型就绪后start |
| 页面只更新展示 | 读快照/渲染；不重建client、不重新枚举或加载模型 |
| 用户刷新设备 | 停受影响client，重新枚举/确认角色，清旧状态；USB串口用refreshDevices |
| 镜头缺失/断开 | 停该角色、禁拍、清实时结论；重新枚举并确认，不猜同ID |
| 离开拍题流程 | cancel上传请求、废弃旧文件准备任务；按功能所有权停拍题相机 |
| App退后台 | 先foreground=false；MCU pause；相机/语音stop；停页面计时器 |
| 所有者最终销毁 | 取消订阅、close全部、移除View、回收归自己所有的Bitmap |
| 返回前台 | 同实例未close可start，不自动恢复旧运动/旧上传 |
| 实例已close | 不可再次start，按需要创建新实例 |

最小Activity采用“页面不可见即停止”的策略。若正式产品要常驻后台/跨页面长期采集，需要你方另行设计平台服务、权限与单一资源所有权并验收，不直接删掉所有stop/close。

## 12. App开发者的验收顺序与排错

按里程碑交付，避免把所有硬件/云端故障混在一起：

1. 不接MCU/云端：自己APK中的CAMERA授权、人工角色确认、预览、无人/不足/正常/异常、过期和进退前台。
2. USB麦克风：输入端点/电平、临时文字覆盖、停顿后的最终片段，以及权限拒绝/拔出/恢复。
3. 第二颗摄像头：双角色不同、缺失/交换/重启、并发与电源/带宽、取景框对应JPEG。
4. MCU：按现场安全计划先STOP、再灯光，检查ACK和实际回读、超时/断线/退后台；确认固件合同后再测运动与扩展。
5. 你方服务器/蓝牙：鉴权、请求和数据schema、取消/错误/重试、隐私与缓存、正式签名和网络策略。
6. 全组合长期测试：CPU/NPU占用、页面响应、供电、温度、HAL稳定和正式安装角度。

| 现象 | 首先检查 |
| --- | --- |
| 相机黑屏/无法打开 | CAMERA权限、角色是否确认、是否已附View/有尺寸、其他App占用、HAL错误；不能反复并行open |
| 显示永远正常 | 是否把不可判定当正常、只看旧issues、忽略CURRENT/EXPIRED、回调被另一个状态文本覆盖 |
| 拍题取到坐姿画面 | 是否绑定同ID/共享同View、错误地按列表第一个自动选；不能将两个角色“兜底”为一个 |
| 镜头插回无法继续 | 重新枚举并确认；冷启动/插拔ID不构成物理身份 |
| 语音加载失败/R8后JNI失败 | 两个必要库、consumer规则、ABI/模型/Runtime冲突 |
| 语音有电平无最终片段 | 看临时文本/端点停顿/噪声、区分输入初始化与识别准确率；不要刷新时重复创建模型 |
| 串口没有设备 | 是否真USB-UART/支持驱动、授权、两端Host接法、电平/CRC |
| MCU连接却操作失败 | pending、实际状态有效期/fault、SDK安全门控、capability；不要禁用安全检查 |
| 改了SDK源码界面却不变 | 当前依赖方式、安装的包名、构建产物路径和版本；AAR方式须重新生成并替换AAR |
| 上传失败 | 是否是你方代理地址、rawJPEG合同、HTTPS/调试策略、服务鉴权/响应schema；不是把key放进App |
| 升级提示签名不匹配 | 构建机debug key/正式签名；先确认迁移方案，不直接卸载丢数据 |

无线ADB配对/连接端口不同，重启可能变；使用板卡当前值，不把文档中的历史IP当固定配置：

```powershell
$device = '板卡IP:当前连接端口'
adb connect $device
adb devices -l
adb -s $device install -r '自己的APK路径'
adb -s $device logcat -d -s AndroidRuntime:E CameraService:E Camera3-Device:E McuSerial:I StreamingSpeech:I
```

APK安装不是固件烧写；屏幕/系统镜像遵循板厂SOP。本SDK目标RK3566/Android11/arm64，不承诺别的SoC/HAL/Android或16KB页面环境开箱即用。

## 13. 交给双方联调前填写的信息

提交：你方APK包名/版本/签名、SDK版本、构建方式、板卡型号/固件、Android/ABI/RKNN Runtime/驱动、相机数量与身份来源、USB音频与串口型号、MCU固件能力描述、复现步骤、当前/目标/实际状态与commandId、脱敏日志。不要把API Key、原始语音或学生照片默认塞进日志。

交付SampleActivity是可运行参考，不是你方产品UI。关键路径索引：

- SDK接入指南：构建环境、依赖、无线ADB和部署细节。
- SDK数据与蓝牙对接.md：DTO/JSON、时钟、有效期和责任边界。
- MCU通信协议.md、MCU扩展协议与安全控制.md：串口合同与强制安全要求。
- SDK0.3.0验证记录.md：已实际测试与尚待验收项。

本教程新增的是接入教学，不变更SDK/API/APK版本。只有一路摄像头的板卡测试不能证明双路并发；编译通过也不能代替真人坐姿/语音准确率和真实MCU实物验收。
