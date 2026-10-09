package com.example.rk3566hardwarevalidator.hardware.speech

import org.junit.Assert.*
import org.junit.Test

class SpeechDataTest {
    @Test fun finalTextEscapesTransportCharactersAndPreservesChinese() {
        val json = SpeechFinalTranscript("你好\n\"灯\"\\\t\u0001", 12, 34).toJson()
        assertTrue(json.contains("你好\\n\\\"灯\\\"\\\\\\t\\u0001"))
        assertTrue(json.contains("\"elapsedRealtimeMs\":12"))
        assertTrue(json.contains("\"observedAtUnixMs\":34"))
    }

    @Test fun accumulatorKeepsRefreshingPartialAndFinal() {
        val accumulator = TranscriptAccumulator()
        assertEquals("第一句", accumulator.update("第一句", true))
        assertTrue(accumulator.update("第二", false).startsWith("第二\n"))
        assertTrue(accumulator.update("第二句", true).startsWith("第二句\n"))
        assertTrue(accumulator.update("第三", false).startsWith("第三\n"))
    }

    @Test fun silenceAndLoudInputsHaveBoundedLevel() {
        assertEquals(0, AudioLevelMeter.percent(ShortArray(10), 10))
        assertTrue(AudioLevelMeter.percent(ShortArray(10) { Short.MAX_VALUE }, 10) in 99..100)
    }
}
