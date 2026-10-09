package com.example.rk3566hardwarevalidator.hardware.mcu

enum class McuCommandType { LIGHT, MOTION, LIGHT_MODE, ARM_POSITION, ARM_HOME, LIGHT_ZONE, BUZZER, INDICATOR, SOFT_SHUTDOWN }

/** ACK_RECEIVED is acceptance only. APPLIED requires a fresh solicited physical-state report. */
enum class McuCommandOutcome {
    ACK_RECEIVED, APPLIED,
    @Deprecated("ACK is acceptance only; use ACK_RECEIVED") ACKNOWLEDGED,
    REJECTED, TIMEOUT, STATE_TIMEOUT, STALE_STATUS, NOT_CONNECTED, WRITE_ERROR, DISCONNECTED,
    CLOSED, INVALID_ARGUMENT, UNSUPPORTED, SUPERSEDED, BUSY
}

data class McuCommandSubmission(
    val commandId: Long, val sequence: Int, val command: McuCommandType,
    val accepted: Boolean, val outcome: McuCommandOutcome? = null
)

data class McuStatusQuerySubmission(val sequence: Int, val command: McuCommandType, val selector: Int,
    val accepted: Boolean, val outcome: McuCommandOutcome? = null)

/** The current wire protocol does not report physical HOME input or calibration state. */
enum class McuPhysicalHomeInput { UNKNOWN }
enum class McuCalibrationState { UNKNOWN }

data class McuJointSnapshot(val jointId: Int,
    val actualPositionMilliDegrees: Int? = null, val targetPositionMilliDegrees: Int? = null,
    val actualSpeedMilliDegreesPerSecond: Int? = null, val targetSpeedMilliDegreesPerSecond: Int? = null,
    val faultFlags: Int? = null, val receivedAtElapsedMs: Long? = null, val stale: Boolean = true,
    val physicalHomeInput: McuPhysicalHomeInput = McuPhysicalHomeInput.UNKNOWN,
    val calibrationState: McuCalibrationState = McuCalibrationState.UNKNOWN)

data class McuCommandResult(
    val sequence: Int, val command: McuCommandType, val outcome: McuCommandOutcome,
    val resultCode: Int? = null, val detail: Int? = null,
    val commandId: Long = 0, val receivedAtElapsedMs: Long = 0
)

data class McuJointCapability(
    val jointId: Int, val minPositionMilliDegrees: Int, val maxPositionMilliDegrees: Int,
    val maxSpeedMilliDegreesPerSecond: Int,
    val homePositionMilliDegrees: Int? = null
) {
    init {
        require(jointId in 0..255)
        require(minPositionMilliDegrees <= maxPositionMilliDegrees)
        require(maxSpeedMilliDegreesPerSecond > 0)
        require(homePositionMilliDegrees == null || homePositionMilliDegrees in minPositionMilliDegrees..maxPositionMilliDegrees)
    }
}

/** Extensions are enabled only by an explicitly supplied, firmware-validated descriptor. No probing. */
data class McuCapabilities(
    val protocolVersion: Int = McuProtocol.VERSION,
    val lightModes: Set<Int> = emptySet(),
    val joints: List<McuJointCapability> = emptyList(),
    val firmwareDescriptorId: String? = null,
    val lightZoneIds: Set<Int> = emptySet(),
    val buzzerPatternIds: Set<Int> = emptySet(),
    val indicatorIds: Set<Int> = emptySet(),
    val supportsSoftShutdown: Boolean = false
) {
    init {
        require(protocolVersion == McuProtocol.VERSION)
        require(lightModes.all { it in 0..255 })
        require((lightZoneIds + buzzerPatternIds + indicatorIds).all { it in 0..255 })
        require(joints.map { it.jointId }.distinct().size == joints.size)
        require((lightModes.isEmpty() && joints.isEmpty() && lightZoneIds.isEmpty() && buzzerPatternIds.isEmpty() &&
            indicatorIds.isEmpty() && !supportsSoftShutdown) || !firmwareDescriptorId.isNullOrBlank())
    }
    val supportsBasicLight: Boolean get() = true
    val supportsBasicMotion: Boolean get() = true
    companion object { val LEGACY_V2 = McuCapabilities() }
}

data class McuSafetyOptions(
    val ackTimeoutMs: Long = 500, val maxRetries: Int = 2,
    val stateTimeoutMs: Long = 3_000, val statusStaleAfterMs: Long = 5_000,
    /** Optional watchdog emits only STOP. Null disables the host watchdog. Firmware needs its own failsafe. */
    val motionTimeoutMs: Long? = null
) {
    init {
        require(ackTimeoutMs > 0 && maxRetries >= 0 && stateTimeoutMs > 0 && statusStaleAfterMs > 0)
        require(motionTimeoutMs == null || motionTimeoutMs > 0)
    }
}

/** Pure synchronized state machine; transport must call canSend immediately before every write. */
class McuCommandTracker(
    val capabilities: McuCapabilities = McuCapabilities.LEGACY_V2,
    val options: McuSafetyOptions = McuSafetyOptions(),
    private val clock: () -> Long,
    private val onResult: (McuCommandResult) -> Unit = {}
) {
    data class Dispatch(val submission: McuCommandSubmission, val frame: McuFrame, val motionEpoch: Long)
    private data class Pending(
        val dispatch: Dispatch, val createdAt: Long, var sentAt: Long? = null, var retries: Int = 0,
        var ackAt: Long? = null, var lastConfirmationRequestAt: Long? = null
    )
    data class ExtendedStatusQuery(val submission: McuStatusQuerySubmission, val frame: McuFrame?)
    private data class Probe(val requestedAt: Long, val commandIds: List<Long>, val extension: Boolean,
        val controlledCommand: Int? = null, val selector: Int? = null)
    private var nextId = 0L
    private var sequence = 0
    private var motionEpoch = 0L
    private var connected = false
    private var closed = false
    private var motionDeadline: Long? = null
    private val pending = linkedMapOf<Long, Pending>()
    private val probes = linkedMapOf<Int, Probe>()
    // The wire has only 8 sequence bits. Avoid reusing recently completed identities.
    private val retiredSequences = linkedMapOf<Int, Long>()
    private val extendedSnapshots = linkedMapOf<Pair<Int, Int>, McuExtendedStatus>()
    @Volatile var lastStatus: McuStatus? = null
        private set

    @Synchronized fun setConnected(value: Boolean) {
        if (!value) {
            motionEpoch++
            motionDeadline = null
            pending.values.toList().forEach { finish(it, McuCommandOutcome.DISCONNECTED) }
            probes.keys.forEach(::retire)
            probes.clear()
            lastStatus = null
            extendedSnapshots.clear()
        }
        connected = value && !closed
    }

    @Synchronized fun submitLight(enabled: Boolean, brightness: Int): Dispatch = submit(
        McuCommandType.LIGHT, if (brightness !in 0..100) McuCommandOutcome.INVALID_ARGUMENT else null
    ) { McuProtocol.setLight(it, enabled, brightness) }

    @Synchronized fun submitMotion(motion: McuMotion, speed: Int = 100): Dispatch {
        if (motion == McuMotion.STOP) {
            motionEpoch++
            motionDeadline = null
            pending.values.filter { it.dispatch.submission.command in setOf(
                McuCommandType.MOTION, McuCommandType.ARM_POSITION, McuCommandType.ARM_HOME) }.toList()
                .forEach { finish(it, McuCommandOutcome.SUPERSEDED) }
        }
        return submit(McuCommandType.MOTION,
            if (motion == McuMotion.UNKNOWN || speed !in 0..100) McuCommandOutcome.INVALID_ARGUMENT
            else if (motion != McuMotion.STOP) actuationStateError() else null
        , emergencyStop = motion == McuMotion.STOP) { McuProtocol.setMotion(it, motion, speed) }
    }

    @Synchronized fun submitLightMode(modeId: Int, brightness: Int): Dispatch = submit(
        McuCommandType.LIGHT_MODE, when {
            modeId !in 0..255 || brightness !in 0..100 -> McuCommandOutcome.INVALID_ARGUMENT
            modeId !in capabilities.lightModes -> McuCommandOutcome.UNSUPPORTED
            else -> null
        }
    ) { McuProtocol.setLightMode(it, modeId, brightness) }

    @Synchronized fun submitArmPosition(jointId: Int, position: Int, speed: Int): Dispatch {
        val joint = capabilities.joints.firstOrNull { it.jointId == jointId }
        val error = when {
            jointId !in 0..255 || speed <= 0 -> McuCommandOutcome.INVALID_ARGUMENT
            joint == null -> McuCommandOutcome.UNSUPPORTED
            position !in joint.minPositionMilliDegrees..joint.maxPositionMilliDegrees ||
                speed > joint.maxSpeedMilliDegreesPerSecond -> McuCommandOutcome.INVALID_ARGUMENT
            else -> actuationStateError()
        }
        return submit(McuCommandType.ARM_POSITION, error, jointId) {
            McuProtocol.setArmPosition(it, jointId, position, speed)
        }
    }

    @Synchronized fun submitArmHome(jointId: Int): Dispatch {
        val joint = capabilities.joints.firstOrNull { it.jointId == jointId }
        val error = when { jointId !in 0..255 -> McuCommandOutcome.INVALID_ARGUMENT
            joint?.homePositionMilliDegrees == null -> McuCommandOutcome.UNSUPPORTED; else -> actuationStateError() }
        return submit(McuCommandType.ARM_HOME, error, jointId) { McuProtocol.homeArm(it, jointId) }
    }

    @Synchronized fun submitLightZone(zoneId: Int, enabled: Boolean, brightness: Int): Dispatch =
        submit(McuCommandType.LIGHT_ZONE, gate(zoneId, capabilities.lightZoneIds, brightness in 0..100), zoneId) {
            McuProtocol.setLightZone(it, zoneId, enabled, brightness)
        }

    @Synchronized fun submitBuzzer(patternId: Int, volumePercent: Int): Dispatch =
        submit(McuCommandType.BUZZER, gate(patternId, capabilities.buzzerPatternIds, volumePercent in 0..100)) {
            McuProtocol.setBuzzer(it, patternId, volumePercent)
        }

    @Synchronized fun submitIndicator(indicatorId: Int, enabled: Boolean): Dispatch =
        submit(McuCommandType.INDICATOR, gate(indicatorId, capabilities.indicatorIds), indicatorId) {
            McuProtocol.setIndicator(it, indicatorId, enabled)
        }

    @Synchronized fun submitSoftShutdown(): Dispatch {
        if (!capabilities.supportsSoftShutdown) return submit(McuCommandType.SOFT_SHUTDOWN, McuCommandOutcome.UNSUPPORTED) { McuProtocol.softShutdown(it) }
        actuationStateError()?.let { error -> return submit(McuCommandType.SOFT_SHUTDOWN, error) { McuProtocol.softShutdown(it) } }
        motionEpoch++; motionDeadline = null
        pending.values.filter { it.dispatch.submission.command in setOf(McuCommandType.MOTION, McuCommandType.ARM_POSITION, McuCommandType.ARM_HOME) }
            .toList().forEach { finish(it, McuCommandOutcome.SUPERSEDED) }
        return submit(McuCommandType.SOFT_SHUTDOWN, null) { McuProtocol.softShutdown(it) }
    }

    private fun gate(id: Int, supported: Set<Int>, valid: Boolean = true): McuCommandOutcome? = when {
        id !in 0..255 || !valid -> McuCommandOutcome.INVALID_ARGUMENT
        id !in supported -> McuCommandOutcome.UNSUPPORTED
        else -> null
    }

    private fun actuationStateError(): McuCommandOutcome? = when {
        closed -> McuCommandOutcome.CLOSED
        !connected -> McuCommandOutcome.NOT_CONNECTED
        isStatusStale() -> McuCommandOutcome.STALE_STATUS
        lastStatus?.faultFlags != 0 -> McuCommandOutcome.REJECTED
        else -> null
    }

    private fun actuatorKey(d: Dispatch): String = when (d.submission.command) {
        McuCommandType.LIGHT, McuCommandType.LIGHT_MODE -> "light"
        McuCommandType.ARM_POSITION, McuCommandType.ARM_HOME -> "arm:${McuProtocol.unsigned(d.frame.payload[0])}"
        McuCommandType.LIGHT_ZONE -> "zone:${McuProtocol.unsigned(d.frame.payload[0])}"
        McuCommandType.INDICATOR -> "indicator:${McuProtocol.unsigned(d.frame.payload[0])}"
        else -> d.submission.command.name
    }

    private fun submit(type: McuCommandType, invalid: McuCommandOutcome?, jointId: Int? = null, emergencyStop: Boolean = false,
                       frame: (Int) -> McuFrame): Dispatch {
        val seq = if (emergencyStop) 0 else nextSequence()
        val error = when { closed -> McuCommandOutcome.CLOSED; invalid != null -> invalid
            !connected -> McuCommandOutcome.NOT_CONNECTED; seq < 0 -> McuCommandOutcome.BUSY; else -> null }
        val submission = McuCommandSubmission(++nextId, seq, type, error == null, error)
        val dispatch = Dispatch(submission, if (error == null) frame(seq) else McuFrame(sequence = seq.coerceAtLeast(0), command = 0), motionEpoch)
        if (error != null) emit(dispatch, error) else {
            pending.values.filter { actuatorKey(it.dispatch) == actuatorKey(dispatch) }
                .toList().forEach { finish(it, McuCommandOutcome.SUPERSEDED) }
            if (type == McuCommandType.MOTION) motionDeadline = null
            pending[submission.commandId] = Pending(dispatch, clock())
        }
        return dispatch
    }

    @Synchronized fun canSend(dispatch: Dispatch): Boolean {
        val p = pending[dispatch.submission.commandId] ?: return false
        if (closed || !connected || dispatch.submission.command == McuCommandType.MOTION && dispatch.motionEpoch != motionEpoch) return false
        val stop = dispatch.submission.command == McuCommandType.MOTION &&
            McuProtocol.unsigned(dispatch.frame.payload[0]) == McuMotion.STOP.code
        if (p.sentAt == null && !stop && clock() - p.createdAt >= options.stateTimeoutMs) {
            finish(p, McuCommandOutcome.TIMEOUT)
            return false
        }
        val hazardous = dispatch.submission.command in setOf(McuCommandType.ARM_POSITION, McuCommandType.ARM_HOME,
            McuCommandType.SOFT_SHUTDOWN) || dispatch.submission.command == McuCommandType.MOTION && !stop
        val error = if (hazardous) actuationStateError() else null
        if (error != null) { finish(p, error); return false }
        return p.ackAt == null
    }

    @Synchronized fun markSent(dispatch: Dispatch) {
        val p = pending[dispatch.submission.commandId] ?: return
        if (!canSend(dispatch)) return
        val now = clock()
        if (p.sentAt != null) p.retries++
        p.sentAt = now
        if (dispatch.submission.command == McuCommandType.MOTION &&
            McuProtocol.unsigned(dispatch.frame.payload[0]) != McuMotion.STOP.code && motionDeadline == null) {
            motionDeadline = options.motionTimeoutMs?.let { now + it }
        }
    }

    @Synchronized fun writeFailed(dispatch: Dispatch) {
        pending[dispatch.submission.commandId]?.let { finish(it, McuCommandOutcome.WRITE_ERROR) }
    }

    @Synchronized fun onAck(frame: McuFrame): Boolean {
        if (closed || !connected || frame.version != McuProtocol.VERSION || frame.flags and McuProtocol.FLAG_RESPONSE == 0) return false
        val ack = McuAck.from(frame) ?: return false
        val p = pending.values.firstOrNull { it.dispatch.submission.sequence == frame.sequence &&
            it.dispatch.frame.command == ack.acknowledgedCommand && it.sentAt != null } ?: return false
        if (p.ackAt != null) return false
        if (!ack.isSuccess) finish(p, McuCommandOutcome.REJECTED, ack.resultCode, ack.detail) else {
            p.ackAt = clock()
            emit(p.dispatch, McuCommandOutcome.ACK_RECEIVED, ack.resultCode, ack.detail)
        }
        return ack.isSuccess
    }

    /** Queries carry their own sequence; only replies to probes made after ACK can establish APPLIED. */
    @Synchronized fun statusRequest(extensionFor: Dispatch? = null): McuFrame? {
        if (!connected || closed) return null
        val seq = nextSequence()
        if (seq < 0) return null
        val ids = pending.values.filter { it.ackAt != null &&
            if (extensionFor == null) it.dispatch.submission.command in setOf(McuCommandType.LIGHT, McuCommandType.MOTION)
            else it.dispatch == extensionFor }.map { it.dispatch.submission.commandId }
        probes[seq] = Probe(clock(), ids, extensionFor != null, extensionFor?.frame?.command,
            extensionFor?.frame?.payload?.firstOrNull()?.let(McuProtocol::unsigned))
        ids.forEach { pending[it]?.lastConfirmationRequestAt = clock() }
        return if (extensionFor == null) McuProtocol.getStatus(seq)
        else McuProtocol.getExtendedStatus(seq, extensionFor.frame.command, McuProtocol.unsigned(extensionFor.frame.payload[0]))
    }

    @Synchronized fun awaitingExtensionConfirmation(): List<Dispatch> = pending.values.filter {
        it.ackAt != null && (it.lastConfirmationRequestAt == null || clock() - it.lastConfirmationRequestAt!! >= options.ackTimeoutMs) && it.dispatch.submission.command !in
            setOf(McuCommandType.LIGHT, McuCommandType.MOTION, McuCommandType.SOFT_SHUTDOWN)
    }.map { it.dispatch }

    /** Pure read query: requires capability and connection, but never commands an actuator. */
    @Synchronized fun prepareExtendedStatusQuery(type: McuCommandType, selector: Int): ExtendedStatusQuery {
        val command = when (type) {
            McuCommandType.LIGHT_MODE -> McuProtocol.COMMAND_SET_LIGHT_MODE
            McuCommandType.ARM_POSITION -> McuProtocol.COMMAND_SET_ARM_POSITION
            McuCommandType.ARM_HOME -> McuProtocol.COMMAND_HOME_ARM
            McuCommandType.LIGHT_ZONE -> McuProtocol.COMMAND_SET_LIGHT_ZONE
            McuCommandType.BUZZER -> McuProtocol.COMMAND_SET_BUZZER
            McuCommandType.INDICATOR -> McuProtocol.COMMAND_SET_INDICATOR
            else -> null
        }
        val supported = when (type) {
            McuCommandType.LIGHT_MODE -> selector in capabilities.lightModes
            McuCommandType.ARM_POSITION -> capabilities.joints.any { it.jointId == selector }
            McuCommandType.ARM_HOME -> capabilities.joints.any { it.jointId == selector && it.homePositionMilliDegrees != null }
            McuCommandType.LIGHT_ZONE -> selector in capabilities.lightZoneIds
            McuCommandType.BUZZER -> selector in capabilities.buzzerPatternIds
            McuCommandType.INDICATOR -> selector in capabilities.indicatorIds
            else -> false
        }
        val invalid = when { closed -> McuCommandOutcome.CLOSED
            selector !in 0..255 -> McuCommandOutcome.INVALID_ARGUMENT
            !supported || command == null -> McuCommandOutcome.UNSUPPORTED
            !connected -> McuCommandOutcome.NOT_CONNECTED
            else -> null }
        if (invalid != null) return ExtendedStatusQuery(McuStatusQuerySubmission(-1, type, selector, false, invalid), null)
        val seq = nextSequence()
        if (seq < 0) return ExtendedStatusQuery(McuStatusQuerySubmission(-1, type, selector, false, McuCommandOutcome.BUSY), null)
        val ids = pending.values.filter { it.ackAt != null && it.dispatch.frame.command == command &&
            McuProtocol.unsigned(it.dispatch.frame.payload[0]) == selector }.map { it.dispatch.submission.commandId }
        probes[seq] = Probe(clock(), ids, true, command, selector)
        ids.forEach { pending[it]?.lastConfirmationRequestAt = clock() }
        return ExtendedStatusQuery(McuStatusQuerySubmission(seq, type, selector, true),
            McuProtocol.getExtendedStatus(seq, command!!, selector))
    }

    @Synchronized fun extendedStatusSnapshot(type: McuCommandType, selector: Int): McuExtendedStatus? {
        val command = when (type) {
            McuCommandType.LIGHT_MODE -> McuProtocol.COMMAND_SET_LIGHT_MODE
            McuCommandType.ARM_POSITION -> McuProtocol.COMMAND_SET_ARM_POSITION
            McuCommandType.ARM_HOME -> McuProtocol.COMMAND_HOME_ARM
            McuCommandType.LIGHT_ZONE -> McuProtocol.COMMAND_SET_LIGHT_ZONE
            McuCommandType.BUZZER -> McuProtocol.COMMAND_SET_BUZZER
            McuCommandType.INDICATOR -> McuProtocol.COMMAND_SET_INDICATOR
            else -> return null
        }
        return extendedSnapshots[command to selector]?.let { it.copy(stale = it.stale || !connected || closed ||
            clock() - it.receivedAtElapsedMs > options.statusStaleAfterMs) }
    }

    @Synchronized fun jointSnapshots(): List<McuJointSnapshot> = capabilities.joints.map { joint ->
        val status = listOfNotNull(extendedStatusSnapshot(McuCommandType.ARM_POSITION, joint.jointId),
            extendedStatusSnapshot(McuCommandType.ARM_HOME, joint.jointId)).maxByOrNull { it.receivedAtElapsedMs }
        if (status == null) McuJointSnapshot(joint.jointId) else McuJointSnapshot(joint.jointId,
            status.actualValue, status.targetValue, status.actualSpeed, status.targetSpeed,
            status.faultFlags, status.receivedAtElapsedMs, status.stale)
    }

    @Synchronized fun onStatus(frame: McuFrame): McuStatus? {
        if (!connected || closed || frame.version != McuProtocol.VERSION || frame.flags and McuProtocol.FLAG_RESPONSE == 0) return null
        val status = McuStatus.from(frame) ?: return null
        val now = clock()
        val probe = probes.remove(frame.sequence)
        if (probe != null) retire(frame.sequence)
        val fresh = probe != null && !probe.extension && now - probe.requestedAt <= options.statusStaleAfterMs
        val correlated = if (fresh) probe!!.commandIds else emptyList()
        val annotated = status.copy(receivedAtElapsedMs = now, stale = !fresh, relatedCommandIds = correlated)
        lastStatus = annotated
        correlated.forEach { id -> pending[id]?.let { p ->
            if (p.ackAt != null && probe!!.requestedAt >= p.ackAt!! && matches(p.dispatch.frame, status)) {
                finish(p, McuCommandOutcome.APPLIED)
            }
        } }
        if (probe != null && !fresh) probe.commandIds.forEach { pending[it]?.let { p -> finish(p, McuCommandOutcome.STALE_STATUS) } }
        return annotated
    }

    @Synchronized fun onExtendedStatus(frame: McuFrame): McuExtendedStatus? {
        if (!connected || closed || frame.version != McuProtocol.VERSION || frame.flags and McuProtocol.FLAG_RESPONSE == 0) return null
        val status = McuExtendedStatus.from(frame) ?: return null
        val probe = probes.remove(frame.sequence) ?: return status.copy(stale = true, receivedAtElapsedMs = clock())
        retire(frame.sequence)
        if (!probe.extension || probe.controlledCommand != status.controlledCommand || probe.selector != status.selector) return null
        val joint = capabilities.joints.firstOrNull { it.jointId == status.selector }
        if (status.controlledCommand in setOf(McuProtocol.COMMAND_SET_ARM_POSITION, McuProtocol.COMMAND_HOME_ARM) &&
            (joint == null || status.actualValue !in joint.minPositionMilliDegrees..joint.maxPositionMilliDegrees ||
                status.targetValue !in joint.minPositionMilliDegrees..joint.maxPositionMilliDegrees ||
                kotlin.math.abs(status.actualSpeed.toLong()) > joint.maxSpeedMilliDegreesPerSecond ||
                kotlin.math.abs(status.targetSpeed.toLong()) > joint.maxSpeedMilliDegreesPerSecond)) return null
        val fresh = clock() - probe.requestedAt <= options.statusStaleAfterMs
        probe.commandIds.forEach { id -> pending[id]?.let { p ->
            if (!fresh) finish(p, McuCommandOutcome.STALE_STATUS)
            else if (status.matches(p.dispatch.frame, capabilities)) finish(p, McuCommandOutcome.APPLIED)
        } }
        val annotated = status.copy(stale = !fresh, receivedAtElapsedMs = clock(), relatedCommandIds = probe.commandIds)
        extendedSnapshots[status.controlledCommand to status.selector] = annotated
        return annotated
    }

    @Synchronized fun isStatusStale(): Boolean = closed || !connected ||
        (lastStatus?.let { it.stale || clock() - it.receivedAtElapsedMs > options.statusStaleAfterMs } ?: true)

    /** Returns retries, or a watchdog STOP. Never returns superseded, ACKed, or closed work. */
    @Synchronized fun maintenance(): List<Dispatch> {
        if (!connected || closed) return emptyList()
        val now = clock()
        probes.entries.filter { now - it.value.requestedAt > options.statusStaleAfterMs * 2 }
            .map { it.key }.forEach { probes.remove(it); retire(it) }
        if (motionDeadline?.let { now >= it } == true) return listOf(submitMotion(McuMotion.STOP, 0))
        val retry = mutableListOf<Dispatch>()
        pending.values.toList().forEach { p ->
            val ackAt = p.ackAt
            if (ackAt != null) {
                if (now - ackAt >= options.stateTimeoutMs) finish(p, McuCommandOutcome.STATE_TIMEOUT)
            } else if (p.sentAt != null && now - p.sentAt!! >= options.ackTimeoutMs) {
                if (p.retries >= options.maxRetries) finish(p, McuCommandOutcome.TIMEOUT)
                else if (canSend(p.dispatch)) retry += p.dispatch
            }
        }
        return retry
    }

    @Synchronized fun close() {
        if (closed) return
        closed = true; connected = false; motionEpoch++; motionDeadline = null
        pending.values.toList().forEach { finish(it, McuCommandOutcome.CLOSED) }
        probes.clear()
        lastStatus = null
        extendedSnapshots.clear()
    }

    private fun matches(frame: McuFrame, status: McuStatus): Boolean {
        if (status.faultFlags != 0) return false
        val p = frame.payload
        return when (frame.command) {
            McuProtocol.COMMAND_SET_LIGHT -> status.lightEnabled == (McuProtocol.unsigned(p[0]) == 1) && status.brightnessPercent == McuProtocol.unsigned(p[1])
            McuProtocol.COMMAND_SET_MOTION -> status.motion.code == McuProtocol.unsigned(p[0]) && status.targetMotion.code == McuProtocol.unsigned(p[0]) &&
                status.motionSpeedPercent == McuProtocol.unsigned(p[1]) && status.targetSpeedPercent == McuProtocol.unsigned(p[1])
            else -> false
        }
    }
    private fun nextSequence(): Int {
        retiredSequences.entries.removeAll { clock() >= it.value }
        repeat(255) {
            sequence = sequence % 255 + 1
            if (pending.values.none { it.dispatch.submission.sequence == sequence } && sequence !in probes &&
                sequence !in retiredSequences) return sequence
        }
        return -1
    }
    private fun finish(p: Pending, outcome: McuCommandOutcome, resultCode: Int? = null, detail: Int? = null) {
        pending.remove(p.dispatch.submission.commandId)
        retire(p.dispatch.submission.sequence)
        emit(p.dispatch, outcome, resultCode, detail)
    }
    private fun retire(sequence: Int) {
        retiredSequences[sequence] = clock() + options.statusStaleAfterMs + options.stateTimeoutMs
    }
    private fun emit(d: Dispatch, outcome: McuCommandOutcome, resultCode: Int? = null, detail: Int? = null) = onResult(
        McuCommandResult(d.submission.sequence, d.submission.command, outcome, resultCode, detail, d.submission.commandId, clock())
    )
}
