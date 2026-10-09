package com.example.rk3566hardwarevalidator.hardware.pose

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View

/** Transparent overlay that draws the COCO-17 skeleton returned by the NPU. */
class PoseOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {
    private val linePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(0, 230, 255)
        strokeWidth = 5F * resources.displayMetrics.density
        strokeCap = Paint.Cap.ROUND
        style = Paint.Style.STROKE
    }
    private val pointPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(255, 214, 10)
        style = Paint.Style.FILL
    }

    @Volatile
    private var poseResult: PoseResult? = null

    fun updateResult(result: PoseResult?) {
        poseResult = result
        postInvalidateOnAnimation()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val keypoints = poseResult?.keypoints.orEmpty()
        if (keypoints.isEmpty()) return

        val byPart = keypoints.associateBy(PoseKeypoint::bodyPart)
        skeletonEdges.forEach { (startPart, endPart) ->
            val start = byPart[startPart] ?: return@forEach
            val end = byPart[endPart] ?: return@forEach
            if (start.confidence >= KEYPOINT_THRESHOLD && end.confidence >= KEYPOINT_THRESHOLD) {
                canvas.drawLine(
                    start.normalizedX * width,
                    start.normalizedY * height,
                    end.normalizedX * width,
                    end.normalizedY * height,
                    linePaint
                )
            }
        }

        val radius = 4.5F * resources.displayMetrics.density
        keypoints.forEach { keypoint ->
            if (keypoint.confidence >= KEYPOINT_THRESHOLD) {
                canvas.drawCircle(
                    keypoint.normalizedX * width,
                    keypoint.normalizedY * height,
                    radius,
                    pointPaint
                )
            }
        }
    }

    private companion object {
        const val KEYPOINT_THRESHOLD = 0.25F

        val skeletonEdges = listOf(
            BodyPart.NOSE to BodyPart.LEFT_EYE,
            BodyPart.NOSE to BodyPart.RIGHT_EYE,
            BodyPart.LEFT_EYE to BodyPart.LEFT_EAR,
            BodyPart.RIGHT_EYE to BodyPart.RIGHT_EAR,
            BodyPart.LEFT_SHOULDER to BodyPart.RIGHT_SHOULDER,
            BodyPart.LEFT_SHOULDER to BodyPart.LEFT_ELBOW,
            BodyPart.LEFT_ELBOW to BodyPart.LEFT_WRIST,
            BodyPart.RIGHT_SHOULDER to BodyPart.RIGHT_ELBOW,
            BodyPart.RIGHT_ELBOW to BodyPart.RIGHT_WRIST,
            BodyPart.LEFT_SHOULDER to BodyPart.LEFT_HIP,
            BodyPart.RIGHT_SHOULDER to BodyPart.RIGHT_HIP,
            BodyPart.LEFT_HIP to BodyPart.RIGHT_HIP,
            BodyPart.LEFT_HIP to BodyPart.LEFT_KNEE,
            BodyPart.LEFT_KNEE to BodyPart.LEFT_ANKLE,
            BodyPart.RIGHT_HIP to BodyPart.RIGHT_KNEE,
            BodyPart.RIGHT_KNEE to BodyPart.RIGHT_ANKLE
        )
    }
}
