package com.example.rk3566hardwarevalidator.sdk

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.example.rk3566hardwarevalidator.hardware.camera.Camera2PreviewController
import com.example.rk3566hardwarevalidator.hardware.photo.PhotoCropRegion
import com.example.rk3566hardwarevalidator.hardware.pose.PoseEngineStatus
import com.example.rk3566hardwarevalidator.hardware.pose.PoseFrame
import com.example.rk3566hardwarevalidator.hardware.pose.PoseResult
import com.example.rk3566hardwarevalidator.hardware.pose.RknnPoseEngine
import com.example.rk3566hardwarevalidator.hardware.posture.PostureDetection
import com.example.rk3566hardwarevalidator.hardware.posture.PostureDetector
import com.example.rk3566hardwarevalidator.hardware.posture.PostureThresholds
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

data class PostureObservation(
    val frameTimestampNanos: Long,
    val frameWidth: Int,
    val frameHeight: Int,
    val pose: PoseResult,
    val posture: PostureDetection?
) {
    val personDetected: Boolean get() = pose.keypoints.isNotEmpty()
}

enum class RecognitionErrorCode { PERMISSION_DENIED, MODEL_UNAVAILABLE, INFERENCE_FAILED }

sealed interface RecognitionState {
    data object Stopped : RecognitionState
    data object LoadingModel : RecognitionState
    data object StartingCamera : RecognitionState
    data object Running : RecognitionState
    data class CameraMessage(val message: String) : RecognitionState
    data class Error(val message: String, val code: RecognitionErrorCode = RecognitionErrorCode.MODEL_UNAVAILABLE) : RecognitionState
}

/**
 * Application-level facade: this SDK owns Camera2 capture, RKNN inference and posture rules.
 * Public methods and listeners run on the main thread; inference runs on the camera's worker.
 * The host APK must grant CAMERA before [start] and attach [previewView] to its layout.
 */
class PostureRecognitionClient(
    context: Context,
    private val onObservation: (PostureObservation) -> Unit,
    private val onStateChanged: (RecognitionState) -> Unit = {},
    config: CameraSessionConfig = CameraSessionConfig(),
    private val onCameraEvent: (CameraEvent) -> Unit = {},
    thresholds: PostureThresholds = PostureThresholds(),
    private val onBusinessResult: (PostureBusinessResult) -> Unit = {}
) : AutoCloseable {
    val previewView = PosturePreviewView(context)

    private val appContext = context.applicationContext
    private val mainHandler = Handler(Looper.getMainLooper())
    private val engine = RknnPoseEngine(appContext)
    private var detector = PostureDetector(thresholds)
    private val analysisLock = Any()
    private val resultTracker = PostureResultTracker()
    private val businessListeners = CopyOnWriteArrayList<(PostureBusinessResult) -> Unit>()
    @Volatile private var latestBusinessResult: PostureBusinessResult? = null
    private val modelExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "PostureModelInit")
    }
    private val running = AtomicBoolean(false)
    private val generation = AtomicInteger(0)
    private val resultEpoch = AtomicInteger(0)
    @Volatile
    private var modelReady = false
    @Volatile
    private var modelFailed = false
    private var closed = false
    private var reportedRunningSession = -1

    @Volatile
    private var latestObservation: PostureObservation? = null

    private val camera = Camera2PreviewController(
        context = appContext,
        previewView = previewView.cameraTexture,
        onStatusChanged = { message ->
            mainHandler.post {
                if (running.get() && !closed) {
                    previewView.showMessage(message)
                    onStateChanged(RecognitionState.CameraMessage(message))
                }
            }
        },
        onFrameAvailable = ::processFrame,
        config = config.copy(analysisEnabled = true),
        onEvent = { event ->
            if (!closed) {
                if (event is CameraEvent.Disconnected || event is CameraEvent.Missing || event is CameraEvent.Error || event is CameraEvent.Busy) {
                    resetResults()
                    previewView.render(null)
                }
                onCameraEvent(event)
            }
        }
    )

    fun latest(): PostureObservation? = latestObservation

    /** Applies validated rules without reopening the camera/model; discards old smoothing data. */
    fun setThresholds(thresholds: PostureThresholds) {
        requireMainThread()
        check(!closed) { "Client is closed" }
        val replacement = PostureDetector(thresholds)
        synchronized(analysisLock) {
            detector = replacement
            resetResults()
        }
        previewView.render(null)
    }

    fun latestBusinessSnapshot(maxAgeMs: Long = 2_000): PostureBusinessSnapshot =
        PostureResultTracker.snapshot(latestBusinessResult, SystemClock.elapsedRealtime(), maxAgeMs)

    /** Callback is main-thread; closing the subscription is idempotent and removes its listener. */
    fun subscribeBusiness(listener: (PostureBusinessResult) -> Unit): SdkSubscription {
        requireMainThread()
        check(!closed) { "Client is closed" }
        businessListeners.add(listener)
        return SdkSubscription { businessListeners.remove(listener) }
    }

    /** Whether the Camera2 preview can currently produce a still photo. */
    val isPreviewActive: Boolean get() = running.get() && camera.isPreviewActive

    fun start() {
        requireMainThread()
        check(!closed) { "Client is closed" }
        if (!running.compareAndSet(false, true)) return
        val session = generation.incrementAndGet()
        if (appContext.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            running.set(false)
            onStateChanged(RecognitionState.Error("缺少 CAMERA 运行时权限", RecognitionErrorCode.PERMISSION_DENIED))
            return
        }
        // Photo capture and camera diagnostics must remain usable if the NPU is unavailable.
        startCamera(session)
        if (closed || !running.get() || generation.get() != session) return
        if (modelReady) return
        modelFailed = false
        previewView.showMessage("正在加载姿态模型")
        onStateChanged(RecognitionState.LoadingModel)
        if (closed || !running.get() || generation.get() != session) return
        modelExecutor.execute {
            val status = engine.initialize()
            mainHandler.post {
                if (closed || !running.get() || generation.get() != session) return@post
                when (status) {
                    PoseEngineStatus.Ready -> {
                        modelReady = true
                        previewView.showMessage("等待识别")
                    }
                    is PoseEngineStatus.Unavailable -> reportModelFailure(status.reason)
                }
            }
        }
    }

    fun stop() {
        requireMainThread()
        generation.incrementAndGet()
        running.set(false)
        reportedRunningSession = -1
        camera.stop()
        resetResults()
        previewView.render(null)
        if (!closed) onStateChanged(RecognitionState.Stopped)
    }

    fun switchCamera() {
        requireMainThread()
        check(running.get()) { "Camera is not running" }
        // Keep an in-flight model initialization valid when switching cameras.
        if (modelReady) generation.incrementAndGet()
        reportedRunningSession = -1
        resetResults()
        previewView.render(null)
        val session = generation.get()
        onStateChanged(RecognitionState.StartingCamera)
        if (!closed && running.get() && generation.get() == session) camera.switchCamera()
    }

    fun selectCamera(cameraId: String) {
        requireMainThread()
        check(!closed) { "Client is closed" }
        if (modelReady) generation.incrementAndGet()
        reportedRunningSession = -1
        resetResults()
        previewView.render(null)
        camera.selectCamera(cameraId)
    }

    fun retryCamera() {
        requireMainThread()
        check(!closed) { "Client is closed" }
        resetResults()
        camera.retryCamera()
    }

    fun capturePhoto(region: PhotoCropRegion? = null, onResult: (Result<ByteArray>) -> Unit) {
        requireMainThread()
        if (closed) { onResult(Result.failure(IllegalStateException("Client is closed"))); return }
        camera.captureStillPhoto(region) { result ->
            onResult(if (closed) Result.failure(IllegalStateException("Client is closed")) else result)
        }
    }

    fun captureAnnotatedBitmap(): Result<Bitmap> = previewView.captureAnnotatedBitmap()

    override fun close() {
        requireMainThread()
        if (closed) return
        closed = true
        stop()
        businessListeners.clear()
        modelExecutor.execute { engine.close() }
        modelExecutor.shutdown()
    }

    private fun startCamera(session: Int) {
        if (closed || !running.get() || generation.get() != session) return
        onStateChanged(RecognitionState.StartingCamera)
        if (!closed && running.get() && generation.get() == session) camera.start()
    }

    private fun processFrame(frame: PoseFrame) {
        val session = generation.get()
        val epoch = resultEpoch.get()
        if (!running.get() || !modelReady || modelFailed) return
        try {
            val pose = engine.detect(frame)
            val mapped = synchronized(analysisLock) {
                if (!running.get() || generation.get() != session || resultEpoch.get() != epoch) return
                val posture = if (pose.keypoints.isEmpty()) { detector.reset(); null }
                    else detector.update(pose, frame.width, frame.height)
                posture to resultTracker.update(pose, posture, System.currentTimeMillis(), SystemClock.elapsedRealtime(),
                    frame.timestampNanos, frame.width, frame.height)
            }
            val posture = mapped.first
            val business = mapped.second
            val observation = PostureObservation(
                frameTimestampNanos = frame.timestampNanos,
                frameWidth = frame.width,
                frameHeight = frame.height,
                pose = pose,
                posture = posture
            )
            if (running.get() && generation.get() == session && resultEpoch.get() == epoch) {
                mainHandler.post {
                    if (running.get() && !closed && generation.get() == session && resultEpoch.get() == epoch) {
                        latestObservation = observation
                        latestBusinessResult = business
                        if (reportedRunningSession != session) {
                            reportedRunningSession = session
                            onStateChanged(RecognitionState.Running)
                        }
                        if (!running.get() || closed || generation.get() != session || resultEpoch.get() != epoch) return@post
                        previewView.render(observation)
                        onObservation(observation)
                        if (!running.get() || closed || generation.get() != session || resultEpoch.get() != epoch) return@post
                        onBusinessResult(business)
                        businessListeners.forEach {
                            if (running.get() && !closed && generation.get() == session && resultEpoch.get() == epoch && businessListeners.contains(it)) it(business)
                        }
                    }
                }
            }
        } catch (error: Throwable) {
            mainHandler.post {
                if (running.get() && generation.get() == session && resultEpoch.get() == epoch && !closed) {
                    reportModelFailure("姿态推理失败：${error.message ?: error.javaClass.simpleName}", RecognitionErrorCode.INFERENCE_FAILED)
                }
            }
        }
    }

    private fun reportModelFailure(message: String, code: RecognitionErrorCode = RecognitionErrorCode.MODEL_UNAVAILABLE) {
        modelFailed = true
        modelReady = false
        reportedRunningSession = -1
        resetResults()
        previewView.render(null)
        previewView.showMessage(message)
        modelExecutor.execute { engine.close() }
        onStateChanged(RecognitionState.Error(message, code))
    }

    private fun resetResults() = synchronized(analysisLock) {
        resultEpoch.incrementAndGet()
        reportedRunningSession = -1
        detector.reset()
        resultTracker.reset()
        latestObservation = null
        latestBusinessResult = null
    }

    private fun requireMainThread() {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Call on the main thread" }
    }
}
