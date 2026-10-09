package com.example.rk3566hardwarevalidator.sdk

import com.example.rk3566hardwarevalidator.hardware.posture.PostureDetection
import com.example.rk3566hardwarevalidator.hardware.posture.PostureIssue
import com.example.rk3566hardwarevalidator.hardware.pose.PoseResult

/** Scores are model scores, NOT calibrated diagnostic probabilities. */
enum class PostureResultState { NO_PERSON, INSUFFICIENT_KEYPOINTS, OBSERVING, PARTIAL, NORMAL, ABNORMAL }
enum class PostureCheckState { NOT_VISIBLE, OBSERVING, NORMAL, ABNORMAL }
enum class SnapshotAvailability { UNAVAILABLE, CURRENT, EXPIRED }

data class PostureCheckResult(
    val issue: PostureIssue,
    val state: PostureCheckState,
    val continuousAbnormalMs: Long
)

/** Transport-neutral payload. App owns identity, persistence, Bluetooth and delivery policy. */
data class PostureBusinessResult(
    val sequence: Long,
    val observedAtUnixMs: Long,
    val observedAtElapsedMs: Long,
    val sourceFrameTimestampNanos: Long,
    val frameWidth: Int,
    val frameHeight: Int,
    val state: PostureResultState,
    val personScore: Float,
    val inferenceTimeMs: Float,
    val checks: List<PostureCheckResult>,
    val detection: PostureDetection?
) {
    /** No camera images/audio, student IDs or clinical confidence are included. */
    fun toJson(): String = buildString {
        append("{\"schemaVersion\":1,\"sdkVersion\":\"${RobotSdkInfo.VERSION}\",")
        append("\"sequence\":$sequence,\"observedAtUnixMs\":$observedAtUnixMs,")
        append("\"observedAtElapsedMs\":$observedAtElapsedMs,")
        append("\"sourceFrameTimestampNanos\":$sourceFrameTimestampNanos,")
        append("\"sourceClock\":\"CAMERA_UNSPECIFIED\",\"frameWidth\":$frameWidth,\"frameHeight\":$frameHeight,")
        append("\"state\":\"${state.name}\",\"personScore\":${jsonNumber(personScore)},")
        append("\"inferenceTimeMs\":${jsonNumber(inferenceTimeMs)},\"checks\":[")
        checks.forEachIndexed { index, check ->
            if (index > 0) append(',')
            append("{\"issue\":\"${check.issue.name}\",\"state\":\"${check.state.name}\",\"continuousAbnormalMs\":${check.continuousAbnormalMs}}")
        }
        append("],\"measurements\":{")
        append("\"shoulderTiltDegrees\":${jsonNumber(detection?.measurements?.shoulderTiltDegrees)},")
        append("\"headClearanceRatio\":${jsonNumber(detection?.measurements?.headClearanceRatio)},")
        append("\"neckShoulderClearanceRatio\":${jsonNumber(detection?.measurements?.neckShoulderClearanceRatio)}}}")
    }
}

data class PostureBusinessSnapshot(
    val availability: SnapshotAvailability,
    val ageMs: Long?,
    val result: PostureBusinessResult?
) {
    fun toJson(): String = "{\"schemaVersion\":1,\"availability\":\"${availability.name}\",\"ageMs\":${ageMs ?: "null"},\"result\":${result?.toJson() ?: "null"}}"
}

private fun jsonNumber(number: Float?): String = number?.takeIf { it.isFinite() }?.toString() ?: "null"

/** Pure business mapper; a frame gap resets durations instead of counting invisible time. */
class PostureResultTracker(private val maximumFrameGapMs: Long = 2_000) {
    private var sequence = 0L
    private var previousElapsedMs: Long? = null
    private val abnormalSince = mutableMapOf<PostureIssue, Long>()

    init { require(maximumFrameGapMs > 0) }

    @Synchronized
    fun reset() { previousElapsedMs = null; abnormalSince.clear() }

    @Synchronized
    fun update(
        pose: PoseResult,
        detection: PostureDetection?,
        observedAtUnixMs: Long,
        observedAtElapsedMs: Long,
        sourceFrameTimestampNanos: Long,
        frameWidth: Int,
        frameHeight: Int
    ): PostureBusinessResult {
        require(observedAtElapsedMs >= 0)
        val previous = previousElapsedMs
        if (previous != null && (observedAtElapsedMs < previous || observedAtElapsedMs - previous > maximumFrameGapMs)) abnormalSince.clear()
        previousElapsedMs = observedAtElapsedMs
        val person = pose.keypoints.isNotEmpty()
        val checks = PostureIssue.entries.map { issue ->
            val state = when {
                !person || detection == null || issue !in detection.currentlyVisibleChecks -> PostureCheckState.NOT_VISIBLE
                issue !in detection.readyChecks -> PostureCheckState.OBSERVING
                issue in detection.issues -> PostureCheckState.ABNORMAL
                else -> PostureCheckState.NORMAL
            }
            val duration = if (state == PostureCheckState.ABNORMAL) {
                observedAtElapsedMs - abnormalSince.getOrPut(issue) { observedAtElapsedMs }
            } else { abnormalSince.remove(issue); 0L }
            PostureCheckResult(issue, state, duration)
        }
        val state = when {
            !person -> PostureResultState.NO_PERSON
            checks.all { it.state == PostureCheckState.NOT_VISIBLE } -> PostureResultState.INSUFFICIENT_KEYPOINTS
            checks.any { it.state == PostureCheckState.ABNORMAL } -> PostureResultState.ABNORMAL
            checks.all { it.state == PostureCheckState.NORMAL } -> PostureResultState.NORMAL
            checks.any { it.state == PostureCheckState.NORMAL } -> PostureResultState.PARTIAL
            else -> PostureResultState.OBSERVING
        }
        return PostureBusinessResult(++sequence, observedAtUnixMs, observedAtElapsedMs,
            sourceFrameTimestampNanos, frameWidth, frameHeight, state, pose.personConfidence,
            pose.inferenceTimeMs, checks, detection)
    }

    companion object {
        fun snapshot(result: PostureBusinessResult?, nowElapsedMs: Long, maxAgeMs: Long = 2_000): PostureBusinessSnapshot {
            require(maxAgeMs >= 0)
            if (result == null) return PostureBusinessSnapshot(SnapshotAvailability.UNAVAILABLE, null, null)
            val age = nowElapsedMs - result.observedAtElapsedMs
            return PostureBusinessSnapshot(if (age in 0..maxAgeMs) SnapshotAvailability.CURRENT else SnapshotAvailability.EXPIRED,
                age.coerceAtLeast(0), result)
        }
    }
}

fun interface SdkSubscription : AutoCloseable { override fun close() }

object RobotSdkInfo {
    const val VERSION = "0.3.0"
    const val CONTRACT_SCHEMA_VERSION = 1
    const val TARGET_PLATFORM = "RK3566 / Android 11 / arm64-v8a"
    /** Capabilities still depend on granted permissions, Camera HAL and connected MCU firmware. */
    val localCapabilities: Set<String> = setOf("POSTURE", "CAMERA_ROLE_BINDING", "QUESTION_CAMERA", "USB_ASR", "MCU_V2", "TRANSPORT_NEUTRAL_DATA")
}
