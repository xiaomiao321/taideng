package com.example.rk3566hardwarevalidator.hardware.camera

import kotlin.math.abs

internal data class StreamDimensions(val width: Int, val height: Int)

internal data class CameraStreamSelection(
    val preview: StreamDimensions,
    val analysis: StreamDimensions?
)

/** Prefer a pair proven stable on the RK3566 USB Camera HAL over view aspect ratio. */
internal object CameraStreamSelector {
    private val stableRk3566Size = StreamDimensions(640, 480)

    fun select(
        previewSizes: List<StreamDimensions>,
        analysisSizes: List<StreamDimensions>,
        viewWidth: Int,
        viewHeight: Int
    ): CameraStreamSelection {
        if (previewSizes.isEmpty()) return CameraStreamSelection(stableRk3566Size, null)

        val validAnalysis = analysisSizes.filter {
            it.width > 0 && it.height > 0 && it.width % 2 == 0 && it.height % 2 == 0
        }
        if (stableRk3566Size in previewSizes && stableRk3566Size in validAnalysis) {
            return CameraStreamSelection(stableRk3566Size, stableRk3566Size)
        }

        val targetRatio = if (viewHeight <= 0) 4f / 3f else viewWidth.toFloat() / viewHeight
        fun score(size: StreamDimensions): Float =
            abs(size.width.toFloat() / size.height - targetRatio) * 10_000 +
                abs(size.width - viewWidth) + abs(size.height - viewHeight)

        val shared = previewSizes.filter { candidate ->
            candidate in validAnalysis && candidate.width <= 1280 && candidate.height <= 720
        }
        if (shared.isNotEmpty()) {
            val size = shared.minBy(::score)
            return CameraStreamSelection(size, size)
        }

        // Preview remains usable when the HAL exposes no shared YUV size.
        val preview = previewSizes.filter { it.width <= 1920 && it.height <= 1080 }
            .ifEmpty { previewSizes }
            .minBy(::score)
        return CameraStreamSelection(preview, null)
    }
}
