package com.example.rk3566hardwarevalidator.sdk

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.os.Looper
import android.util.AttributeSet
import android.view.Gravity
import android.view.TextureView
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView
import com.example.rk3566hardwarevalidator.hardware.pose.PoseOverlayView
import com.example.rk3566hardwarevalidator.hardware.posture.PostureDetectionState
import com.example.rk3566hardwarevalidator.hardware.posture.PostureIssue

/** Camera preview with the same live skeleton overlay used by the validator APK. */
class PosturePreviewView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : FrameLayout(context, attrs) {
    internal val cameraTexture = TextureView(context)
    private val skeletonOverlay = PoseOverlayView(context)
    private val statusLabel = TextView(context).apply {
        setTextColor(Color.WHITE)
        setBackgroundColor(0xAA000000.toInt())
        textSize = 16F
        val padding = (8F * resources.displayMetrics.density).toInt()
        setPadding(padding, padding, padding, padding)
        text = "等待相机"
    }

    init {
        setBackgroundColor(Color.BLACK)
        addView(cameraTexture, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        addView(skeletonOverlay, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        addView(statusLabel, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT, Gravity.BOTTOM))
    }

    internal fun render(observation: PostureObservation?) {
        skeletonOverlay.updateResult(observation?.pose)
        statusLabel.text = when {
            observation == null -> "等待识别"
            !observation.personDetected -> "未检测到人体"
            observation.posture == null -> "坐姿未判定"
            observation.posture.state == PostureDetectionState.NEEDS_POSITION -> "请调整到相机视野内"
            observation.posture.state == PostureDetectionState.OBSERVING -> "正在观察坐姿"
            observation.posture.issues.isNotEmpty() -> observation.posture.issues.joinToString("、") { issue ->
                when (issue) {
                    PostureIssue.UNEVEN_SHOULDERS -> "高低肩"
                    PostureIssue.HEAD_DOWN -> "低头"
                    PostureIssue.SLOUCHING -> "疑似驼背"
                }
            }
            observation.posture.readyChecks.size < PostureIssue.entries.size -> "已判定项正常"
            else -> "坐姿正常"
        }
    }

    internal fun showMessage(message: String) {
        statusLabel.text = message
    }

    /** Hide SDK diagnostics while the same camera preview is used for another feature. */
    fun setAnnotationsVisible(visible: Boolean) {
        skeletonOverlay.visibility = if (visible) View.VISIBLE else View.GONE
        statusLabel.visibility = if (visible) View.VISIBLE else View.GONE
    }

    /** A diagnostic snapshot of the preview *with* the current skeleton and label. Call on main. */
    fun captureAnnotatedBitmap(): Result<Bitmap> = runCatching {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Capture must be called on the main thread" }
        check(width > 0 && height > 0) { "预览尚未完成布局" }
        val source = cameraTexture.bitmap ?: error("相机预览尚不可用")
        val output = try {
            source.copy(Bitmap.Config.ARGB_8888, true) ?: error("无法复制预览图像")
        } finally {
            source.recycle()
        }
        try {
            val canvas = Canvas(output)
            canvas.save()
            canvas.scale(output.width.toFloat() / width, output.height.toFloat() / height)
            skeletonOverlay.draw(canvas)
            canvas.translate(statusLabel.left.toFloat(), statusLabel.top.toFloat())
            statusLabel.draw(canvas)
            canvas.restore()
            output
        } catch (error: Throwable) {
            output.recycle()
            throw error
        }
    }
}
