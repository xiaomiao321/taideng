package com.example.rk3566hardwarevalidator.hardware.posture

/** Monotonic time is supplied by the caller so this policy is testable without Android. */
class PostureAlertPolicy(
    private val sustainedMillis: Long = 3_000L,
    private val cooldownMillis: Long = 30_000L
) {
    private var abnormalSince: Long? = null
    private var lastAlertAt: Long? = null

    fun shouldAlert(detection: PostureDetection?, now: Long, enabled: Boolean): Boolean {
        val visibleIssues = detection?.issues.orEmpty().intersect(detection?.currentlyVisibleChecks.orEmpty())
        if (!enabled || detection?.state != PostureDetectionState.READY || visibleIssues.isEmpty()) {
            resetObservation()
            return false
        }
        val since = abnormalSince ?: now.also { abnormalSince = it }
        if (now - since < sustainedMillis) return false
        if (lastAlertAt?.let { now - it < cooldownMillis } == true) return false
        lastAlertAt = now
        return true
    }

    fun resetObservation() { abnormalSince = null }
}
