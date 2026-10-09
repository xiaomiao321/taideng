package com.example.rk3566hardwarevalidator.hardware.camera

/**
 * Uses camera timestamps to cap expensive analysis work without delaying preview delivery.
 */
internal class FrameRateLimiter(maxFramesPerSecond: Int) {
    private val minimumIntervalNanos: Long
    private var lastAcceptedTimestampNanos: Long? = null

    init {
        require(maxFramesPerSecond > 0) { "maxFramesPerSecond must be positive" }
        minimumIntervalNanos = NANOS_PER_SECOND / maxFramesPerSecond
    }

    @Synchronized
    fun shouldProcess(timestampNanos: Long): Boolean {
        val previous = lastAcceptedTimestampNanos
        val timestampReset = previous != null && timestampNanos < previous
        if (previous == null || timestampReset || timestampNanos - previous >= minimumIntervalNanos) {
            lastAcceptedTimestampNanos = timestampNanos
            return true
        }
        return false
    }

    @Synchronized
    fun reset() {
        lastAcceptedTimestampNanos = null
    }

    private companion object {
        const val NANOS_PER_SECOND = 1_000_000_000L
    }
}
