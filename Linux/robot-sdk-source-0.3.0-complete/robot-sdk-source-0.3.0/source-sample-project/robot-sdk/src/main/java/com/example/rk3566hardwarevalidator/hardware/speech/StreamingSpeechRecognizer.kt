package com.example.rk3566hardwarevalidator.hardware.speech

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.core.content.ContextCompat
import com.k2fsa.sherpa.onnx.EndpointConfig
import com.k2fsa.sherpa.onnx.EndpointRule
import com.k2fsa.sherpa.onnx.FeatureConfig
import com.k2fsa.sherpa.onnx.OnlineModelConfig
import com.k2fsa.sherpa.onnx.OnlineRecognizer
import com.k2fsa.sherpa.onnx.OnlineRecognizerConfig
import com.k2fsa.sherpa.onnx.OnlineTransducerModelConfig
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.log10
import kotlin.math.sqrt

enum class SpeechErrorCode { PERMISSION_DENIED, INPUT_UNAVAILABLE, AUDIO_UNSUPPORTED, AUDIO_FAILED, MODEL_FAILED, RECOGNITION_FAILED }

sealed interface SpeechRecognitionState {
    data object LoadingModel : SpeechRecognitionState
    data class Idle(
        val transcript: String = "",
        val modelLoadTimeMs: Long? = null
    ) : SpeechRecognitionState
    data class Listening(
        val transcript: String,
        val inputDevice: String,
        val inputLevelPercent: Int
    ) : SpeechRecognitionState
    data class Error(val message: String, val code: SpeechErrorCode = SpeechErrorCode.AUDIO_FAILED) : SpeechRecognitionState
}

/** One endpointed utterance. The timestamp is elapsed realtime, not wall-clock time. */
data class SpeechFinalTranscript(
    val text: String,
    val elapsedRealtimeMs: Long,
    val observedAtUnixMs: Long = System.currentTimeMillis()
) {
    fun toJson(): String = "{\"schemaVersion\":1,\"type\":\"SPEECH_FINAL\",\"text\":\"${escapeJson(text)}\",\"elapsedRealtimeMs\":$elapsedRealtimeMs,\"observedAtUnixMs\":$observedAtUnixMs}"
}

private fun escapeJson(value: String): String = buildString {
    value.forEach { ch -> when (ch) {
        '"' -> append("\\\"")
        '\\' -> append("\\\\")
        '\n' -> append("\\n")
        '\r' -> append("\\r")
        '\t' -> append("\\t")
        else -> if (ch.code < 32) append("\\u" + ch.code.toString(16).padStart(4, '0')) else append(ch)
    } }
}

/** Always-on bilingual ASR backed by a Zipformer transducer on RK3566's single NPU core. */
class StreamingSpeechRecognizer(
    context: Context,
    private val onStateChanged: (SpeechRecognitionState) -> Unit,
    private val onFinalTranscript: (SpeechFinalTranscript) -> Unit = {}
) : AutoCloseable {
    private val appContext = context.applicationContext
    private val audioManager = appContext.getSystemService(AudioManager::class.java)
    private val mainHandler = Handler(Looper.getMainLooper())
    private val modelExecutor = Executors.newSingleThreadExecutor { task ->
        Thread(task, "SpeechModelLoader")
    }
    private val recording = AtomicBoolean(false)
    private val recordingGeneration = AtomicInteger(0)
    private var restartRequested = false

    @Volatile
    private var recognizer: OnlineRecognizer? = null

    @Volatile
    private var audioRecord: AudioRecord? = null

    @Volatile
    private var recordingThread: Thread? = null

    @Volatile
    private var closed = false

    @Volatile
    private var suppressInputUntil = 0L

    /** Keep the microphone open, but discard our own chime and its short acoustic tail. */
    fun suppressInputFor(durationMillis: Long) {
        require(durationMillis in 0..60_000) { "Duration must be 0..60000 ms" }
        suppressInputUntil = SystemClock.elapsedRealtime() + durationMillis
    }

    init {
        initializeModel()
    }

    val isRecording: Boolean
        get() = recording.get()

    @SuppressLint("MissingPermission")
    @Synchronized
    fun start(): Boolean {
        if (closed || recording.get()) return false
        // stop() is asynchronous. Never let two streams share one native recognizer.
        if (recordingThread != null) { restartRequested = true; return false }
        if (ContextCompat.checkSelfPermission(appContext, Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            dispatch(SpeechRecognitionState.Error("没有录音权限", SpeechErrorCode.PERMISSION_DENIED))
            return false
        }
        val activeRecognizer = recognizer
        if (activeRecognizer == null) {
            dispatch(SpeechRecognitionState.LoadingModel)
            return false
        }
        val usbInput = findUsbInputDevice()
        if (usbInput == null) {
            dispatch(SpeechRecognitionState.Error("未发现 USB 摄像头麦克风，请检查连接", SpeechErrorCode.INPUT_UNAVAILABLE))
            return false
        }

        val minimumBufferBytes = AudioRecord.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        if (minimumBufferBytes <= 0) {
            dispatch(SpeechRecognitionState.Error("板卡不支持 16 kHz 单声道录音", SpeechErrorCode.AUDIO_UNSUPPORTED))
            return false
        }

        val recorder = try {
            AudioRecord.Builder()
                .setAudioSource(MediaRecorder.AudioSource.VOICE_RECOGNITION)
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                        .build()
                )
                .setBufferSizeInBytes(maxOf(minimumBufferBytes * 2, MINIMUM_RECORD_BUFFER_BYTES))
                .build()
        } catch (error: Throwable) {
            dispatch(SpeechRecognitionState.Error("创建录音器失败：${error.message.orEmpty()}"))
            return false
        }
        if (!recorder.setPreferredDevice(usbInput)) {
            recorder.release()
            dispatch(SpeechRecognitionState.Error("无法选择 USB 摄像头麦克风"))
            return false
        }
        if (recorder.state != AudioRecord.STATE_INITIALIZED) {
            recorder.release()
            dispatch(SpeechRecognitionState.Error("USB 摄像头麦克风初始化失败"))
            return false
        }

        return try {
            recorder.startRecording()
            if (recorder.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
                recorder.release()
                dispatch(SpeechRecognitionState.Error("USB 摄像头麦克风未进入录音状态"))
                false
            } else {
                val deviceName = usbInput.displayName()
                Log.i(TAG, "Using audio input: $deviceName (id=${usbInput.id})")
                audioRecord = recorder
                recording.set(true)
                val session = recordingGeneration.incrementAndGet()
                dispatch(SpeechRecognitionState.Listening("", deviceName, 0), session)
                recordingThread = Thread(
                    { processAudio(activeRecognizer, recorder, deviceName, session) },
                    "StreamingSpeechRecognition"
                ).also(Thread::start)
                true
            }
        } catch (error: Throwable) {
            recorder.release()
            dispatch(SpeechRecognitionState.Error("启动录音失败：${error.message.orEmpty()}"))
            false
        }
    }

    @Synchronized
    fun stop() {
        restartRequested = false
        recordingGeneration.incrementAndGet()
        recording.set(false)
        runCatching { audioRecord?.stop() }
    }

    @Synchronized
    override fun close() {
        if (closed) return
        closed = true
        stop()
        val activeThread = recordingThread
        val activeRecognizer = recognizer
        recognizer = null
        Thread({
            // Release only after decoding has really ended; a timeout followed by release is unsafe.
            runCatching { activeThread?.join() }
            runCatching { activeRecognizer?.release() }
        }, "SpeechRecognizerRelease").start()
        modelExecutor.shutdownNow()
    }

    private fun initializeModel() {
        dispatch(SpeechRecognitionState.LoadingModel)
        modelExecutor.execute {
            val startTime = SystemClock.elapsedRealtime()
            try {
                val modelConfig = OnlineModelConfig(
                    transducer = OnlineTransducerModelConfig(
                        encoder = "$MODEL_ASSET_DIRECTORY/encoder.rknn",
                        decoder = "$MODEL_ASSET_DIRECTORY/decoder.rknn",
                        joiner = "$MODEL_ASSET_DIRECTORY/joiner.rknn"
                    ),
                    tokens = "$MODEL_ASSET_DIRECTORY/tokens.txt",
                    // The patched RKNN provider uses 1 for runtime-default scheduling.
                    // RK3566 exposes a single NPU core shared with pose inference.
                    numThreads = 1,
                    provider = "rknn"
                )
                val config = OnlineRecognizerConfig(
                    featConfig = FeatureConfig(sampleRate = SAMPLE_RATE, featureDim = FEATURE_DIMENSION),
                    modelConfig = modelConfig,
                    endpointConfig = EndpointConfig(
                        rule1 = EndpointRule(false, 2.4F, 0F),
                        rule2 = EndpointRule(true, 1.0F, 0F),
                        rule3 = EndpointRule(false, 0F, 20F)
                    ),
                    enableEndpoint = true,
                    decodingMethod = "greedy_search"
                )
                val createdRecognizer = OnlineRecognizer(appContext.assets, config)
                synchronized(this) { if (closed) {
                    createdRecognizer.release()
                } else {
                    recognizer = createdRecognizer
                    val loadTimeMs = SystemClock.elapsedRealtime() - startTime
                    Log.i(TAG, "Speech model ready in $loadTimeMs ms")
                    dispatch(
                        SpeechRecognitionState.Idle(
                            modelLoadTimeMs = loadTimeMs
                        )
                    )
                } }
            } catch (error: Throwable) {
                if (!closed) {
                    Log.e(TAG, "Failed to initialize speech model", error)
                    dispatch(
                        SpeechRecognitionState.Error(
                            "语音模型初始化失败：${error.message ?: error.javaClass.simpleName}", SpeechErrorCode.MODEL_FAILED
                        )
                    )
                }
            }
        }
    }

    private fun processAudio(
        activeRecognizer: OnlineRecognizer,
        recorder: AudioRecord,
        deviceName: String,
        session: Int
    ) {
        var stream: com.k2fsa.sherpa.onnx.OnlineStream? = null
        val buffer = ShortArray(SAMPLES_PER_READ)
        val transcript = TranscriptAccumulator()
        var lastPublishTime = 0L
        var lastLoggedPartial = ""
        var failed = false
        var wasSuppressed = false
        try {
            val activeStream = activeRecognizer.createStream()
            stream = activeStream
            while (recording.get() && recordingGeneration.get() == session) {
                val samplesRead = recorder.read(buffer, 0, buffer.size)
                if (samplesRead <= 0) {
                    if (recording.get()) {
                        throw IllegalStateException("AudioRecord 读取失败：$samplesRead")
                    }
                    break
                }

                if (SystemClock.elapsedRealtime() < suppressInputUntil) {
                    if (!wasSuppressed) {
                        activeRecognizer.reset(activeStream)
                        lastLoggedPartial = ""
                        wasSuppressed = true
                    }
                    continue
                }
                wasSuppressed = false
                val samples = FloatArray(samplesRead) { index -> buffer[index] / 32768F }
                activeStream.acceptWaveform(samples, sampleRate = SAMPLE_RATE)
                while (activeRecognizer.isReady(activeStream)) {
                    activeRecognizer.decode(activeStream)
                }
                val endpoint = activeRecognizer.isEndpoint(activeStream)
                val text = activeRecognizer.getResult(activeStream).text.trim()
                if (text.isNotEmpty() && text != lastLoggedPartial) {
                    lastLoggedPartial = text
                    Log.d(TAG, "Partial result: $text")
                }
                val displayText = transcript.update(text, endpoint)
                if (endpoint) {
                    if (text.isNotEmpty()) {
                        Log.i(TAG, "Endpoint result: $text")
                        dispatchFinal(SpeechFinalTranscript(text, SystemClock.elapsedRealtime()), session)
                    }
                    lastLoggedPartial = ""
                    activeRecognizer.reset(activeStream)
                }

                val now = SystemClock.elapsedRealtime()
                if (endpoint || now - lastPublishTime >= STATE_UPDATE_INTERVAL_MS) {
                    lastPublishTime = now
                    dispatch(
                        SpeechRecognitionState.Listening(
                            transcript = displayText,
                            inputDevice = deviceName,
                            inputLevelPercent = AudioLevelMeter.percent(buffer, samplesRead)
                        ), session
                    )
                }
            }
        } catch (error: Throwable) {
            if (recording.get() && !closed) {
                failed = true
                Log.e(TAG, "Speech recognition failed", error)
                dispatch(
                    SpeechRecognitionState.Error(
                        "语音识别失败：${error.message ?: error.javaClass.simpleName}", SpeechErrorCode.RECOGNITION_FAILED
                    ), session
                )
            }
        } finally {
            recording.set(false)
            runCatching { stream?.release() }
            runCatching {
                if (recorder.recordingState == AudioRecord.RECORDSTATE_RECORDING) recorder.stop()
            }
            recorder.release()
            val cleanup = synchronized(this) {
                if (audioRecord === recorder) audioRecord = null
                recordingThread = null
                (restartRequested to recordingGeneration.get()).also { restartRequested = false }
            }
            val restart = cleanup.first
            val cleanupGeneration = cleanup.second
            if (restart && !closed) mainHandler.post {
                if (!closed && recordingGeneration.get() == cleanupGeneration) start()
            }
            if (!closed && !failed) {
                dispatch(SpeechRecognitionState.Idle(transcript.currentText()), cleanupGeneration)
            }
        }
    }

    private fun findUsbInputDevice(): AudioDeviceInfo? = audioManager
        .getDevices(AudioManager.GET_DEVICES_INPUTS)
        .firstOrNull { device ->
            device.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                device.type == AudioDeviceInfo.TYPE_USB_HEADSET
        }

    private fun AudioDeviceInfo.displayName(): String =
        productName?.toString()?.takeIf(String::isNotBlank)?.let { "USB：$it" }
            ?: "USB 麦克风"

    private fun dispatch(state: SpeechRecognitionState, session: Int? = null) {
        mainHandler.post {
            if (!closed && (session == null || recordingGeneration.get() == session)) onStateChanged(state)
        }
    }

    private fun dispatchFinal(result: SpeechFinalTranscript, session: Int) {
        mainHandler.post {
            if (!closed && recordingGeneration.get() == session) onFinalTranscript(result)
        }
    }

    private companion object {
        const val TAG = "StreamingSpeech"
        const val SAMPLE_RATE = 16_000
        const val FEATURE_DIMENSION = 80
        const val SAMPLES_PER_READ = SAMPLE_RATE / 10
        const val MINIMUM_RECORD_BUFFER_BYTES = SAMPLES_PER_READ * 2 * 4
        const val STATE_UPDATE_INTERVAL_MS = 200L
        const val MODEL_ASSET_DIRECTORY =
            "models/asr/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-rknn-2023-02-16"
    }
}

internal class TranscriptAccumulator {
    private val finalizedSegments = mutableListOf<String>()
    private var partialText = ""

    fun update(text: String, endpoint: Boolean): String {
        partialText = text.trim()
        if (endpoint && partialText.isNotEmpty()) {
            finalizedSegments += partialText
            while (finalizedSegments.size > MAX_RETAINED_SEGMENTS) {
                finalizedSegments.removeAt(0)
            }
            partialText = ""
        }
        return currentText()
    }

    fun currentText(): String = buildList {
        partialText.takeIf(String::isNotEmpty)?.let(::add)
        addAll(
            finalizedSegments.asReversed().take(MAX_DISPLAY_SEGMENTS - size)
        )
    }.joinToString(separator = "\n")

    private companion object {
        const val MAX_RETAINED_SEGMENTS = 20
        const val MAX_DISPLAY_SEGMENTS = 3
    }
}

internal object AudioLevelMeter {
    fun percent(samples: ShortArray, count: Int): Int {
        if (count <= 0) return 0
        var sumSquares = 0.0
        repeat(minOf(count, samples.size)) { index ->
            val normalized = samples[index] / 32768.0
            sumSquares += normalized * normalized
        }
        val rms = sqrt(sumSquares / minOf(count, samples.size))
        if (rms <= 0.000001) return 0
        val decibels = 20.0 * log10(rms)
        return (((decibels + 60.0) / 60.0) * 100.0).toInt().coerceIn(0, 100)
    }
}
