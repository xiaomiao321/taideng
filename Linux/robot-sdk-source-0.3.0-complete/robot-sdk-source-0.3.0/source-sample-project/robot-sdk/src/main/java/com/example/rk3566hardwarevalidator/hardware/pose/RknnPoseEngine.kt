package com.example.rk3566hardwarevalidator.hardware.pose

import android.content.Context
import java.nio.ByteBuffer

/**
 * YOLOv8n Pose implementation backed by RK3566's RKNN NPU runtime.
 *
 * The model is initialized from the APK asset once. Calls are synchronized so lifecycle
 * shutdown cannot release the native RKNN context while an inference is in progress.
 */
class RknnPoseEngine(context: Context) : PoseEngine {
    private val appContext = context.applicationContext
    private var nativeHandle = 0L

    @Synchronized
    override fun initialize(): PoseEngineStatus {
        if (nativeHandle != 0L) return PoseEngineStatus.Ready
        libraryLoadError?.let { error ->
            return PoseEngineStatus.Unavailable("加载 RKNN JNI 失败：${error.message}")
        }

        return try {
            val modelData = appContext.assets.open(MODEL_ASSET_PATH).use { it.readBytes() }
            nativeHandle = nativeCreate(modelData)
            if (nativeHandle == 0L) {
                PoseEngineStatus.Unavailable("RKNN 模型初始化返回空句柄")
            } else {
                PoseEngineStatus.Ready
            }
        } catch (error: Throwable) {
            PoseEngineStatus.Unavailable("RKNN 模型初始化失败：${error.message ?: error.javaClass.simpleName}")
        }
    }

    @Synchronized
    override fun detect(frame: PoseFrame): PoseResult {
        check(nativeHandle != 0L) { "RKNN pose engine is not initialized" }
        require(frame.format == PosePixelFormat.NV12) { "RKNN pose engine currently accepts NV12 frames" }
        require(frame.buffer.isDirect) { "Pose frame must use a direct ByteBuffer" }

        val values = nativeDetect(nativeHandle, frame.buffer, frame.width, frame.height)
        val inferenceTimeMs = values.getOrElse(0) { 0F }
        val personConfidence = values.getOrElse(1) { 0F }
        if (values.size < RESULT_HEADER_SIZE + BodyPart.entries.size * VALUES_PER_KEYPOINT) {
            return PoseResult(emptyList(), inferenceTimeMs, personConfidence)
        }

        val keypoints = BodyPart.entries.mapIndexed { index, bodyPart ->
            val offset = RESULT_HEADER_SIZE + index * VALUES_PER_KEYPOINT
            PoseKeypoint(
                bodyPart = bodyPart,
                normalizedX = values[offset],
                normalizedY = values[offset + 1],
                confidence = values[offset + 2]
            )
        }
        return PoseResult(keypoints, inferenceTimeMs, personConfidence)
    }

    @Synchronized
    override fun close() {
        if (nativeHandle != 0L) {
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
    }

    private external fun nativeCreate(modelData: ByteArray): Long

    private external fun nativeDetect(
        handle: Long,
        nv12Buffer: ByteBuffer,
        width: Int,
        height: Int
    ): FloatArray

    private external fun nativeDestroy(handle: Long)

    private companion object {
        const val MODEL_ASSET_PATH = "models/yolov8n_pose_rk3566_int8.rknn"
        const val RESULT_HEADER_SIZE = 2
        const val VALUES_PER_KEYPOINT = 3

        val libraryLoadError: Throwable? = runCatching {
            System.loadLibrary("pose_native")
        }.exceptionOrNull()
    }
}
