package com.example.rk3566hardwarevalidator.hardware.posture

import com.example.rk3566hardwarevalidator.hardware.pose.BodyPart
import com.example.rk3566hardwarevalidator.hardware.pose.PoseKeypoint
import com.example.rk3566hardwarevalidator.hardware.pose.PoseResult
import java.util.ArrayDeque
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot

/** Posture checks that can be estimated from a front-facing COCO-17 skeleton. */
enum class PostureIssue {
    UNEVEN_SHOULDERS,
    HEAD_DOWN,
    SLOUCHING
}

enum class PostureDetectionState {
    NEEDS_POSITION,
    OBSERVING,
    READY
}

data class PostureMeasurements(
    val shoulderTiltDegrees: Float? = null,
    val headClearanceRatio: Float? = null,
    val neckShoulderClearanceRatio: Float? = null
)

data class PostureDetection(
    val state: PostureDetectionState,
    val issues: Set<PostureIssue>,
    val readyChecks: Set<PostureIssue>,
    val currentlyVisibleChecks: Set<PostureIssue>,
    val measurements: PostureMeasurements
)

data class PostureThresholds(
    val minimumKeypointConfidence: Float = 0.35F,
    val minimumShoulderWidthFraction: Float = 0.06F,
    val unevenShoulderAngleDegrees: Float = 8F,
    val headDownClearanceRatio: Float = 0.45F,
    val slouchingNeckShoulderClearanceRatio: Float = 0.32F,
    val historySize: Int = 12,
    val minimumStableSamples: Int = 6,
    val issueVoteRatio: Float = 0.60F,
    val maximumMissingFrames: Int = 6
)

/**
 * Converts pose keypoints into stable posture warnings.
 *
 * Measurements use pixel coordinates so camera aspect ratio does not distort angles. All checks
 * only require upper-body keypoints because the target camera sits on the desk and cannot see the
 * hips. Each check keeps its own history so a temporarily hidden face does not invalidate the
 * visible shoulder checks.
 */
class PostureDetector(
    private val thresholds: PostureThresholds = PostureThresholds()
) {
    private val histories = PostureIssue.entries.associateWith { ArrayDeque<Boolean>() }
    private val missingFrameCounts = PostureIssue.entries.associateWith { 0 }.toMutableMap()

    init {
        require(thresholds.minimumKeypointConfidence.isFinite() && thresholds.minimumKeypointConfidence in 0F..1F)
        require(thresholds.minimumShoulderWidthFraction.isFinite() && thresholds.minimumShoulderWidthFraction > 0F && thresholds.minimumShoulderWidthFraction <= 1F)
        require(thresholds.unevenShoulderAngleDegrees.isFinite() && thresholds.unevenShoulderAngleDegrees in 0F..90F)
        require(thresholds.headDownClearanceRatio.isFinite() && thresholds.slouchingNeckShoulderClearanceRatio.isFinite())
        require(thresholds.historySize > 0)
        require(thresholds.minimumStableSamples in 1..thresholds.historySize)
        require(thresholds.issueVoteRatio in 0F..1F)
        require(thresholds.maximumMissingFrames > 0)
    }

    @Synchronized
    fun update(poseResult: PoseResult, frameWidth: Int, frameHeight: Int): PostureDetection {
        val assessment = assess(poseResult, frameWidth, frameHeight)
        PostureIssue.entries.forEach { issue ->
            val detected = assessment.observations[issue]
            if (detected != null) {
                missingFrameCounts[issue] = 0
                histories.getValue(issue).apply {
                    addLast(detected)
                    while (size > thresholds.historySize) removeFirst()
                }
            } else {
                val missingFrames = missingFrameCounts.getValue(issue) + 1
                missingFrameCounts[issue] = missingFrames
                if (missingFrames >= thresholds.maximumMissingFrames) {
                    histories.getValue(issue).clear()
                }
            }
        }

        val readyChecks = histories
            .filterValues { history -> history.size >= thresholds.minimumStableSamples }
            .keys
        val stableIssues = readyChecks.filterTo(linkedSetOf()) { issue ->
            val history = histories.getValue(issue)
            history.count { detected -> detected }.toFloat() / history.size >=
                thresholds.issueVoteRatio
        }
        val state = when {
            assessment.observations.isEmpty() -> PostureDetectionState.NEEDS_POSITION
            readyChecks.isEmpty() -> PostureDetectionState.OBSERVING
            else -> PostureDetectionState.READY
        }

        return PostureDetection(
            state = state,
            issues = stableIssues,
            readyChecks = readyChecks,
            currentlyVisibleChecks = assessment.observations.keys,
            measurements = assessment.measurements
        )
    }

    @Synchronized
    fun reset() {
        histories.values.forEach(ArrayDeque<Boolean>::clear)
        missingFrameCounts.keys.forEach { issue -> missingFrameCounts[issue] = 0 }
    }

    private fun assess(
        poseResult: PoseResult,
        frameWidth: Int,
        frameHeight: Int
    ): FrameAssessment {
        if (frameWidth <= 0 || frameHeight <= 0 || poseResult.keypoints.isEmpty()) {
            return FrameAssessment()
        }

        val points = poseResult.keypoints.associate { keypoint ->
            keypoint.bodyPart to keypoint.toPixelPoint(frameWidth, frameHeight)
        }
        val leftShoulder = points.usable(BodyPart.LEFT_SHOULDER) ?: return FrameAssessment()
        val rightShoulder = points.usable(BodyPart.RIGHT_SHOULDER) ?: return FrameAssessment()
        val shoulderWidth = leftShoulder.distanceTo(rightShoulder)
        val minimumShoulderWidth = minOf(frameWidth, frameHeight) *
            thresholds.minimumShoulderWidthFraction
        if (shoulderWidth < minimumShoulderWidth) return FrameAssessment()

        val observations = linkedMapOf<PostureIssue, Boolean>()
        val shoulderTiltDegrees = Math.toDegrees(
            atan2(
                abs(leftShoulder.y - rightShoulder.y).toDouble(),
                abs(leftShoulder.x - rightShoulder.x).toDouble()
            )
        ).toFloat()
        observations[PostureIssue.UNEVEN_SHOULDERS] =
            shoulderTiltDegrees >= thresholds.unevenShoulderAngleDegrees

        val shoulderMidpoint = leftShoulder.midpoint(rightShoulder)
        val facePoint = points.usable(BodyPart.NOSE)
            ?: midpointIfUsable(points, BodyPart.LEFT_EYE, BodyPart.RIGHT_EYE)
        val headClearanceRatio = facePoint?.let { face ->
            (shoulderMidpoint.y - face.y) / shoulderWidth
        }
        if (headClearanceRatio != null) {
            observations[PostureIssue.HEAD_DOWN] =
                headClearanceRatio < thresholds.headDownClearanceRatio
        }

        val earMidpoint = midpointIfUsable(points, BodyPart.LEFT_EAR, BodyPart.RIGHT_EAR)
        val neckShoulderClearanceRatio = earMidpoint?.let { ears ->
            (shoulderMidpoint.y - ears.y) / shoulderWidth
        }
        if (neckShoulderClearanceRatio != null) {
            observations[PostureIssue.SLOUCHING] =
                neckShoulderClearanceRatio < thresholds.slouchingNeckShoulderClearanceRatio
        }

        return FrameAssessment(
            observations = observations,
            measurements = PostureMeasurements(
                shoulderTiltDegrees = shoulderTiltDegrees,
                headClearanceRatio = headClearanceRatio,
                neckShoulderClearanceRatio = neckShoulderClearanceRatio
            )
        )
    }

    private fun Map<BodyPart, PixelPoint>.usable(bodyPart: BodyPart): PixelPoint? =
        get(bodyPart)?.takeIf { point -> point.confidence >= thresholds.minimumKeypointConfidence }

    private fun midpointIfUsable(
        points: Map<BodyPart, PixelPoint>,
        first: BodyPart,
        second: BodyPart
    ): PixelPoint? {
        val firstPoint = points.usable(first) ?: return null
        val secondPoint = points.usable(second) ?: return null
        return firstPoint.midpoint(secondPoint)
    }

    private fun PoseKeypoint.toPixelPoint(frameWidth: Int, frameHeight: Int) = PixelPoint(
        x = normalizedX * frameWidth,
        y = normalizedY * frameHeight,
        confidence = confidence
    )

    private data class FrameAssessment(
        val observations: Map<PostureIssue, Boolean> = emptyMap(),
        val measurements: PostureMeasurements = PostureMeasurements()
    )

    private data class PixelPoint(
        val x: Float,
        val y: Float,
        val confidence: Float
    ) {
        fun distanceTo(other: PixelPoint): Float = hypot(x - other.x, y - other.y)

        fun midpoint(other: PixelPoint) = PixelPoint(
            x = (x + other.x) / 2F,
            y = (y + other.y) / 2F,
            confidence = minOf(confidence, other.confidence)
        )
    }
}
