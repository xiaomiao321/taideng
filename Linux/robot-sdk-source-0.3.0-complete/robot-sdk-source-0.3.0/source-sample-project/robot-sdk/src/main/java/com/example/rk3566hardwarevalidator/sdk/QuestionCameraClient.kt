package com.example.rk3566hardwarevalidator.sdk

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.view.TextureView
import com.example.rk3566hardwarevalidator.hardware.camera.Camera2PreviewController
import com.example.rk3566hardwarevalidator.hardware.photo.PhotoCropRegion

/**
 * Independent question camera. Uses one preview stream and JPEG-encodes the preview bitmap;
 * no YUV reader, pose model, NPU inference or third JPEG capture stream is allocated.
 * Public methods and callbacks use the main thread. CAMERA permission belongs to the host.
 */
class QuestionCameraClient(
    context: Context,
    config: CameraSessionConfig = CameraSessionConfig(analysisEnabled = false),
    private val onEvent: (CameraEvent) -> Unit = {}
) : AutoCloseable {
    val previewView = TextureView(context)
    private val mainHandler = Handler(Looper.getMainLooper())
    private var closed = false
    private var running = false
    private val camera = Camera2PreviewController(
        context.applicationContext,
        previewView,
        onStatusChanged = {},
        onFrameAvailable = {},
        config = config.copy(analysisEnabled = false),
        onEvent = { if (!closed) onEvent(it) }
    )

    val isPreviewActive: Boolean get() = !closed && running && camera.isPreviewActive

    fun start() {
        requireMainThread()
        check(!closed) { "Client is closed" }
        if (running) return
        running = true
        camera.start()
    }

    fun stop() {
        requireMainThread()
        if (closed) return
        running = false
        camera.stop()
    }

    fun selectCamera(cameraId: String) {
        requireMainThread()
        check(!closed) { "Client is closed" }
        camera.selectCamera(cameraId)
    }

    fun retryCamera() {
        requireMainThread()
        check(!closed) { "Client is closed" }
        camera.retryCamera()
    }

    /** Concurrent requests fail; a capture interrupted by stop/select/close never returns success. */
    fun capturePhoto(region: PhotoCropRegion? = null, onResult: (Result<ByteArray>) -> Unit) {
        requireMainThread()
        if (closed) {
            mainHandler.post { onResult(Result.failure(IllegalStateException("Client is closed"))) }
            return
        }
        camera.captureStillPhoto(region) { result ->
            onResult(if (closed) Result.failure(IllegalStateException("Client is closed")) else result)
        }
    }

    override fun close() {
        requireMainThread()
        if (closed) return
        running = false
        closed = true
        camera.stop()
    }

    private fun requireMainThread() {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Call on the main thread" }
    }
}
