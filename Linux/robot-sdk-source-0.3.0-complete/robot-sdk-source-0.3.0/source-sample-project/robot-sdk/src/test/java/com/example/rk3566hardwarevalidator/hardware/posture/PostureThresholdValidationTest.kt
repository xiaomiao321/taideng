package com.example.rk3566hardwarevalidator.hardware.posture

import org.junit.Test

class PostureThresholdValidationTest {
    @Test(expected = IllegalArgumentException::class)
    fun rejectsNanConfidence() { PostureDetector(PostureThresholds(minimumKeypointConfidence = Float.NaN)) }
    @Test(expected = IllegalArgumentException::class)
    fun rejectsImpossibleAngle() { PostureDetector(PostureThresholds(unevenShoulderAngleDegrees = 100F)) }
    @Test(expected = IllegalArgumentException::class)
    fun rejectsInconsistentHistory() { PostureDetector(PostureThresholds(historySize = 2, minimumStableSamples = 3)) }
    @Test(expected = IllegalArgumentException::class)
    fun rejectsInfinityClearance() { PostureDetector(PostureThresholds(headDownClearanceRatio = Float.POSITIVE_INFINITY)) }
}
