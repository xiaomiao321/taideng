package com.example.rk3566hardwarevalidator.hardware.photo

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.min

/** A movable crop window; the same normalized bounds are used when encoding the JPEG. */
class QuestionCropOverlayView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {
    var region = PhotoCropRegion()
        private set
    private val shade = Paint().apply { color = 0x88000000.toInt() }
    private val border = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.STROKE
        strokeWidth = 2F * resources.displayMetrics.density
    }
    private val handle = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(77, 164, 255) }
    private val drawingRect = RectF()
    private var mode = -1
    private var startX = 0F
    private var startY = 0F
    private var initial = region

    init {
        isClickable = true
        contentDescription = "取题区域，拖动边角调整大小，拖动框内移动"
    }

    fun resetRegion() {
        region = PhotoCropRegion()
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val rect = drawingRect.apply { set(region.left * width, region.top * height, region.right * width, region.bottom * height) }
        canvas.drawRect(0F, 0F, width.toFloat(), rect.top, shade)
        canvas.drawRect(0F, rect.bottom, width.toFloat(), height.toFloat(), shade)
        canvas.drawRect(0F, rect.top, rect.left, rect.bottom, shade)
        canvas.drawRect(rect.right, rect.top, width.toFloat(), rect.bottom, shade)
        canvas.drawRect(rect, border)
        val radius = 7F * resources.displayMetrics.density
        canvas.drawCircle(rect.left, rect.top, radius, handle)
        canvas.drawCircle(rect.right, rect.top, radius, handle)
        canvas.drawCircle(rect.left, rect.bottom, radius, handle)
        canvas.drawCircle(rect.right, rect.bottom, radius, handle)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!isEnabled || width <= 0 || height <= 0) return false
        val x = event.x / width
        val y = event.y / height
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val corners = listOf(region.left to region.top, region.right to region.top,
                    region.left to region.bottom, region.right to region.bottom)
                val tolerance = 32F * resources.displayMetrics.density
                mode = corners.indexOfFirst { (cx, cy) ->
                    abs(event.x - cx * width) <= tolerance && abs(event.y - cy * height) <= tolerance
                }
                if (mode < 0 && x in region.left..region.right && y in region.top..region.bottom) mode = 4
                if (mode < 0) return false
                initial = region
                startX = x
                startY = y
                parent.requestDisallowInterceptTouchEvent(true)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (mode < 0) return false
                val minW = min(0.4F, 96F * resources.displayMetrics.density / width)
                val minH = min(0.4F, 96F * resources.displayMetrics.density / height)
                region = when (mode) {
                    0 -> initial.copy(left = x.coerceIn(0F, initial.right - minW), top = y.coerceIn(0F, initial.bottom - minH))
                    1 -> initial.copy(right = x.coerceIn(initial.left + minW, 1F), top = y.coerceIn(0F, initial.bottom - minH))
                    2 -> initial.copy(left = x.coerceIn(0F, initial.right - minW), bottom = y.coerceIn(initial.top + minH, 1F))
                    3 -> initial.copy(right = x.coerceIn(initial.left + minW, 1F), bottom = y.coerceIn(initial.top + minH, 1F))
                    else -> {
                        val dx = (x - startX).coerceIn(-initial.left, 1F - initial.right)
                        val dy = (y - startY).coerceIn(-initial.top, 1F - initial.bottom)
                        initial.copy(left = initial.left + dx, right = initial.right + dx,
                            top = initial.top + dy, bottom = initial.bottom + dy)
                    }
                }
                invalidate()
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (mode < 0) return false
                mode = -1
                parent.requestDisallowInterceptTouchEvent(false)
                if (event.actionMasked == MotionEvent.ACTION_UP) performClick()
                return true
            }
        }
        return mode >= 0
    }

    override fun performClick(): Boolean = super.performClick()
}
