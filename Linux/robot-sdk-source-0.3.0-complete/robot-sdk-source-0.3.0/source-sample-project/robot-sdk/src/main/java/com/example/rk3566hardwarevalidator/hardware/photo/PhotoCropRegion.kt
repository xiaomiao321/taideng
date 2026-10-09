package com.example.rk3566hardwarevalidator.hardware.photo

import kotlin.math.ceil
import kotlin.math.floor

/** Coordinates relative to the displayed TextureView, not a sensor crop. */
data class PhotoCropRegion(
    val left: Float = 0.06F,
    val top: Float = 0.08F,
    val right: Float = 0.94F,
    val bottom: Float = 0.92F
) {
    init {
        require(left.isFinite() && top.isFinite() && right.isFinite() && bottom.isFinite())
        require(left >= 0F && top >= 0F && right <= 1F && bottom <= 1F)
        require(left < right && top < bottom)
    }

    fun pixels(width: Int, height: Int): CropPixels {
        require(width > 0 && height > 0)
        val x = floor(left * width).toInt().coerceIn(0, width - 1)
        val y = floor(top * height).toInt().coerceIn(0, height - 1)
        val endX = ceil(right * width).toInt().coerceIn(x + 1, width)
        val endY = ceil(bottom * height).toInt().coerceIn(y + 1, height)
        return CropPixels(x, y, endX - x, endY - y)
    }

    companion object {
        val FULL_FRAME = PhotoCropRegion(0F, 0F, 1F, 1F)
    }
}

data class CropPixels(val x: Int, val y: Int, val width: Int, val height: Int)
