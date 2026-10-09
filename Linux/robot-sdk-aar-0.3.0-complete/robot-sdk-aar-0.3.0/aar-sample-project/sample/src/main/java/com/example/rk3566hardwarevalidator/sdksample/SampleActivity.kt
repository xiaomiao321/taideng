package com.example.rk3566hardwarevalidator.sdksample

import android.Manifest
import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Bundle
import android.view.ViewGroup
import android.widget.*
import com.example.rk3566hardwarevalidator.hardware.mcu.*
import com.example.rk3566hardwarevalidator.hardware.speech.SpeechFinalTranscript
import com.example.rk3566hardwarevalidator.hardware.speech.SpeechRecognitionState
import com.example.rk3566hardwarevalidator.hardware.speech.StreamingSpeechRecognizer
import com.example.rk3566hardwarevalidator.sdk.*
import org.json.JSONArray
import org.json.JSONObject

/** The same host UI is compiled once with a binary SDK and once with the SDK project. */
class SampleActivity : Activity() {
    private var postureClient: PostureRecognitionClient? = null
    private var questionClient: QuestionCameraClient? = null
    private lateinit var inventory: CameraInventory
    private val registry = CameraRoleRegistry()
    private var cameras = emptyList<CameraDescriptor>()
    private var topologyChanged = false
    private val cameraEvents = mutableMapOf<CameraRole, String>()
    private lateinit var speechRecognizer: StreamingSpeechRecognizer
    private lateinit var mcu: McuSerialController
    private lateinit var cameraState: TextView
    private lateinit var questionState: TextView
    private lateinit var postureState: TextView
    private lateinit var cameraJson: TextView
    private lateinit var postureJson: TextView
    private lateinit var photoState: TextView
    private lateinit var speechState: TextView
    private lateinit var speechFinal: TextView
    private lateinit var mcuState: TextView
    private lateinit var mcuStatus: TextView
    private lateinit var mcuResult: TextView
    private lateinit var snapshot: ImageView
    private lateinit var lightSwitch: Switch
    private lateinit var brightness: SeekBar
    private lateinit var postureSelection: Spinner
    private lateinit var questionSelection: Spinner
    private lateinit var modeSelection: Spinner
    private lateinit var jointSelection: Spinner
    private lateinit var motionButtons: List<Button>
    private val pendingCommands = mutableSetOf<Long>()
    private var capturedBitmap: Bitmap? = null
    private var foreground = false
    private var permissionRequested = false
    private var speechReady = false
    private var mcuConnected = false
    private var latestMcuFaultFlags: Int? = null
    private var applyingMcuStatus = false
    private var photoInProgress = false
    private val refreshJson = object : Runnable {
        override fun run() {
            if (!foreground) return
            updateJson()
            updateMcuControls()
            cameraJson.postDelayed(this, 1_000)
        }
    }
    private val restartCameras = Runnable {
        if (foreground && hasPermission(Manifest.permission.CAMERA)) startBoundCameras()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_sample)
        findViewById<TextView>(R.id.sampleIntegrationMode).text = "SDK ${RobotSdkInfo.VERSION} · ${BuildConfig.INTEGRATION_MODE}"
        cameraState = findViewById(R.id.cameraState)
        questionState = findViewById(R.id.questionCameraState)
        postureState = findViewById(R.id.postureState)
        cameraJson = findViewById(R.id.cameraJson)
        postureJson = findViewById(R.id.postureJson)
        photoState = findViewById(R.id.photoState)
        speechState = findViewById(R.id.speechState)
        speechFinal = findViewById(R.id.speechFinal)
        mcuState = findViewById(R.id.mcuState)
        mcuStatus = findViewById(R.id.mcuStatus)
        mcuResult = findViewById(R.id.mcuResult)
        snapshot = findViewById(R.id.annotatedSnapshot)
        lightSwitch = findViewById(R.id.lightSwitch)
        brightness = findViewById(R.id.brightnessSeekBar)
        postureSelection = findViewById(R.id.postureCameraSelection)
        questionSelection = findViewById(R.id.questionCameraSelection)
        modeSelection = findViewById(R.id.lightModeSelection)
        jointSelection = findViewById(R.id.armJointSelection)
        inventory = CameraInventory(this)
        speechRecognizer = StreamingSpeechRecognizer(this, ::showSpeechState, ::showFinalTranscript)
        mcu = McuSerialController(
            context = this,
            onConnectionStateChanged = ::showMcuConnection,
            onStatusChanged = ::showMcuStatus,
            onProtocolMessage = { mcuResult.text = it },
            onCommandResult = ::showCommandResult,
            onExtendedStatusChanged = { mcuStatus.text = "扩展实际状态：$it" }
        )
        configureControls()
        refreshCameraInventory()
        mcu.start()
    }

    private fun configureControls() {
        findViewById<Button>(R.id.refreshCamerasButton).setOnClickListener { refreshCameraInventory() }
        findViewById<Button>(R.id.bindPostureButton).setOnClickListener { bindRole(CameraRole.POSTURE, postureSelection) }
        findViewById<Button>(R.id.bindQuestionButton).setOnClickListener { bindRole(CameraRole.QUESTION, questionSelection) }
        findViewById<Button>(R.id.retryCameraButton).setOnClickListener {
            if (!hasPermission(Manifest.permission.CAMERA)) requestPermissions(arrayOf(Manifest.permission.CAMERA), PERMISSION_REQUEST)
            else {
                cameraState.removeCallbacks(restartCameras)
                postureClient?.stop()
                questionClient?.stop()
                cameraState.text = "等待相机资源释放后重试已绑定设备"
                cameraState.postDelayed(restartCameras, 1_000)
            }
        }
        findViewById<Button>(R.id.copyCameraJsonButton).setOnClickListener {
            updateJson(); copyJson("相机角色", cameraJson.text.toString())
        }
        findViewById<Button>(R.id.copyPostureJsonButton).setOnClickListener {
            updateJson(); copyJson("坐姿业务快照", postureJson.text.toString())
        }
        findViewById<Button>(R.id.captureAnnotatedButton).setOnClickListener {
            val client = postureClient
            if (client == null) { photoState.text = "请先确认坐姿摄像头"; return@setOnClickListener }
            client.captureAnnotatedBitmap().onSuccess { bitmap ->
                snapshot.setImageBitmap(bitmap)
                capturedBitmap?.recycle()
                capturedBitmap = bitmap
                photoState.text = "坐姿标注诊断图：${bitmap.width} × ${bitmap.height}"
            }.onFailure { photoState.text = "标注截图失败：${it.message}" }
        }
        findViewById<Button>(R.id.capturePhotoButton).setOnClickListener {
            val client = questionClient
            if (client == null) { photoState.text = "未绑定拍题角色；不会借用坐姿摄像头"; return@setOnClickListener }
            if (photoInProgress) return@setOnClickListener
            photoInProgress = true
            findViewById<Button>(R.id.capturePhotoButton).isEnabled = false
            client.capturePhoto { result ->
                photoInProgress = false
                if (isDestroyed) return@capturePhoto
                findViewById<Button>(R.id.capturePhotoButton).isEnabled = true
                result.onSuccess { jpeg ->
                    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                    BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size, bounds)
                    photoState.text = "独立拍题预览 JPEG：${bounds.outWidth} × ${bounds.outHeight}，${jpeg.size} 字节；未上传"
                }.onFailure { photoState.text = "拍照失败：${it.message}" }
            }
        }
        findViewById<Button>(R.id.retrySpeechButton).setOnClickListener {
            if (hasPermission(Manifest.permission.RECORD_AUDIO)) maybeStartSpeech()
            else requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), PERMISSION_REQUEST)
        }
        findViewById<Button>(R.id.refreshMcuButton).setOnClickListener { mcu.refreshDevices() }
        lightSwitch.setOnCheckedChangeListener { _, enabled ->
            if (!applyingMcuStatus && canSubmitCommand()) submit(mcu.setLight(enabled, brightness.progress))
        }
        brightness.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) = Unit
            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) {
                if (!applyingMcuStatus && canSubmitCommand()) submit(mcu.setLight(lightSwitch.isChecked, brightness.progress))
            }
        })
        motionButtons = listOf(R.id.forwardButton to McuMotion.FORWARD, R.id.backwardButton to McuMotion.BACKWARD,
            R.id.leftButton to McuMotion.TURN_LEFT, R.id.rightButton to McuMotion.TURN_RIGHT).map { (id, motion) ->
            findViewById<Button>(id).apply { setOnClickListener { if (canSubmitCommand()) submit(mcu.setMotion(motion)) } }
        }
        // STOP bypasses the pending-command UI gate, including when disconnected.
        findViewById<Button>(R.id.stopButton).setOnClickListener { submit(mcu.stopMotion()) }
        val modes = mcu.capabilities.lightModes.sorted()
        modeSelection.adapter = adapter(if (modes.isEmpty()) listOf("无固件支持的灯光模式") else modes.map { "模式 $it" })
        val joints = mcu.capabilities.joints
        jointSelection.adapter = adapter(if (joints.isEmpty()) listOf("无固件支持的机械臂关节") else joints.map {
            "关节 ${it.jointId}：${it.minPositionMilliDegrees}..${it.maxPositionMilliDegrees} 毫度"
        })
        findViewById<TextView>(R.id.mcuCapabilities).text = if (modes.isEmpty() && joints.isEmpty()) {
            "模式/机械臂：没有经固件验证的 capability，不支持；不发送扩展帧"
        } else "固件 capability：${mcu.capabilities.firmwareDescriptorId}"
        findViewById<Button>(R.id.setLightModeButton).apply {
            text = if (modes.isEmpty()) "灯光模式：不支持" else "应用所选灯光模式"
            setOnClickListener { if (canSubmitCommand() && modes.isNotEmpty()) submit(mcu.setLightMode(modes[modeSelection.selectedItemPosition], brightness.progress)) }
        }
        findViewById<Button>(R.id.setArmButton).apply {
            text = if (joints.isEmpty()) "机械臂：不支持" else "应用机械臂目标"
            setOnClickListener {
                if (!canSubmitCommand() || joints.isEmpty()) return@setOnClickListener
                val position = findViewById<EditText>(R.id.armPositionInput).text.toString().toIntOrNull()
                val speed = findViewById<EditText>(R.id.armSpeedInput).text.toString().toIntOrNull()
                if (position == null || speed == null) { mcuResult.text = "请填写目标毫度和毫度/秒"; return@setOnClickListener }
                submit(mcu.setArmPosition(joints[jointSelection.selectedItemPosition].jointId, position, speed))
            }
        }
        updateMcuControls()
    }

    private fun adapter(values: List<String>) = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, values)

    private fun refreshCameraInventory() {
        cameraState.removeCallbacks(restartCameras)
        postureClient?.close(); postureClient = null
        questionClient?.close(); questionClient = null
        findViewById<FrameLayout>(R.id.previewHost).removeAllViews()
        findViewById<FrameLayout>(R.id.questionPreviewHost).removeAllViews()
        cameras = runCatching { inventory.enumerate() }.getOrElse {
            cameraState.text = "枚举失败：${it.message}"; emptyList()
        }
        registry.refresh(cameras)
        topologyChanged = false
        cameraEvents.clear()
        val choices = listOf("请选择并核对预览") + cameras.map { "Camera ${it.cameraId} · ${it.capabilityFingerprint.take(18)}" }
        postureSelection.adapter = adapter(choices)
        questionSelection.adapter = adapter(choices)
        cameraState.text = "枚举到 ${cameras.size} 个 Camera2 ID；请确认坐姿角色"
        questionState.text = "请确认不同的拍题设备；仅一个 ID 时拍题角色不可同时配置"
        updateJson()
    }

    private fun bindRole(role: CameraRole, selection: Spinner) {
        val descriptor = cameras.getOrNull(selection.selectedItemPosition - 1)
        val label = if (role == CameraRole.POSTURE) cameraState else questionState
        if (topologyChanged) { label.text = "设备拓扑已变化，请重新枚举后确认角色"; return }
        if (descriptor == null) { label.text = "请先选择设备"; return }
        val resolution = registry.bind(role, descriptor.cameraId)
        label.text = "${resolution.status}：${resolution.message}"
        if (resolution.status.name !in setOf("BOUND", "SESSION_BOUND")) { updateJson(); return }
        if (role == CameraRole.POSTURE) {
            postureClient?.close()
            postureClient = PostureRecognitionClient(this, ::showObservation, ::showRecognitionState,
                config = CameraSessionConfig(cameraId = descriptor.cameraId),
                onCameraEvent = { showCameraEvent(CameraRole.POSTURE, it) },
                onBusinessResult = { updateJson() })
            attachPreview(R.id.previewHost, postureClient!!.previewView)
        } else {
            questionClient?.close()
            questionClient = QuestionCameraClient(this,
                config = CameraSessionConfig(cameraId = descriptor.cameraId, analysisEnabled = false),
                onEvent = { showCameraEvent(CameraRole.QUESTION, it) })
            attachPreview(R.id.questionPreviewHost, questionClient!!.previewView)
        }
        if (foreground && hasPermission(Manifest.permission.CAMERA)) startBoundCameras()
        else if (!hasPermission(Manifest.permission.CAMERA)) requestPermissions(arrayOf(Manifest.permission.CAMERA), PERMISSION_REQUEST)
        updateJson()
    }

    private fun attachPreview(hostId: Int, view: android.view.View) {
        findViewById<FrameLayout>(hostId).apply {
            removeAllViews()
            addView(view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        }
    }

    private fun showCameraEvent(role: CameraRole, event: CameraEvent) {
        cameraEvents[role] = event.toString()
        (if (role == CameraRole.POSTURE) cameraState else questionState).text = "$role：$event"
        if (event is CameraEvent.Missing || event is CameraEvent.Disconnected) {
            // A reappearing Camera2 ID does not establish the same physical USB camera.
            topologyChanged = true
            registry.unbind(role)
            if (role == CameraRole.POSTURE) postureClient?.stop() else questionClient?.stop()
            (if (role == CameraRole.POSTURE) cameraState else questionState).text = "$role：$event；请重新枚举并确认"
        }
        updateJson()
    }

    private fun updateJson() {
        val devices = JSONArray()
        cameras.forEach { devices.put(JSONObject().put("cameraId", it.cameraId)
            .put("capabilityFingerprint", it.capabilityFingerprint).put("stableKey", it.stableKey ?: JSONObject.NULL)) }
        val roles = JSONObject()
        CameraRole.entries.forEach { role ->
            val binding = registry.resolve(role)
            roles.put(role.name, JSONObject().put("status", binding.status.name)
                .put("cameraId", binding.cameraId ?: JSONObject.NULL).put("message", binding.message)
                .put("lastEvent", cameraEvents[role] ?: JSONObject.NULL))
        }
        cameraJson.text = JSONObject().put("sdkVersion", RobotSdkInfo.VERSION)
            .put("deviceInfo", JSONObject(RobotDeviceInfo.capture(this).toJson()))
            .put("devices", devices).put("roles", roles).toString(2)
        postureJson.text = postureClient?.latestBusinessSnapshot(maxAgeMs = 2_000)?.toJson()
            ?: JSONObject().put("sampleState", "POSTURE_ROLE_UNBOUND").put("result", JSONObject.NULL).toString(2)
    }

    private fun copyJson(label: String, json: String) {
        getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText(label, json))
        Toast.makeText(this, "已复制 $label；未发送蓝牙或网络消息", Toast.LENGTH_SHORT).show()
    }

    private fun showObservation(observation: PostureObservation) {
        postureState.text = when {
            !observation.personDetected -> "未检测到人；不能判定为坐姿正常"
            observation.posture == null -> "有人体，坐姿暂不可判定"
            else -> "${observation.posture?.state} · ${observation.posture?.issues}\n推理 ${observation.pose.inferenceTimeMs.toInt()} ms；业务判定见快照"
        }
    }

    private fun showRecognitionState(state: RecognitionState) { cameraState.text = state.toString(); updateJson() }
    private fun showSpeechState(state: SpeechRecognitionState) {
        speechState.text = when (state) {
            SpeechRecognitionState.LoadingModel -> "正在加载语音模型"
            is SpeechRecognitionState.Idle -> {
                speechReady = true
                if (foreground) maybeStartSpeech()
                "语音模型就绪 · ${state.transcript}"
            }
            is SpeechRecognitionState.Listening -> "${state.inputDevice} · 电平 ${state.inputLevelPercent}%\n临时文字：${state.transcript}"
            is SpeechRecognitionState.Error -> state.message
        }
    }
    private fun showFinalTranscript(result: SpeechFinalTranscript) { speechFinal.text = "持续识别最终语句：${result.text}\n${result.toJson()}" }

    private fun canSubmitCommand() = mcuConnected && pendingCommands.isEmpty() &&
        !mcu.isStatusStale() && latestMcuFaultFlags == 0
    private fun submit(submission: McuCommandSubmission) {
        if (submission.accepted) pendingCommands += submission.commandId
        mcuResult.text = "${submission.command} #${submission.commandId}：" +
            if (submission.accepted) "处理中，等待接收与实际状态" else "提交失败 ${submission.outcome}"
        updateMcuControls()
    }
    private fun updateMcuControls() {
        val available = canSubmitCommand()
        lightSwitch.isEnabled = available && mcu.capabilities.supportsBasicLight
        brightness.isEnabled = lightSwitch.isEnabled
        motionButtons.forEach { it.isEnabled = available && mcu.capabilities.supportsBasicMotion }
        findViewById<Button>(R.id.stopButton).isEnabled = true
        findViewById<Button>(R.id.setLightModeButton).isEnabled = available && mcu.capabilities.lightModes.isNotEmpty()
        findViewById<Button>(R.id.setArmButton).isEnabled = available && mcu.capabilities.joints.isNotEmpty()
        modeSelection.isEnabled = available && mcu.capabilities.lightModes.isNotEmpty()
        jointSelection.isEnabled = available && mcu.capabilities.joints.isNotEmpty()
        findViewById<EditText>(R.id.armPositionInput).isEnabled = available && mcu.capabilities.joints.isNotEmpty()
        findViewById<EditText>(R.id.armSpeedInput).isEnabled = available && mcu.capabilities.joints.isNotEmpty()
        if (mcuConnected && mcu.isStatusStale()) mcuStatus.text = "实际状态已过期或尚无回读；不可据此宣称动作完成"
    }
    private fun showMcuConnection(state: McuConnectionState) {
        mcuConnected = state is McuConnectionState.Connected
        mcuState.text = state.toString()
        if (!mcuConnected) {
            pendingCommands.clear()
            latestMcuFaultFlags = null
            mcuStatus.text = "未连接；没有有效实际状态"
        }
        updateMcuControls()
    }
    private fun showMcuStatus(status: McuStatus) {
        latestMcuFaultFlags = status.faultFlags
        applyingMcuStatus = true
        lightSwitch.isChecked = status.lightEnabled
        brightness.progress = status.brightnessPercent
        applyingMcuStatus = false
        mcuStatus.text = "实际亮度 ${status.brightnessPercent}% · 运动 ${status.motion} · 故障 0x${status.faultFlags.toString(16)}\n" +
            "回读 ${status.receivedAtElapsedMs} ms · stale=${status.stale} · 关联 ${status.relatedCommandIds}"
        updateMcuControls()
    }
    private fun showCommandResult(result: McuCommandResult) {
        if (result.outcome.name !in setOf("ACK_RECEIVED", "ACKNOWLEDGED")) pendingCommands -= result.commandId
        mcuResult.text = "${result.command} #${result.commandId} / seq ${result.sequence}：${result.outcome}" +
            if (result.outcome == McuCommandOutcome.ACK_RECEIVED) "（仅收到；继续等实际状态）"
            else "（code=${result.resultCode}，detail=${result.detail}）"
        updateMcuControls()
    }

    override fun onResume() {
        super.onResume()
        foreground = true
        startGrantedFeatures()
        cameraJson.removeCallbacks(refreshJson)
        cameraJson.post(refreshJson)
        if (!permissionRequested) {
            val missing = listOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO).filterNot(::hasPermission)
            if (missing.isNotEmpty()) { permissionRequested = true; requestPermissions(missing.toTypedArray(), PERMISSION_REQUEST) }
        }
    }
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == PERMISSION_REQUEST && foreground) startGrantedFeatures()
    }
    private fun startGrantedFeatures() {
        if (hasPermission(Manifest.permission.CAMERA)) startBoundCameras()
        maybeStartSpeech()
    }
    private fun startBoundCameras() {
        if (registry.resolve(CameraRole.POSTURE).cameraId != null) postureClient?.start()
        if (registry.resolve(CameraRole.QUESTION).cameraId != null) questionClient?.start()
    }
    private fun maybeStartSpeech() {
        if (foreground && speechReady && hasPermission(Manifest.permission.RECORD_AUDIO) && !speechRecognizer.isRecording) speechRecognizer.start()
    }
    private fun hasPermission(permission: String) = checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED
    override fun onPause() {
        foreground = false
        cameraJson.removeCallbacks(refreshJson)
        cameraState.removeCallbacks(restartCameras)
        submit(mcu.pause())
        postureClient?.stop(); questionClient?.stop(); speechRecognizer.stop()
        super.onPause()
    }
    override fun onDestroy() {
        mcu.close(); speechRecognizer.close(); postureClient?.close(); questionClient?.close()
        snapshot.setImageDrawable(null)
        capturedBitmap?.recycle()
        super.onDestroy()
    }
    private companion object { const val PERMISSION_REQUEST = 1001 }
}
