package com.example.rk3566hardwarevalidator.sdk

/** cameraId is a Camera2 ID for this boot/session, never a persistent physical identity. */
data class CameraSessionConfig(
    val cameraId: String? = null,
    val analysisEnabled: Boolean = true,
    val fps: Int = 8,
    val recoverOnReconnect: Boolean = false
) {
    init {
        require(cameraId == null || cameraId.isNotBlank()) { "cameraId must not be blank" }
        require(fps in 1..60) { "fps must be between 1 and 60" }
    }
}

/** Delivered on the main thread. Missing/disconnected cameras never switch to another ID. */
sealed interface CameraEvent {
    data class Opening(val cameraId: String) : CameraEvent
    /** The session is configured and a new preview texture frame has actually arrived. */
    data class PreviewStarted(val cameraId: String) : CameraEvent
    data class Missing(val cameraId: String) : CameraEvent
    data class Busy(val cameraId: String) : CameraEvent
    data class Disconnected(val cameraId: String) : CameraEvent
    data class Available(val cameraId: String) : CameraEvent
    data class Error(val cameraId: String?, val message: String) : CameraEvent
    data object Stopped : CameraEvent
}
