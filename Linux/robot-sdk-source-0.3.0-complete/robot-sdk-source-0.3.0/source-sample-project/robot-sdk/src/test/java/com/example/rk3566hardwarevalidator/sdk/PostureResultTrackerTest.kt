package com.example.rk3566hardwarevalidator.sdk

import com.example.rk3566hardwarevalidator.hardware.pose.*
import com.example.rk3566hardwarevalidator.hardware.posture.*
import org.junit.Assert.*
import org.junit.Test

class PostureResultTrackerTest {
    private val person = PoseResult(listOf(PoseKeypoint(BodyPart.NOSE, .5F, .3F, .9F)), 12F, .9F)
    private val all = PostureIssue.entries.toSet()
    private fun detection(visible: Set<PostureIssue> = all, ready: Set<PostureIssue> = all, issues: Set<PostureIssue> = emptySet()) =
        PostureDetection(PostureDetectionState.READY, issues, ready, visible, PostureMeasurements())
    private fun update(tracker: PostureResultTracker, time: Long, detection: PostureDetection?, pose: PoseResult = person) =
        tracker.update(pose, detection, 1_000_000 + time, time, 999, 640, 480)

    @Test fun missingIsNotNormal() {
        val tracker = PostureResultTracker()
        assertEquals(PostureResultState.NO_PERSON, update(tracker, 0, null, person.copy(keypoints = emptyList())).state)
        assertEquals(PostureResultState.INSUFFICIENT_KEYPOINTS, update(tracker, 10, detection(visible = emptySet())).state)
        assertEquals(PostureResultState.OBSERVING, update(tracker, 20, detection(ready = emptySet())).state)
        assertEquals(PostureResultState.PARTIAL, update(tracker, 30, detection(visible = setOf(PostureIssue.HEAD_DOWN))).state)
        assertEquals(PostureResultState.NORMAL, update(tracker, 40, detection()).state)
    }

    @Test fun missingCheckDoesNotReuseStaleIssueAndDuration() {
        val tracker = PostureResultTracker()
        val abnormal = detection(issues = setOf(PostureIssue.HEAD_DOWN))
        assertEquals(0L, update(tracker, 100, abnormal).checks[1].continuousAbnormalMs)
        assertEquals(500L, update(tracker, 600, abnormal).checks[1].continuousAbnormalMs)
        val missing = update(tracker, 700, abnormal.copy(currentlyVisibleChecks = all - PostureIssue.HEAD_DOWN))
        assertEquals(PostureResultState.PARTIAL, missing.state)
        assertEquals(PostureCheckState.NOT_VISIBLE, missing.checks[1].state)
        assertEquals(0L, update(tracker, 800, abnormal).checks[1].continuousAbnormalMs)
        assertEquals(0L, update(tracker, 4000, abnormal).checks[1].continuousAbnormalMs)
        assertEquals(0L, update(tracker, 3000, abnormal).checks[1].continuousAbnormalMs)
    }

    @Test fun resetAndFreshnessAreExplicit() {
        val tracker = PostureResultTracker()
        val first = update(tracker, 100, detection())
        assertEquals(SnapshotAvailability.CURRENT, PostureResultTracker.snapshot(first, 2100).availability)
        assertEquals(SnapshotAvailability.EXPIRED, PostureResultTracker.snapshot(first, 2101).availability)
        assertEquals(SnapshotAvailability.EXPIRED, PostureResultTracker.snapshot(first, 0).availability)
        assertEquals(SnapshotAvailability.UNAVAILABLE, PostureResultTracker.snapshot(null, 0).availability)
        tracker.reset()
        assertTrue(update(tracker, 200, detection()).sequence > first.sequence)
    }

    @Test fun jsonContainsOnlyTransportDataAndFiniteNumbers() {
        val result = update(PostureResultTracker(), 0, null, person.copy(personConfidence = Float.NaN, inferenceTimeMs = Float.POSITIVE_INFINITY))
        val json = result.toJson()
        assertTrue(json.contains("\"personScore\":null"))
        assertFalse(json.contains("NaN"))
        assertFalse(json.contains("Infinity"))
        assertTrue(json.contains("CAMERA_UNSPECIFIED"))
        assertFalse(json.contains("student"))
        assertTrue(PostureResultTracker.snapshot(null, 0).toJson().contains("\"result\":null"))
    }
}
