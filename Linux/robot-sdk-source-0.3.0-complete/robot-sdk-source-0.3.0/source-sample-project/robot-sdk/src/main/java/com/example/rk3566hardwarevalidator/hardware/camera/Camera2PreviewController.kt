package com.example.rk3566hardwarevalidator.hardware.camera

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.Image
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.util.Size
import android.view.Surface
import android.view.TextureView
import androidx.core.content.ContextCompat
import com.example.rk3566hardwarevalidator.hardware.pose.PoseFrame
import com.example.rk3566hardwarevalidator.hardware.pose.PosePixelFormat
import com.example.rk3566hardwarevalidator.hardware.photo.PhotoCropRegion
import com.example.rk3566hardwarevalidator.sdk.CameraEvent
import com.example.rk3566hardwarevalidator.sdk.CameraSessionConfig
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Android 标准 Camera2 预览实现。
 *
 * 该类只负责平台相机能力，不包含 RKNN、坐姿规则或任何板厂私有 API。
 */
class Camera2PreviewController(
    context: Context,
    private val previewView: TextureView,
    private val onStatusChanged: (String) -> Unit,
    private val onFrameAvailable: (PoseFrame) -> Unit,
    private val config: CameraSessionConfig = CameraSessionConfig(),
    private val onEvent: (CameraEvent) -> Unit = {}
) {
    /** Keeps the original four-argument JVM signature and Kotlin trailing-frame-lambda calls. */
    constructor(
        context: Context,
        previewView: TextureView,
        onStatusChanged: (String) -> Unit,
        onFrameAvailable: (PoseFrame) -> Unit
    ) : this(context, previewView, onStatusChanged, onFrameAvailable, CameraSessionConfig(), {})

    private val cameraManager = context.getSystemService(CameraManager::class.java)
    private val appContext = context.applicationContext
    private val mainHandler = Handler(Looper.getMainLooper())

    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    @Volatile
    private var cameraDevice: CameraDevice? = null

    @Volatile
    private var captureSession: CameraCaptureSession? = null
    private var previewSurface: Surface? = null
    private var imageReader: ImageReader? = null
    private var analysisExecutor: ExecutorService? = null
    private var analysisBuffer: ByteBuffer? = null
    private val analysisBusy = AtomicBoolean(false)
    // ImageReader.close() invalidates acquired planes. Keep release and plane reads exclusive.
    private val cameraStateLock = Any()
    private val analysisFrameRateLimiter = FrameRateLimiter(config.fps)
    private var cameraIds: List<String> = emptyList()
    private var selectedCameraId: String? = config.cameraId
    private var generation = 0L
    @Volatile private var lifecycleEpoch = 0L
    private var openingLease: Any? = null
    private var waitingForReconnect = false
    private var recoveryEligible = false
    private val photoBusy = AtomicBoolean(false)
    private var hasPreviewFrame = false
    private var previewBaselineTimestamp = 0L
    @Volatile
    private var isStarted = false

    val isPreviewActive: Boolean
        get() = synchronized(cameraStateLock) { isStarted && cameraDevice != null && captureSession != null && hasPreviewFrame }

    private val surfaceTextureListener = object : TextureView.SurfaceTextureListener {
        override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int) {
            openSelectedCamera(width, height)
        }

        override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) = Unit

        override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
            closeCamera()
            return true
        }

        override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {
            synchronized(cameraStateLock) {
                if (!isStarted || captureSession == null || hasPreviewFrame ||
                    surface !== previewView.surfaceTexture || surface.timestamp == previewBaselineTimestamp) return
                val id = cameraDevice?.id ?: return
                hasPreviewFrame = true
                event(CameraEvent.PreviewStarted(id))
            }
        }
    }

    private val availabilityCallback = object : CameraManager.AvailabilityCallback() {
        override fun onCameraAvailable(cameraId: String) {
            synchronized(cameraStateLock) {
                if (!isStarted || cameraId != selectedCameraId || !waitingForReconnect) return
                event(CameraEvent.Available(cameraId))
                if (config.recoverOnReconnect && recoveryEligible) {
                    waitingForReconnect = false
                    recoveryEligible = false
                    openSelectedCamera(previewView.width, previewView.height)
                }
            }
        }

        override fun onCameraUnavailable(cameraId: String) = Unit
    }

    fun start() {
        requireMainThread()
        if (isStarted) return
        lifecycleEpoch++
        isStarted = true
        waitingForReconnect = false
        recoveryEligible = false
        startCameraThread()
        if (config.analysisEnabled) startAnalysisExecutor()
        previewView.surfaceTextureListener = surfaceTextureListener
        cameraManager.registerAvailabilityCallback(availabilityCallback, mainHandler)

        cameraIds = try {
            cameraManager.cameraIdList.toList()
        } catch (error: CameraAccessException) {
            onStatusChanged("读取 Camera ID 失败：${error.reason}")
            emptyList()
        }

        if (cameraIds.isEmpty()) {
            onStatusChanged("Camera2 未枚举到摄像头\n请检查 UVC/MIPI 驱动和 Camera HAL")
            selectedCameraId?.let {
                waitingForReconnect = true
                recoveryEligible = true
                event(CameraEvent.Missing(it))
            } ?: event(CameraEvent.Error(null, "Camera2 未枚举到摄像头"))
            return
        }

        if (selectedCameraId == null) selectedCameraId = cameraIds.first()
        if (previewView.isAvailable) {
            openSelectedCamera(previewView.width, previewView.height)
        } else {
            previewView.surfaceTextureListener = surfaceTextureListener
        }
    }

    fun stop() {
        requireMainThread()
        lifecycleEpoch++
        isStarted = false
        cameraManager.unregisterAvailabilityCallback(availabilityCallback)
        previewView.surfaceTextureListener = null
        closeCamera()
        // Let an in-flight inference clear analysisBusy before the next start.
        analysisExecutor?.shutdown()
        analysisExecutor = null
        analysisBuffer = null
        analysisFrameRateLimiter.reset()
        cameraThread?.quitSafely()
        cameraThread = null
        cameraHandler = null
        event(CameraEvent.Stopped)
    }

    fun switchCamera() {
        requireMainThread()
        if (cameraIds.size < 2) {
            onStatusChanged("当前 Camera2 只枚举到 1 个 Camera ID")
            return
        }

        val index = cameraIds.indexOf(selectedCameraId)
        selectCamera(cameraIds[(index + 1).mod(cameraIds.size)])
    }

    /** An explicit missing ID remains selected; it never falls back to another camera. */
    fun selectCamera(cameraId: String) {
        requireMainThread()
        require(cameraId.isNotBlank()) { "cameraId must not be blank" }
        synchronized(cameraStateLock) {
            selectedCameraId = cameraId
            waitingForReconnect = false
            recoveryEligible = false
            closeCamera()
            if (isStarted && previewView.isAvailable) openSelectedCamera(previewView.width, previewView.height)
        }
    }

    /** Controlled retry after a missing camera, disconnect or HAL error. */
    fun retryCamera() {
        requireMainThread()
        synchronized(cameraStateLock) {
            waitingForReconnect = false
            recoveryEligible = false
            if (isStarted && previewView.isAvailable) openSelectedCamera(previewView.width, previewView.height)
        }
    }

    fun captureStillPhoto(region: PhotoCropRegion? = null, onResult: (Result<ByteArray>) -> Unit) {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Capture on the main thread" }
        if (!isPreviewActive || !previewView.isAvailable) {
            mainHandler.post { onResult(Result.failure(IllegalStateException("相机尚未准备好拍照"))) }
            return
        }
        if (!photoBusy.compareAndSet(false, true)) {
            mainHandler.post { onResult(Result.failure(IllegalStateException("已有拍照请求正在处理"))) }
            return
        }
        val photoGeneration = synchronized(cameraStateLock) { generation }

        val bitmap = try {
            previewView.bitmap ?: error("无法读取相机预览帧")
        } catch (error: Throwable) {
            photoBusy.set(false)
            mainHandler.post { onResult(Result.failure(error)) }
            return
        }

        Thread({
            var cropped: Bitmap? = null
            val result = runCatching {
                val encodedBitmap = if (region == null) bitmap else {
                    val bounds = region.pixels(bitmap.width, bitmap.height)
                    Bitmap.createBitmap(bitmap, bounds.x, bounds.y, bounds.width, bounds.height)
                        .also { cropped = it }
                }
                ByteArrayOutputStream().use { output ->
                    check(
                        encodedBitmap.compress(
                            Bitmap.CompressFormat.JPEG,
                            PREVIEW_JPEG_QUALITY,
                            output
                        )
                    ) { "预览帧 JPEG 压缩失败" }
                    output.toByteArray()
                }
            }
            cropped?.takeUnless { it === bitmap }?.recycle()
            bitmap.recycle()
            mainHandler.post {
                photoBusy.set(false)
                val current = synchronized(cameraStateLock) { isStarted && generation == photoGeneration }
                onResult(if (current) result else Result.failure(IllegalStateException("拍照期间相机会话已停止或切换")))
            }
        }, "CameraPreviewCapture").start()
    }

    private fun startCameraThread() {
        if (cameraThread != null) return
        cameraThread = HandlerThread("Camera2Preview").also { it.start() }
        cameraHandler = Handler(cameraThread!!.looper)
    }

    private fun startAnalysisExecutor() {
        if (analysisExecutor != null) return
        analysisExecutor = Executors.newSingleThreadExecutor { task ->
            Thread(task, "RknnPoseInference")
        }
    }

    @SuppressLint("MissingPermission")
    private fun openSelectedCamera(viewWidth: Int, viewHeight: Int) {
        synchronized(cameraStateLock) {
        if (!isStarted || !previewView.isAvailable || cameraDevice != null || openingLease != null || waitingForReconnect) return
        if (ContextCompat.checkSelfPermission(appContext, Manifest.permission.CAMERA) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            reportError(selectedCameraId, "没有相机权限")
            return
        }

        val cameraId = selectedCameraId ?: return
        cameraIds = try { cameraManager.cameraIdList.toList() } catch (error: CameraAccessException) {
            reportError(cameraId, "读取 Camera ID 失败：${error.reason}")
            return
        }
        if (cameraId !in cameraIds) {
            waitingForReconnect = true
            recoveryEligible = true
            onStatusChanged("指定 Camera $cameraId 不存在，等待重新连接或人工重试")
            event(CameraEvent.Missing(cameraId))
            return
        }
        val characteristics = try {
            cameraManager.getCameraCharacteristics(cameraId)
        } catch (error: CameraAccessException) {
            reportError(cameraId, "读取 Camera $cameraId 能力失败：${error.reason}")
            return
        }
        val streamMap = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val selection = CameraStreamSelector.select(
            streamMap?.getOutputSizes(SurfaceTexture::class.java).orEmpty().map {
                StreamDimensions(it.width, it.height)
            },
            (if (config.analysisEnabled) streamMap?.getOutputSizes(ImageFormat.YUV_420_888).orEmpty() else emptyArray()).map {
                StreamDimensions(it.width, it.height)
            },
            viewWidth,
            viewHeight
        )
        val previewSize = Size(selection.preview.width, selection.preview.height)
        val analysisSize = selection.analysis?.let { Size(it.width, it.height) }

        onStatusChanged(
            buildCameraDescription(cameraId, characteristics, previewSize, analysisSize)
        )
        val lease = Any()
        if (!CameraOwnership.acquire(cameraId, lease)) {
            onStatusChanged("Camera $cameraId 已被另一相机客户端占用")
            event(CameraEvent.Busy(cameraId))
            return
        }
        val attempt = ++generation
        openingLease = lease
        event(CameraEvent.Opening(cameraId))
        try {
            cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    synchronized(cameraStateLock) {
                        if (!isStarted || generation != attempt || openingLease !== lease || !previewView.isAvailable) {
                            camera.close()
                            return
                        }
                        openingLease = null
                        cameraDevice = camera
                        try {
                            createPreviewSession(camera, previewSize, analysisSize, characteristics, attempt)
                        } catch (error: Exception) {
                            waitingForReconnect = true
                            recoveryEligible = false
                            closeCamera()
                            reportError(cameraId, "创建 Camera2 会话失败：${error.message}")
                        }
                    }
                }

                override fun onDisconnected(camera: CameraDevice) {
                    synchronized(cameraStateLock) {
                        if (generation == attempt) {
                            waitingForReconnect = true
                            recoveryEligible = true
                            closeCamera()
                            onStatusChanged("Camera $cameraId 已断开")
                            event(CameraEvent.Disconnected(cameraId))
                        }
                        camera.close()
                    }
                }

                override fun onError(camera: CameraDevice, error: Int) {
                    synchronized(cameraStateLock) {
                        if (generation == attempt) {
                            waitingForReconnect = true
                            recoveryEligible = false
                            closeCamera()
                            reportError(cameraId, "Camera $cameraId 打开失败，错误码：$error")
                        }
                        camera.close()
                    }
                }
                override fun onClosed(camera: CameraDevice) {
                    CameraOwnership.release(cameraId, lease)
                    synchronized(cameraStateLock) {
                        if (isStarted && selectedCameraId == cameraId && !waitingForReconnect && cameraDevice == null && openingLease == null) {
                            openSelectedCamera(previewView.width, previewView.height)
                        }
                    }
                }
            }, mainHandler)
        } catch (error: Exception) {
            openingLease = null
            CameraOwnership.release(cameraId, lease)
            reportError(cameraId, "打开 Camera $cameraId 失败：${error.message}")
        }
        }
    }

    private fun createPreviewSession(
        camera: CameraDevice,
        previewSize: Size,
        analysisSize: Size?,
        characteristics: CameraCharacteristics,
        attempt: Long
    ) {
        val texture = checkNotNull(previewView.surfaceTexture) { "Preview surface disappeared" }
        texture.setDefaultBufferSize(previewSize.width, previewSize.height)
        val surface = Surface(texture).also { previewSurface = it }
        val targets = mutableListOf(surface)

        val analysisReader = analysisSize?.let { size ->
            ImageReader.newInstance(
                size.width,
                size.height,
                ImageFormat.YUV_420_888,
                IMAGE_READER_CAPACITY
            ).also { reader ->
                reader.setOnImageAvailableListener(::onAnalysisImageAvailable, cameraHandler)
                imageReader = reader
                targets += reader.surface
            }
        }
        val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
            addTarget(surface)
            analysisReader?.surface?.let(::addTarget)
            set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
            set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
            characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                ?.filter { it.lower <= config.fps && it.upper >= config.fps }
                ?.minByOrNull { it.upper - it.lower }
                ?.let { set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
        }

        camera.createCaptureSession(
            targets,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    synchronized(cameraStateLock) {
                        if (!isStarted || generation != attempt || cameraDevice !== camera) {
                            session.close()
                            return
                        }
                        captureSession = session
                        try {
                            // A retained TextureView can still contain a frame from the old session.
                            // Wait for a new texture update before reporting readiness or taking a JPEG.
                            previewBaselineTimestamp = texture.timestamp
                            hasPreviewFrame = false
                            session.setRepeatingRequest(request.build(), null, cameraHandler)
                        } catch (error: Exception) {
                            waitingForReconnect = true
                            recoveryEligible = false
                            closeCamera()
                            reportError(camera.id, "启动相机预览失败：${error.message}")
                        }
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    synchronized(cameraStateLock) {
                        session.close()
                        if (isStarted && generation == attempt && cameraDevice === camera) {
                            waitingForReconnect = true
                            recoveryEligible = false
                            closeCamera()
                            reportError(camera.id, "Camera2 预览会话配置失败")
                        }
                    }
                }
            },
            cameraHandler
        )
    }

    private fun closeCamera() {
        synchronized(cameraStateLock) {
            generation++
            hasPreviewFrame = false
            openingLease = null
            captureSession?.close()
            captureSession = null
            imageReader?.setOnImageAvailableListener(null, null)
            imageReader?.close()
            imageReader = null
            previewSurface?.release()
            previewSurface = null
            cameraDevice?.close()
            cameraDevice = null
            analysisFrameRateLimiter.reset()
        }
    }

    private fun onAnalysisImageAvailable(reader: ImageReader) {
        synchronized(cameraStateLock) {
            // A callback queued before stop() may run after the old reader was closed.
            if (!isStarted || reader !== imageReader) return
            val image = try {
                reader.acquireLatestImage()
            } catch (_: IllegalStateException) {
                return
            } ?: return
            try {
                if (!analysisBusy.compareAndSet(false, true)) return
                if (!analysisFrameRateLimiter.shouldProcess(SystemClock.elapsedRealtimeNanos())) {
                    analysisBusy.set(false)
                    return
                }

                val buffer = packNv12(image)
                val frame = PoseFrame(
                    buffer = buffer,
                    width = image.width,
                    height = image.height,
                    format = PosePixelFormat.NV12,
                    rotationDegrees = 0,
                    timestampNanos = image.timestamp
                )
                val executor = analysisExecutor
                if (executor == null || executor.isShutdown) {
                    analysisBusy.set(false)
                    return
                }
                val frameGeneration = generation
                executor.execute {
                    try {
                        if (isStarted && synchronized(cameraStateLock) { generation == frameGeneration }) {
                            onFrameAvailable(frame)
                        }
                    } catch (error: Throwable) {
                        Log.e(TAG, "Pose frame processing failed", error)
                    } finally {
                        analysisBusy.set(false)
                    }
                }
            } catch (error: Throwable) {
                analysisBusy.set(false)
                Log.e(TAG, "Unable to pack Camera2 YUV frame", error)
            } finally {
                image.close()
            }
        }
    }

    private fun packNv12(image: Image): ByteBuffer {
        val width = image.width
        val height = image.height
        val requiredCapacity = width * height * 3 / 2
        val output = analysisBuffer
            ?.takeIf { it.capacity() >= requiredCapacity }
            ?: ByteBuffer.allocateDirect(requiredCapacity).also { analysisBuffer = it }
        output.clear()

        val yPlane = image.planes[0]
        val yBuffer = yPlane.buffer.duplicate()
        val yBase = yBuffer.position()
        repeat(height) { row ->
            repeat(width) { column ->
                output.put(yBuffer.get(yBase + row * yPlane.rowStride + column * yPlane.pixelStride))
            }
        }

        val uPlane = image.planes[1]
        val vPlane = image.planes[2]
        val uBuffer = uPlane.buffer.duplicate()
        val vBuffer = vPlane.buffer.duplicate()
        val uBase = uBuffer.position()
        val vBase = vBuffer.position()
        repeat(height / 2) { row ->
            repeat(width / 2) { column ->
                output.put(uBuffer.get(uBase + row * uPlane.rowStride + column * uPlane.pixelStride))
                output.put(vBuffer.get(vBase + row * vPlane.rowStride + column * vPlane.pixelStride))
            }
        }
        output.flip()
        return output
    }

    private fun buildCameraDescription(
        cameraId: String,
        characteristics: CameraCharacteristics,
        previewSize: Size,
        analysisSize: Size?
    ): String {
        val facing = when (characteristics.get(CameraCharacteristics.LENS_FACING)) {
            CameraCharacteristics.LENS_FACING_FRONT -> "前置"
            CameraCharacteristics.LENS_FACING_BACK -> "后置"
            CameraCharacteristics.LENS_FACING_EXTERNAL -> "外接"
            else -> "未知"
        }
        val sizes = characteristics
            .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?.getOutputSizes(SurfaceTexture::class.java)
            .orEmpty()
            .sortedByDescending { it.width.toLong() * it.height }
        val fpsRanges = characteristics
            .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            .orEmpty()
        val fpsDescription = fpsRanges
            .joinToString { range -> "${range.lower}-${range.upper}" }
            .ifEmpty { "未报告" }

        return buildString {
            appendLine("Camera ID：$cameraId（$facing）")
            appendLine("预览：${previewSize.width} × ${previewSize.height}")
            appendLine(
                analysisSize?.let { "NPU 输入：${it.width} × ${it.height} YUV" }
                    ?: if (config.analysisEnabled) "NPU 输入：未找到安全的 YUV 双流尺寸" else "分析流：已禁用（独立预览相机）"
            )
            appendLine("搜题拍照：使用当前预览帧 JPEG（RK3566 兼容模式）")
            if (config.analysisEnabled) appendLine("坐姿检测上限：${config.fps} FPS")
            appendLine("FPS：$fpsDescription")
            append("分辨率：${sizes.take(6).joinToString { "${it.width}×${it.height}" }}")
        }
    }

    private fun event(value: CameraEvent) {
        val eventGeneration = generation
        val eventEpoch = lifecycleEpoch
        val topologyId = when (value) {
            is CameraEvent.Missing -> value.cameraId
            is CameraEvent.Disconnected -> value.cameraId
            is CameraEvent.Available -> value.cameraId
            else -> null
        }
        mainHandler.post {
            // Preserve a real disconnect notification even if automatic reconnect has already
            // opened the next session. Stop/start and selecting another ID still invalidate it.
            val current = synchronized(cameraStateLock) {
                lifecycleEpoch == eventEpoch &&
                    (generation == eventGeneration || (topologyId != null && selectedCameraId == topologyId))
            }
            if (current &&
                (isStarted || value == CameraEvent.Stopped)) onEvent(value)
        }
    }

    private fun reportError(cameraId: String?, message: String) {
        onStatusChanged(message)
        event(CameraEvent.Error(cameraId, message))
    }

    private fun requireMainThread() {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Call on the main thread" }
    }

    private companion object {
        const val TAG = "Camera2Pose"
        const val IMAGE_READER_CAPACITY = 2
        const val PREVIEW_JPEG_QUALITY = 92
    }
}

/** Lease survives close() until CameraDevice.onClosed, including late onOpened callbacks. */
internal object CameraOwnership {
    private val leases = mutableMapOf<String, Any>()
    @Synchronized fun acquire(cameraId: String, owner: Any): Boolean {
        if (leases.containsKey(cameraId)) return false
        leases[cameraId] = owner
        return true
    }
    @Synchronized fun release(cameraId: String, owner: Any) {
        if (leases[cameraId] === owner) leases.remove(cameraId)
    }
}
