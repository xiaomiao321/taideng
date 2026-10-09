package com.example.rk3566hardwarevalidator.hardware.photo

import android.content.Context
import android.os.Handler
import android.os.Looper
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

data class PhotoSearchSolution(
    val question: String,
    val answer: String,
    val steps: List<String>,
    val knowledgePoints: List<String>,
    val warning: String,
    val requestId: String?,
    val model: String?,
    val totalTokens: Int?
)

sealed class PhotoSearchState {
    object Idle : PhotoSearchState()
    data class Uploading(val imageBytes: Long) : PhotoSearchState()
    object Solving : PhotoSearchState()
    data class Success(val solution: PhotoSearchSolution) : PhotoSearchState()
    data class Error(val message: String) : PhotoSearchState()
}

class PhotoSearchClient(
    context: Context,
    private val onStateChanged: (PhotoSearchState) -> Unit
) : AutoCloseable {
    private val appContext = context.applicationContext
    private val mainHandler = Handler(Looper.getMainLooper())
    private val executor: ExecutorService = Executors.newSingleThreadExecutor { task ->
        Thread(task, "PhotoSearchWorker")
    }
    private val generation = AtomicInteger(0)
    @Volatile private var closed = false
    @Volatile private var activeConnection: HttpURLConnection? = null

    @Volatile
    var isBusy: Boolean = false
        private set

    @Synchronized
    fun solve(photo: File, endpoint: String): Boolean {
        if (closed || isBusy) return false
        isBusy = true
        val request = generation.incrementAndGet()
        emit(PhotoSearchState.Uploading(photo.length()), request)
        executor.execute {
            runCatching {
                require(photo.isFile && photo.length() > 0L) { "题目照片不存在或为空" }
                require(photo.length() <= MAX_IMAGE_BYTES) { "题目照片超过 8 MiB" }
                validateTransport(endpoint)
                uploadAndSolve(photo, endpoint, request)
            }.onSuccess { solution ->
                synchronized(this) {
                    if (!closed && generation.get() == request) {
                        isBusy = false
                        emit(PhotoSearchState.Success(solution), request)
                    }
                }
            }.onFailure { error ->
                synchronized(this) {
                    if (!closed && generation.get() == request) {
                        isBusy = false
                        val detail = error.message ?: error.javaClass.simpleName
                        emit(PhotoSearchState.Error("拍照搜题失败：$detail"), request)
                    }
                }
            }
        }
        return true
    }

    /** Discards in-flight results; server/provider billing may already have occurred. */
    @Synchronized
    fun cancel() {
        generation.incrementAndGet()
        isBusy = false
        activeConnection?.let { connection ->
            Thread({ runCatching { connection.disconnect() } }, "PhotoSearchCancellation").start()
        }
        activeConnection = null
        if (!closed) emit(PhotoSearchState.Idle)
    }

    @Synchronized
    override fun close() {
        if (closed) return
        closed = true
        cancel()
        executor.shutdownNow()
        mainHandler.removeCallbacksAndMessages(null)
    }

    private fun uploadAndSolve(photo: File, endpoint: String, request: Int): PhotoSearchSolution {
        val connection = (URL(endpoint).openConnection() as HttpURLConnection).apply {
            connectTimeout = CONNECT_TIMEOUT_MS
            readTimeout = READ_TIMEOUT_MS
            requestMethod = "POST"
            doOutput = true
            useCaches = false
            instanceFollowRedirects = false
            setFixedLengthStreamingMode(photo.length())
            setRequestProperty("Content-Type", "image/jpeg")
            setRequestProperty("Accept", "application/json")
            setRequestProperty("User-Agent", "AItaid-PhotoSearch")
        }
        synchronized(this) {
            check(!closed && generation.get() == request) { "请求已取消" }
            activeConnection = connection
        }
        try {
            connection.outputStream.buffered().use { output ->
                photo.inputStream().buffered().use { input -> input.copyTo(output) }
            }
            emit(PhotoSearchState.Solving, request)
            val responseCode = connection.responseCode
            val responseBody = readResponse(
                if (responseCode in 200..299) connection.inputStream else connection.errorStream
            )
            if (responseCode !in 200..299) {
                val serverMessage = runCatching {
                    JSONObject(responseBody).optString("error").trim()
                }.getOrNull().orEmpty()
                error(serverMessage.ifBlank { "服务器返回 HTTP $responseCode" })
            }
            return PhotoSearchSolutionParser.parse(responseBody)
        } finally {
            connection.disconnect()
            synchronized(this) { if (activeConnection === connection) activeConnection = null }
        }
    }

    private fun readResponse(input: java.io.InputStream?): String {
        if (input == null) return ""
        val output = ByteArrayOutputStream()
        input.use { stream ->
            val buffer = ByteArray(4096)
            while (true) {
                val count = stream.read(buffer)
                if (count < 0) break
                check(output.size() + count <= MAX_RESPONSE_BYTES) { "服务器响应超过 1 MiB" }
                output.write(buffer, 0, count)
            }
        }
        return output.toString(Charsets.UTF_8.name())
    }

    private fun validateTransport(endpoint: String) {
        val protocol = URL(endpoint).protocol.lowercase(Locale.US)
        check(protocol == "https" || (isDebuggable() && protocol == "http")) {
            "正式版本仅允许 HTTPS 搜题服务器地址"
        }
    }

    private fun isDebuggable(): Boolean =
        appContext.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE != 0

    private fun emit(state: PhotoSearchState, request: Int = generation.get()) {
        mainHandler.post { if (!closed && generation.get() == request) onStateChanged(state) }
    }

    private companion object {
        const val MAX_IMAGE_BYTES = 8L * 1024L * 1024L
        const val MAX_RESPONSE_BYTES = 1024 * 1024
        const val CONNECT_TIMEOUT_MS = 10_000
        const val READ_TIMEOUT_MS = 180_000
    }
}

object PhotoSearchSolutionParser {
    fun parse(json: String): PhotoSearchSolution {
        val root = JSONObject(json)
        require(root.optBoolean("success", false)) {
            root.optString("error").ifBlank { "搜题服务器未返回成功状态" }
        }
        val question = root.getString("question").trim()
        val answer = root.getString("answer").trim()
        require(question.isNotEmpty()) { "服务器返回的题目为空" }
        require(answer.isNotEmpty()) { "服务器返回的答案为空" }
        return PhotoSearchSolution(
            question = question,
            answer = answer,
            steps = root.optJSONArray("steps").toStringList(),
            knowledgePoints = root.optJSONArray("knowledge_points").toStringList(),
            warning = root.optString("warning").trim(),
            requestId = root.optString("request_id").trim().ifEmpty { null },
            model = root.optString("model").trim().ifEmpty { null },
            totalTokens = root.optJSONObject("usage")
                ?.optInt("total_tokens", -1)
                ?.takeIf { it >= 0 }
        )
    }

    private fun JSONArray?.toStringList(): List<String> {
        if (this == null) return emptyList()
        return buildList {
            repeat(length()) { index ->
                optString(index).trim().takeIf(String::isNotEmpty)?.let(::add)
            }
        }
    }
}
