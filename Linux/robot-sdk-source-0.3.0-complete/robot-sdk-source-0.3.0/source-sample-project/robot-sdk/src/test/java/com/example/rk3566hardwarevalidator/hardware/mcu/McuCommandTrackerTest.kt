package com.example.rk3566hardwarevalidator.hardware.mcu

import org.junit.Assert.*
import org.junit.Test

class McuCommandTrackerTest {
    private var now = 100L
    private val results = mutableListOf<McuCommandResult>()
    private fun tracker(capabilities: McuCapabilities = McuCapabilities.LEGACY_V2,
                        options: McuSafetyOptions = McuSafetyOptions()) =
        McuCommandTracker(capabilities, options, { now }, results::add).apply {
            setConnected(true)
            onStatus(status(statusRequest()!!.sequence))
        }
    private fun ack(d: McuCommandTracker.Dispatch, result: Int = 0) = McuFrame(
        flags = McuProtocol.FLAG_RESPONSE, sequence = d.submission.sequence,
        command = McuProtocol.COMMAND_ACK, payload = byteArrayOf(d.frame.command.toByte(), result.toByte(), 0, 0))
    private fun status(sequence: Int, actual: McuMotion = McuMotion.STOP, target: McuMotion = actual,
                       speed: Int = 0, targetSpeed: Int = speed, fault: Int = 0): McuFrame {
        val p = ByteArray(16)
        p[2] = actual.code.toByte(); p[3] = target.code.toByte()
        p[4] = speed.toByte(); p[5] = targetSpeed.toByte(); p[6] = fault.toByte()
        return McuFrame(flags = McuProtocol.FLAG_RESPONSE, sequence = sequence,
            command = McuProtocol.COMMAND_STATUS, payload = p)
    }

    @Test fun ackDoesNotProveApplicationAndTargetAloneDoesNotProveIt() {
        val t = tracker()
        val d = t.submitMotion(McuMotion.FORWARD, 30)
        t.markSent(d); t.onAck(ack(d))
        assertEquals(listOf(McuCommandOutcome.ACK_RECEIVED), results.map { it.outcome })
        val q = t.statusRequest()!!
        t.onStatus(status(q.sequence, McuMotion.STOP, McuMotion.FORWARD, 0, 30))
        assertEquals(1, results.size)
        val q2 = t.statusRequest()!!
        val observed = t.onStatus(status(q2.sequence, McuMotion.FORWARD, speed = 30))!!
        assertFalse(observed.stale)
        assertEquals(listOf(d.submission.commandId), observed.relatedCommandIds)
        assertEquals(McuCommandOutcome.APPLIED, results.last().outcome)
        assertEquals(d.submission.commandId, results.last().commandId)
    }

    @Test fun oldOrUnsolicitedStatusCannotApplyNewCommand() {
        val t = tracker()
        val before = t.statusRequest()!!
        val d = t.submitMotion(McuMotion.FORWARD, 40)
        t.markSent(d); t.onAck(ack(d))
        t.onStatus(status(before.sequence, McuMotion.FORWARD, speed = 40))
        t.onStatus(status(254, McuMotion.FORWARD, speed = 40))
        assertEquals(listOf(McuCommandOutcome.ACK_RECEIVED), results.map { it.outcome })
        assertTrue(t.isStatusStale())
    }

    @Test fun expiredStatusIsTerminalButCannotClaimApplied() {
        val t = tracker(options = McuSafetyOptions(statusStaleAfterMs = 10))
        val d = t.submitMotion(McuMotion.FORWARD, 40)
        t.markSent(d); t.onAck(ack(d)); val q = t.statusRequest()!!
        now += 11
        assertTrue(t.onStatus(status(q.sequence, McuMotion.FORWARD, speed = 40))!!.stale)
        assertEquals(McuCommandOutcome.STALE_STATUS, results.last().outcome)
    }

    @Test fun statusAgeAndDisconnectInvalidateState() {
        val t = tracker(options = McuSafetyOptions(statusStaleAfterMs = 10))
        t.onStatus(status(t.statusRequest()!!.sequence))
        assertFalse(t.isStatusStale()); now += 11; assertTrue(t.isStatusStale())
        t.setConnected(false); assertNull(t.lastStatus)
    }

    @Test fun stopPreemptsQueuedAndRetryMotion() {
        val t = tracker()
        val sent = t.submitMotion(McuMotion.FORWARD, 60); t.markSent(sent)
        now += 500
        val retry = t.maintenance().single()
        val stop = t.submitMotion(McuMotion.STOP, 0)
        assertFalse(t.canSend(sent)); assertFalse(t.canSend(retry)); assertTrue(t.canSend(stop))
        assertEquals(0, stop.submission.sequence)
        now += 500
        assertTrue(t.maintenance().isEmpty()) // STOP has not yet been sent.
        assertEquals(McuCommandOutcome.SUPERSEDED, results.first().outcome)
        t.onAck(ack(sent)); assertEquals(1, results.size)
    }

    @Test fun newerSameActuatorSupersedesOldAndItsLateAck() {
        val t = tracker()
        val old = t.submitLight(true, 20); t.markSent(old)
        val replacement = t.submitLight(false, 0)
        assertFalse(t.canSend(old)); assertTrue(t.canSend(replacement))
        assertFalse(t.onAck(ack(old)))
        assertEquals(McuCommandOutcome.SUPERSEDED, results.single().outcome)
    }

    @Test fun timeoutRejectionAndStateTimeoutAreDifferent() {
        val t = tracker(options = McuSafetyOptions(ackTimeoutMs = 10, maxRetries = 0, stateTimeoutMs = 20))
        val lost = t.submitLight(true, 10); t.markSent(lost); now += 10; t.maintenance()
        assertEquals(McuCommandOutcome.TIMEOUT, results.last().outcome)
        val rejected = t.submitLight(true, 10); t.markSent(rejected); t.onAck(ack(rejected, McuProtocol.RESULT_BUSY))
        assertEquals(McuCommandOutcome.REJECTED, results.last().outcome)
        val unobserved = t.submitLight(true, 10); t.markSent(unobserved); t.onAck(ack(unobserved))
        now += 20; t.maintenance(); assertEquals(McuCommandOutcome.STATE_TIMEOUT, results.last().outcome)
    }

    @Test fun malformedOrFaultedStatusCannotApply() {
        val t = tracker(); val d = t.submitMotion(McuMotion.FORWARD, 30)
        t.markSent(d); t.onAck(ack(d)); val q = t.statusRequest()!!
        val malformed = status(q.sequence, McuMotion.FORWARD, speed = 30).also { it.payload[4] = 120 }
        assertNull(t.onStatus(malformed))
        t.onStatus(status(q.sequence, McuMotion.FORWARD, speed = 30, fault = 1))
        assertEquals(McuCommandOutcome.ACK_RECEIVED, results.last().outcome)
    }

    @Test fun closedAndNotConnectedAreTypedNoExceptionOrRetries() {
        val t = tracker(); val d = t.submitMotion(McuMotion.FORWARD, 30); t.markSent(d)
        t.close(); assertEquals(McuCommandOutcome.CLOSED, results.last().outcome)
        assertFalse(t.canSend(d)); assertTrue(t.maintenance().isEmpty())
        assertEquals(McuCommandOutcome.CLOSED, t.submitLight(true, 40).submission.outcome)
        val disconnected = tracker(); disconnected.setConnected(false)
        assertEquals(McuCommandOutcome.NOT_CONNECTED, disconnected.submitMotion(McuMotion.FORWARD).submission.outcome)
    }

    @Test fun defaultCapabilitiesNeverEncodeDangerousExtensionAndInvalidInputIsTyped() {
        val t = tracker()
        assertEquals(McuCommandOutcome.UNSUPPORTED, t.submitLightMode(1, 30).submission.outcome)
        assertEquals(McuCommandOutcome.UNSUPPORTED, t.submitArmPosition(0, 90_000, 30_000).submission.outcome)
        assertEquals(McuCommandOutcome.INVALID_ARGUMENT, t.submitLight(true, 101).submission.outcome)
        assertEquals(McuCommandOutcome.INVALID_ARGUMENT, t.submitMotion(McuMotion.UNKNOWN).submission.outcome)
        assertFalse(t.capabilities.joints.isNotEmpty())
    }

    @Test fun firmwareDescriptorBoundsAreAuthoritativeAndExtensionUsesSignedLittleEndian() {
        val caps = McuCapabilities(lightModes = setOf(7),
            joints = listOf(McuJointCapability(3, -45_000, 80_000, 20_000)), firmwareDescriptorId = "simulator-v1")
        val t = tracker(caps)
        assertEquals(McuCommandOutcome.INVALID_ARGUMENT, t.submitArmPosition(3, 90_000, 10_000).submission.outcome)
        assertEquals(McuCommandOutcome.INVALID_ARGUMENT, t.submitArmPosition(3, 0, 30_000).submission.outcome)
        val d = t.submitArmPosition(3, -20_000, 10_000)
        assertTrue(d.submission.accepted)
        assertEquals(-20_000, McuProtocol.readInt32(d.frame.payload, 4))
        assertEquals(McuProtocol.COMMAND_SET_ARM_POSITION, d.frame.command)
        assertEquals(McuCommandOutcome.UNSUPPORTED, t.submitLightMode(1, 30).submission.outcome)
    }

    @Test fun watchdogEmitsStopOnlyAndInvalidatesOldRetry() {
        val t = tracker(options = McuSafetyOptions(motionTimeoutMs = 50))
        val d = t.submitMotion(McuMotion.FORWARD, 30); t.markSent(d)
        now += 50
        val stop = t.maintenance().single()
        assertEquals(McuMotion.STOP.code, McuProtocol.unsigned(stop.frame.payload[0]))
        assertFalse(t.canSend(d)); assertTrue(t.canSend(stop))
    }

    @Test fun stopAlsoCancelsQueuedAndRetryingArmCommands() {
        val caps = McuCapabilities(joints = listOf(McuJointCapability(1, -100, 100, 50, 0),
            McuJointCapability(2, -100, 100, 50, 0)), firmwareDescriptorId = "test")
        val t = tracker(caps)
        val queued = t.submitArmPosition(1, 50, 20)
        val sent = t.submitArmHome(2)
        t.markSent(sent)
        val stop = t.submitMotion(McuMotion.STOP, 0)
        assertFalse(t.canSend(queued))
        assertFalse(t.canSend(sent))
        assertTrue(t.canSend(stop))
        assertEquals(McuCommandOutcome.SUPERSEDED, results.first { it.commandId == queued.submission.commandId }.outcome)
        assertEquals(McuCommandOutcome.SUPERSEDED, results.first { it.commandId == sent.submission.commandId }.outcome)
        now += 500
        assertTrue(t.maintenance().none { it == queued || it == sent })
    }

    @Test fun crcFragmentationAndRecoveryCoverNewCommands() {
        val frame = McuProtocol.setArmPosition(34, 1, -12345, 1000)
        val bytes = McuProtocol.encode(frame)
        val decoder = McuFrameDecoder()
        assertTrue(decoder.feed(bytes.copyOfRange(0, 7)).isEmpty())
        assertEquals(frame, decoder.feed(bytes.copyOfRange(7, bytes.size)).single())
        val damaged = bytes.copyOf().also { it[12] = (it[12].toInt() xor 1).toByte() }
        assertEquals(frame, McuFrameDecoder().feed(damaged + bytes).single())
    }

    @Test fun actuationRequiresFreshFaultFreeStateButStopDoesNot() {
        val caps = McuCapabilities(joints = listOf(McuJointCapability(3, -100, 100, 50, 0)),
            firmwareDescriptorId = "test-firmware", supportsSoftShutdown = true)
        val t = McuCommandTracker(caps, clock = { now }, onResult = results::add)
        t.setConnected(true)
        assertEquals(McuCommandOutcome.STALE_STATUS, t.submitMotion(McuMotion.FORWARD, 20).submission.outcome)
        assertEquals(McuCommandOutcome.STALE_STATUS, t.submitArmPosition(3, 50, 20).submission.outcome)
        assertEquals(McuCommandOutcome.STALE_STATUS, t.submitArmHome(3).submission.outcome)
        assertEquals(McuCommandOutcome.STALE_STATUS, t.submitSoftShutdown().submission.outcome)
        assertTrue(t.submitMotion(McuMotion.STOP, 0).submission.accepted)
        t.onStatus(status(t.statusRequest()!!.sequence, fault = 1))
        assertEquals(McuCommandOutcome.REJECTED, t.submitMotion(McuMotion.FORWARD, 20).submission.outcome)
        assertEquals(McuCommandOutcome.REJECTED, t.submitArmHome(3).submission.outcome)
        assertTrue(t.submitMotion(McuMotion.STOP, 0).submission.accepted)
        t.onStatus(status(t.statusRequest()!!.sequence))
        assertTrue(t.submitArmPosition(3, 50, 20).submission.accepted)
        now += 5001
        assertEquals(McuCommandOutcome.STALE_STATUS, t.submitMotion(McuMotion.FORWARD, 20).submission.outcome)
    }

    @Test fun allOptionalExtensionsAreGatedAndHomeSupersedesJointPosition() {
        val legacy = tracker()
        assertEquals(McuCommandOutcome.UNSUPPORTED, legacy.submitArmHome(3).submission.outcome)
        assertEquals(McuCommandOutcome.UNSUPPORTED, legacy.submitLightZone(1, true, 50).submission.outcome)
        assertEquals(McuCommandOutcome.UNSUPPORTED, legacy.submitBuzzer(1, 50).submission.outcome)
        assertEquals(McuCommandOutcome.UNSUPPORTED, legacy.submitIndicator(2, true).submission.outcome)
        assertEquals(McuCommandOutcome.UNSUPPORTED, legacy.submitSoftShutdown().submission.outcome)
        val caps = McuCapabilities(joints = listOf(McuJointCapability(3, -100, 100, 50, 0)),
            firmwareDescriptorId = "test-firmware", lightZoneIds = setOf(1),
            buzzerPatternIds = setOf(0, 1), indicatorIds = setOf(2), supportsSoftShutdown = true)
        val t = tracker(caps)
        val position = t.submitArmPosition(3, 50, 20)
        val home = t.submitArmHome(3)
        assertFalse(t.canSend(position)); assertTrue(t.canSend(home))
        assertTrue(t.submitLightZone(1, true, 50).submission.accepted)
        assertTrue(t.submitBuzzer(1, 50).submission.accepted)
        assertTrue(t.submitIndicator(2, true).submission.accepted)
        assertTrue(t.submitSoftShutdown().submission.accepted)
        assertFalse(t.canSend(home))
    }

    @Test fun extensionApplicationNeedsObservedPositionAndCanPollAgain() {
        val caps = McuCapabilities(joints = listOf(McuJointCapability(3, -100, 100, 50, 0)),
            firmwareDescriptorId = "test-firmware")
        val t = tracker(caps)
        val d = t.submitArmPosition(3, 50, 20); t.markSent(d); t.onAck(ack(d))
        fun reply(sequence: Int, actual: Int, speed: Int): McuFrame {
            val p = ByteArray(20); p[0] = McuProtocol.COMMAND_SET_ARM_POSITION.toByte(); p[1] = 3
            McuProtocol.writeInt32(p, 4, actual); McuProtocol.writeInt32(p, 8, 50)
            McuProtocol.writeInt32(p, 12, speed)
            return McuFrame(flags = McuProtocol.FLAG_RESPONSE, sequence = sequence,
                command = McuProtocol.COMMAND_EXTENDED_STATUS, payload = p)
        }
        val q = t.statusRequest(d)!!
        t.onExtendedStatus(reply(q.sequence, 20, 20))
        assertEquals(McuCommandOutcome.ACK_RECEIVED, results.last().outcome)
        now += 500
        assertEquals(d, t.awaitingExtensionConfirmation().single())
        t.onExtendedStatus(reply(t.statusRequest(d)!!.sequence, 50, 0))
        assertEquals(McuCommandOutcome.APPLIED, results.last().outcome)
    }

    @Test fun staleStateAtActualWritePreventsQueuedMotionAndOldQueuedLightExpires() {
        val t = tracker()
        val d = t.submitMotion(McuMotion.FORWARD, 30)
        now += 100
        t.onStatus(status(t.statusRequest()!!.sequence, fault = 1))
        assertFalse(t.canSend(d)); assertEquals(McuCommandOutcome.REJECTED, results.last().outcome)
        val light = t.submitLight(true, 30)
        now += 3000
        assertFalse(t.canSend(light)); assertEquals(McuCommandOutcome.TIMEOUT, results.last().outcome)
        assertTrue(t.submitMotion(McuMotion.STOP, 0).submission.accepted)
    }

    @Test fun readOnlyQueriesNeedCapabilityButNeverRequireOrIssueActuation() {
        val caps = McuCapabilities(joints = listOf(McuJointCapability(3, -100, 100, 50, 0)),
            lightZoneIds = setOf(1), firmwareDescriptorId = "test-firmware")
        val t = McuCommandTracker(caps, clock = { now }, onResult = results::add)
        t.setConnected(true)
        assertTrue(t.isStatusStale())
        assertNull(t.jointSnapshots().single().actualPositionMilliDegrees)
        val q = t.prepareExtendedStatusQuery(McuCommandType.ARM_POSITION, 3)
        assertTrue(q.submission.accepted)
        assertEquals(McuProtocol.COMMAND_GET_EXTENDED_STATUS, q.frame!!.command)
        assertFalse(q.frame!!.flags and McuProtocol.FLAG_ACK_REQUIRED != 0)
        val p = ByteArray(20); p[0] = McuProtocol.COMMAND_SET_ARM_POSITION.toByte(); p[1] = 3
        McuProtocol.writeInt32(p, 4, 25); McuProtocol.writeInt32(p, 8, 25)
        t.onExtendedStatus(McuFrame(flags = McuProtocol.FLAG_RESPONSE, sequence = q.submission.sequence,
            command = McuProtocol.COMMAND_EXTENDED_STATUS, payload = p))
        val observed = t.jointSnapshots().single()
        assertEquals(25, observed.actualPositionMilliDegrees); assertFalse(observed.stale)
        assertEquals(McuPhysicalHomeInput.UNKNOWN, observed.physicalHomeInput)
        assertEquals(McuCalibrationState.UNKNOWN, observed.calibrationState)
        assertTrue(results.isEmpty()) // A read-only query has not submitted any actuator command.
        now += 5001; assertTrue(t.jointSnapshots().single().stale)
        t.setConnected(false); assertNull(t.jointSnapshots().single().actualPositionMilliDegrees)
        assertEquals(McuCommandOutcome.NOT_CONNECTED,
            t.prepareExtendedStatusQuery(McuCommandType.ARM_POSITION, 3).submission.outcome)
        assertEquals(McuCommandOutcome.UNSUPPORTED,
            tracker().prepareExtendedStatusQuery(McuCommandType.ARM_POSITION, 3).submission.outcome)
    }

    @Test fun reservedBytesUnknownMotionAndInvalidExtensionStatusNeverBecomeFresh() {
        val t = tracker()
        val q = t.statusRequest()!!
        for (offset in listOf(2, 3, 14, 15)) {
            val invalid = status(q.sequence)
            invalid.payload[offset] = if (offset < 4) 99 else 1
            assertNull(t.onStatus(invalid))
        }
        val d = t.submitLight(true, 20); t.markSent(d)
        assertFalse(t.onAck(ack(d).also { it.payload[3] = 1 }))
        assertTrue(results.isEmpty())
        val p = ByteArray(20)
        p[0] = McuProtocol.COMMAND_SET_INDICATOR.toByte(); p[1] = 2
        assertNull(McuExtendedStatus.from(McuFrame(sequence = 1, command = McuProtocol.COMMAND_EXTENDED_STATUS,
            payload = p.copyOf().also { it[3] = 1 })))
        assertNull(McuExtendedStatus.from(McuFrame(sequence = 1, command = McuProtocol.COMMAND_EXTENDED_STATUS,
            payload = p.copyOf().also { it[0] = 99 })))
        assertNull(McuExtendedStatus.from(McuFrame(sequence = 1, command = McuProtocol.COMMAND_EXTENDED_STATUS,
            payload = p.copyOf().also { McuProtocol.writeInt32(it, 4, 2) })))
    }
}
