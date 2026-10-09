package com.example.rk3566hardwarevalidator.hardware.pose

import java.nio.ByteBuffer

/**
 * 姿态模型的硬件无关接口。
 *
 * 未来可分别提供 RknnPoseEngine（RK3566 NPU）和测试用实现，业务层只依赖此接口。
 */
interface PoseEngine : AutoCloseable {
    fun initialize(): PoseEngineStatus

    fun detect(frame: PoseFrame): PoseResult

    override fun close()
}

data class PoseFrame(
    val buffer: ByteBuffer,
    val width: Int,
    val height: Int,
    val format: PosePixelFormat,
    val rotationDegrees: Int,
    val timestampNanos: Long
)

enum class PosePixelFormat {
    RGB888,
    NV12
}

data class PoseKeypoint(
    val bodyPart: BodyPart,
    val normalizedX: Float,
    val normalizedY: Float,
    val confidence: Float
)

data class PoseResult(
    val keypoints: List<PoseKeypoint>,
    val inferenceTimeMs: Float,
    val personConfidence: Float
)

enum class BodyPart {
    NOSE,
    LEFT_EYE,
    RIGHT_EYE,
    LEFT_EAR,
    RIGHT_EAR,
    LEFT_SHOULDER,
    RIGHT_SHOULDER,
    LEFT_ELBOW,
    RIGHT_ELBOW,
    LEFT_WRIST,
    RIGHT_WRIST,
    LEFT_HIP,
    RIGHT_HIP,
    LEFT_KNEE,
    RIGHT_KNEE,
    LEFT_ANKLE,
    RIGHT_ANKLE
}

sealed interface PoseEngineStatus {
    data object Ready : PoseEngineStatus
    data class Unavailable(val reason: String) : PoseEngineStatus
}
