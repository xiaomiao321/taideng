package com.example.rk3566hardwarevalidator.hardware.mcu

/**
 * MCU serial protocol shared by the Android transport and local unit tests.
 * Multi-byte values are little-endian.
 */
object McuProtocol {
    const val SOF_1 = 0xA5
    const val SOF_2 = 0x5A
    const val VERSION = 0x02
    const val MAX_PAYLOAD_LENGTH = 64

    const val FLAG_RESPONSE = 0x01
    const val FLAG_ACK_REQUIRED = 0x02
    const val FLAG_ERROR = 0x04

    const val COMMAND_PING = 0x01
    const val COMMAND_SET_LIGHT = 0x10
    const val COMMAND_SET_MOTION = 0x11
    const val COMMAND_GET_STATUS = 0x20
    const val COMMAND_STATUS = 0x21
    // Reserved extension v1. Never sent without a firmware-validated capability descriptor.
    const val COMMAND_SET_LIGHT_MODE = 0x30
    const val COMMAND_SET_ARM_POSITION = 0x31
    const val COMMAND_GET_EXTENDED_STATUS = 0x32
    const val COMMAND_EXTENDED_STATUS = 0x33
    const val COMMAND_HOME_ARM = 0x34
    const val COMMAND_SET_LIGHT_ZONE = 0x35
    const val COMMAND_SET_BUZZER = 0x36
    const val COMMAND_SET_INDICATOR = 0x37
    const val COMMAND_SOFT_SHUTDOWN = 0x38
    const val COMMAND_ACK = 0x7F

    const val RESULT_OK = 0x00
    const val RESULT_INVALID_COMMAND = 0x01
    const val RESULT_INVALID_LENGTH = 0x02
    const val RESULT_INVALID_VALUE = 0x03
    const val RESULT_BUSY = 0x04
    const val RESULT_HARDWARE_FAULT = 0x05
    const val RESULT_INTERNAL_ERROR = 0xFF

    fun encode(frame: McuFrame): ByteArray {
        require(frame.version in 0..0xFF)
        require(frame.flags in 0..0xFF)
        require(frame.sequence in 0..0xFF)
        require(frame.command in 0..0xFF)
        require(frame.payload.size <= MAX_PAYLOAD_LENGTH)

        val payloadLength = frame.payload.size
        val result = ByteArray(FRAME_OVERHEAD + payloadLength)
        result[0] = SOF_1.toByte()
        result[1] = SOF_2.toByte()
        result[2] = frame.version.toByte()
        result[3] = frame.flags.toByte()
        result[4] = frame.sequence.toByte()
        result[5] = frame.command.toByte()
        result[6] = payloadLength.toByte()
        result[7] = (payloadLength ushr 8).toByte()
        frame.payload.copyInto(result, destinationOffset = HEADER_LENGTH)
        val crc = crc16Modbus(result, offset = 2, length = 6 + payloadLength)
        result[result.lastIndex - 1] = crc.toByte()
        result[result.lastIndex] = (crc ushr 8).toByte()
        return result
    }

    fun setLight(sequence: Int, enabled: Boolean, brightnessPercent: Int): McuFrame {
        require(brightnessPercent in 0..100)
        return McuFrame(
            flags = FLAG_ACK_REQUIRED,
            sequence = sequence,
            command = COMMAND_SET_LIGHT,
            payload = byteArrayOf(
                (if (enabled) 0x01 else 0x00).toByte(),
                brightnessPercent.toByte(),
                0x00,
                0x00
            )
        )
    }

    fun setMotion(
        sequence: Int,
        motion: McuMotion,
        speedPercent: Int = DEFAULT_MOTION_SPEED
    ): McuFrame {
        require(motion != McuMotion.UNKNOWN)
        require(speedPercent in 0..100)
        val effectiveSpeed = if (motion == McuMotion.STOP) 0 else speedPercent
        return McuFrame(
            flags = FLAG_ACK_REQUIRED,
            sequence = sequence,
            command = COMMAND_SET_MOTION,
            payload = byteArrayOf(
                motion.code.toByte(),
                effectiveSpeed.toByte(),
                0x00, // control flags, reserved
                0x00  // reserved
            )
        )
    }

    fun getStatus(sequence: Int): McuFrame = McuFrame(
        sequence = sequence,
        command = COMMAND_GET_STATUS
    )

    fun setLightMode(sequence: Int, modeId: Int, brightnessPercent: Int): McuFrame {
        require(modeId in 0..255 && brightnessPercent in 0..100)
        return McuFrame(flags = FLAG_ACK_REQUIRED, sequence = sequence,
            command = COMMAND_SET_LIGHT_MODE, payload = byteArrayOf(modeId.toByte(), brightnessPercent.toByte(), 0, 0))
    }

    fun setArmPosition(sequence: Int, jointId: Int, positionMilliDegrees: Int,
                       speedMilliDegreesPerSecond: Int): McuFrame {
        require(jointId in 0..255 && speedMilliDegreesPerSecond > 0)
        val p = ByteArray(12)
        p[0] = jointId.toByte()
        writeInt32(p, 4, positionMilliDegrees)
        writeInt32(p, 8, speedMilliDegreesPerSecond)
        return McuFrame(flags = FLAG_ACK_REQUIRED, sequence = sequence, command = COMMAND_SET_ARM_POSITION, payload = p)
    }

    fun getExtendedStatus(sequence: Int, command: Int, selector: Int): McuFrame = McuFrame(
        sequence = sequence, command = COMMAND_GET_EXTENDED_STATUS,
        payload = byteArrayOf(command.toByte(), selector.toByte(), 0, 0))

    fun homeArm(sequence: Int, jointId: Int): McuFrame {
        require(jointId in 0..255)
        return extension(sequence, COMMAND_HOME_ARM, byteArrayOf(jointId.toByte(), 0, 0, 0))
    }

    fun setLightZone(sequence: Int, zoneId: Int, enabled: Boolean, brightnessPercent: Int): McuFrame {
        require(zoneId in 0..255 && brightnessPercent in 0..100)
        return extension(sequence, COMMAND_SET_LIGHT_ZONE, byteArrayOf(zoneId.toByte(), if (enabled) 1 else 0, brightnessPercent.toByte(), 0))
    }

    fun setBuzzer(sequence: Int, patternId: Int, volumePercent: Int): McuFrame {
        require(patternId in 0..255 && volumePercent in 0..100)
        return extension(sequence, COMMAND_SET_BUZZER, byteArrayOf(patternId.toByte(), volumePercent.toByte(), 0, 0))
    }

    fun setIndicator(sequence: Int, indicatorId: Int, enabled: Boolean): McuFrame {
        require(indicatorId in 0..255)
        return extension(sequence, COMMAND_SET_INDICATOR, byteArrayOf(indicatorId.toByte(), if (enabled) 1 else 0, 0, 0))
    }

    fun softShutdown(sequence: Int): McuFrame = extension(sequence, COMMAND_SOFT_SHUTDOWN, byteArrayOf(0x53, 0x44, 0x01, 0))

    private fun extension(sequence: Int, command: Int, payload: ByteArray): McuFrame =
        McuFrame(flags = FLAG_ACK_REQUIRED, sequence = sequence, command = command, payload = payload)

    internal fun readInt32(p: ByteArray, offset: Int): Int = unsigned(p[offset]) or
        (unsigned(p[offset + 1]) shl 8) or (unsigned(p[offset + 2]) shl 16) or (unsigned(p[offset + 3]) shl 24)

    internal fun writeInt32(p: ByteArray, offset: Int, value: Int) {
        repeat(4) { p[offset + it] = (value ushr (it * 8)).toByte() }
    }

    fun crc16Modbus(data: ByteArray, offset: Int = 0, length: Int = data.size): Int {
        require(offset >= 0 && length >= 0 && offset + length <= data.size)
        var crc = 0xFFFF
        for (index in offset until offset + length) {
            crc = crc xor unsigned(data[index])
            repeat(8) {
                crc = if (crc and 0x0001 != 0) {
                    (crc ushr 1) xor 0xA001
                } else {
                    crc ushr 1
                }
            }
        }
        return crc and 0xFFFF
    }

    internal fun unsigned(value: Byte): Int = value.toInt() and 0xFF

    const val HEADER_LENGTH = 8
    const val FRAME_OVERHEAD = 10
    const val DEFAULT_MOTION_SPEED = 100
}

enum class McuMotion(val code: Int) {
    STOP(0x00),
    FORWARD(0x01),
    BACKWARD(0x02),
    TURN_LEFT(0x03),
    TURN_RIGHT(0x04),
    UNKNOWN(0xFF);

    companion object {
        fun fromCode(code: Int): McuMotion = entries.firstOrNull { it.code == code } ?: UNKNOWN
    }
}

data class McuFrame(
    val version: Int = McuProtocol.VERSION,
    val flags: Int = 0,
    val sequence: Int,
    val command: Int,
    val payload: ByteArray = byteArrayOf()
) {
    override fun equals(other: Any?): Boolean = other is McuFrame &&
        version == other.version &&
        flags == other.flags &&
        sequence == other.sequence &&
        command == other.command &&
        payload.contentEquals(other.payload)

    override fun hashCode(): Int {
        var result = version
        result = 31 * result + flags
        result = 31 * result + sequence
        result = 31 * result + command
        result = 31 * result + payload.contentHashCode()
        return result
    }
}

data class McuAck(
    val acknowledgedCommand: Int,
    val resultCode: Int,
    val detail: Int
) {
    val isSuccess: Boolean get() = resultCode == McuProtocol.RESULT_OK

    companion object {
        fun from(frame: McuFrame): McuAck? {
            if (frame.command != McuProtocol.COMMAND_ACK || frame.payload.size != 4) return null
            if (frame.payload[3].toInt() != 0) return null
            return McuAck(
                acknowledgedCommand = McuProtocol.unsigned(frame.payload[0]),
                resultCode = McuProtocol.unsigned(frame.payload[1]),
                detail = McuProtocol.unsigned(frame.payload[2])
            )
        }
    }
}

data class McuStatus(
    val lightEnabled: Boolean,
    val brightnessPercent: Int,
    val motion: McuMotion,
    val targetMotion: McuMotion,
    val motionSpeedPercent: Int,
    val targetSpeedPercent: Int,
    val faultFlags: Int,
    val statusFlags: Int,
    val uptimeSeconds: Long,
    val protocolErrorCount: Int,
    val receivedAtElapsedMs: Long = 0,
    val stale: Boolean = true,
    val relatedCommandIds: List<Long> = emptyList()
) {
    companion object {
        fun from(frame: McuFrame): McuStatus? {
            if (frame.command != McuProtocol.COMMAND_STATUS || frame.payload.size != 16) return null
            val payload = frame.payload
            if (McuProtocol.unsigned(payload[0]) > 1 || McuProtocol.unsigned(payload[1]) > 100 ||
                McuProtocol.unsigned(payload[4]) > 100 || McuProtocol.unsigned(payload[5]) > 100 ||
                McuProtocol.unsigned(payload[2]) > 4 || McuProtocol.unsigned(payload[3]) > 4 ||
                payload[14].toInt() != 0 || payload[15].toInt() != 0) return null
            val uptime = McuProtocol.unsigned(payload[8]).toLong() or
                (McuProtocol.unsigned(payload[9]).toLong() shl 8) or
                (McuProtocol.unsigned(payload[10]).toLong() shl 16) or
                (McuProtocol.unsigned(payload[11]).toLong() shl 24)
            return McuStatus(
                lightEnabled = McuProtocol.unsigned(payload[0]) and 0x01 != 0,
                brightnessPercent = McuProtocol.unsigned(payload[1]).coerceIn(0, 100),
                motion = McuMotion.fromCode(McuProtocol.unsigned(payload[2])),
                targetMotion = McuMotion.fromCode(McuProtocol.unsigned(payload[3])),
                motionSpeedPercent = McuProtocol.unsigned(payload[4]).coerceIn(0, 100),
                targetSpeedPercent = McuProtocol.unsigned(payload[5]).coerceIn(0, 100),
                faultFlags = McuProtocol.unsigned(payload[6]),
                statusFlags = McuProtocol.unsigned(payload[7]),
                uptimeSeconds = uptime,
                protocolErrorCount = McuProtocol.unsigned(payload[12]) or
                    (McuProtocol.unsigned(payload[13]) shl 8)
            )
        }
    }
}

/** Extension v1, reported values are measured/observed values, never copied from an ACK. */
data class McuExtendedStatus(
    val controlledCommand: Int, val selector: Int, val faultFlags: Int,
    val actualValue: Int, val targetValue: Int, val actualSpeed: Int, val targetSpeed: Int,
    val receivedAtElapsedMs: Long = 0, val stale: Boolean = true,
    val relatedCommandIds: List<Long> = emptyList()
) {
    internal fun matches(frame: McuFrame, capabilities: McuCapabilities): Boolean {
        if (faultFlags != 0 || frame.command != controlledCommand || McuProtocol.unsigned(frame.payload[0]) != selector) return false
        return when (controlledCommand) {
            McuProtocol.COMMAND_SET_LIGHT_MODE -> actualValue == selector && targetValue == selector &&
                actualSpeed == McuProtocol.unsigned(frame.payload[1]) && targetSpeed == actualSpeed
            McuProtocol.COMMAND_SET_ARM_POSITION -> actualValue == McuProtocol.readInt32(frame.payload, 4) &&
                targetValue == actualValue && actualSpeed == 0 && targetSpeed == 0
            McuProtocol.COMMAND_HOME_ARM -> capabilities.joints.firstOrNull { it.jointId == selector }
                ?.homePositionMilliDegrees?.let { actualValue == it && targetValue == it && actualSpeed == 0 && targetSpeed == 0 } == true
            McuProtocol.COMMAND_SET_LIGHT_ZONE -> actualValue == McuProtocol.unsigned(frame.payload[1]) && targetValue == actualValue &&
                actualSpeed == McuProtocol.unsigned(frame.payload[2]) && targetSpeed == actualSpeed
            McuProtocol.COMMAND_SET_BUZZER -> actualValue == selector && targetValue == selector &&
                actualSpeed == McuProtocol.unsigned(frame.payload[1]) && targetSpeed == actualSpeed
            McuProtocol.COMMAND_SET_INDICATOR -> actualValue == McuProtocol.unsigned(frame.payload[1]) &&
                targetValue == actualValue && actualSpeed == 0 && targetSpeed == 0
            else -> false
        }
    }
    companion object {
        fun from(frame: McuFrame): McuExtendedStatus? {
            if (frame.command != McuProtocol.COMMAND_EXTENDED_STATUS || frame.payload.size != 20) return null
            val p = frame.payload
            val controlled = McuProtocol.unsigned(p[0])
            if (p[3].toInt() != 0 || controlled !in setOf(McuProtocol.COMMAND_SET_LIGHT_MODE,
                    McuProtocol.COMMAND_SET_ARM_POSITION, McuProtocol.COMMAND_HOME_ARM,
                    McuProtocol.COMMAND_SET_LIGHT_ZONE, McuProtocol.COMMAND_SET_BUZZER,
                    McuProtocol.COMMAND_SET_INDICATOR)) return null
            val actual = McuProtocol.readInt32(p, 4)
            val target = McuProtocol.readInt32(p, 8)
            val actualSpeed = McuProtocol.readInt32(p, 12)
            val targetSpeed = McuProtocol.readInt32(p, 16)
            if (controlled in setOf(McuProtocol.COMMAND_SET_LIGHT_MODE, McuProtocol.COMMAND_SET_BUZZER) &&
                (actual !in 0..255 || target !in 0..255 || actualSpeed !in 0..100 || targetSpeed !in 0..100)) return null
            if (controlled == McuProtocol.COMMAND_SET_LIGHT_ZONE &&
                (actual !in 0..1 || target !in 0..1 || actualSpeed !in 0..100 || targetSpeed !in 0..100)) return null
            if (controlled == McuProtocol.COMMAND_SET_INDICATOR &&
                (actual !in 0..1 || target !in 0..1 || actualSpeed != 0 || targetSpeed != 0)) return null
            return McuExtendedStatus(McuProtocol.unsigned(p[0]), McuProtocol.unsigned(p[1]),
                McuProtocol.unsigned(p[2]), McuProtocol.readInt32(p, 4), McuProtocol.readInt32(p, 8),
                McuProtocol.readInt32(p, 12), McuProtocol.readInt32(p, 16))
        }
    }
}

class McuFrameDecoder {
    private var buffer = byteArrayOf()

    fun reset() {
        buffer = byteArrayOf()
    }

    fun feed(chunk: ByteArray): List<McuFrame> {
        if (chunk.isEmpty()) return emptyList()
        buffer += chunk
        val frames = mutableListOf<McuFrame>()

        while (true) {
            val start = findStart(buffer)
            if (start < 0) {
                buffer = if (buffer.lastOrNull()?.toInt()?.and(0xFF) == McuProtocol.SOF_1) {
                    byteArrayOf(buffer.last())
                } else {
                    byteArrayOf()
                }
                break
            }
            if (start > 0) buffer = buffer.copyOfRange(start, buffer.size)
            if (buffer.size < McuProtocol.FRAME_OVERHEAD) break

            val payloadLength = McuProtocol.unsigned(buffer[6]) or
                (McuProtocol.unsigned(buffer[7]) shl 8)
            if (payloadLength > McuProtocol.MAX_PAYLOAD_LENGTH) {
                buffer = buffer.copyOfRange(1, buffer.size)
                continue
            }
            val frameLength = McuProtocol.FRAME_OVERHEAD + payloadLength
            if (buffer.size < frameLength) break

            val expectedCrc = McuProtocol.unsigned(buffer[frameLength - 2]) or
                (McuProtocol.unsigned(buffer[frameLength - 1]) shl 8)
            val actualCrc = McuProtocol.crc16Modbus(buffer, 2, 6 + payloadLength)
            if (actualCrc != expectedCrc) {
                buffer = buffer.copyOfRange(1, buffer.size)
                continue
            }

            frames += McuFrame(
                version = McuProtocol.unsigned(buffer[2]),
                flags = McuProtocol.unsigned(buffer[3]),
                sequence = McuProtocol.unsigned(buffer[4]),
                command = McuProtocol.unsigned(buffer[5]),
                payload = buffer.copyOfRange(
                    McuProtocol.HEADER_LENGTH,
                    McuProtocol.HEADER_LENGTH + payloadLength
                )
            )
            buffer = buffer.copyOfRange(frameLength, buffer.size)
        }
        return frames
    }

    private fun findStart(data: ByteArray): Int {
        for (index in 0 until data.lastIndex) {
            if (McuProtocol.unsigned(data[index]) == McuProtocol.SOF_1 &&
                McuProtocol.unsigned(data[index + 1]) == McuProtocol.SOF_2
            ) {
                return index
            }
        }
        return -1
    }
}
