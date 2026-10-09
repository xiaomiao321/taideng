package com.example.rk3566hardwarevalidator.hardware.camera

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CameraStreamSelectorTest {
    @Test
    fun wideViewStillUsesValidatedRk3566Pair() {
        val selection = CameraStreamSelector.select(
            previewSizes = listOf(StreamDimensions(1280, 720), StreamDimensions(640, 480)),
            analysisSizes = listOf(StreamDimensions(640, 480)),
            viewWidth = 768,
            viewHeight = 426
        )

        assertEquals(StreamDimensions(640, 480), selection.preview)
        assertEquals(StreamDimensions(640, 480), selection.analysis)
    }

    @Test
    fun otherBoardUsesMatchingPreviewAndAnalysisSize() {
        val selection = CameraStreamSelector.select(
            previewSizes = listOf(StreamDimensions(800, 600), StreamDimensions(1280, 720)),
            analysisSizes = listOf(StreamDimensions(1280, 720)),
            viewWidth = 700,
            viewHeight = 400
        )

        assertEquals(StreamDimensions(1280, 720), selection.preview)
        assertEquals(selection.preview, selection.analysis)
    }

    @Test
    fun noSharedSizeKeepsPreviewWithoutRiskyYuvPair() {
        val selection = CameraStreamSelector.select(
            previewSizes = listOf(StreamDimensions(800, 600)),
            analysisSizes = listOf(StreamDimensions(640, 480)),
            viewWidth = 800,
            viewHeight = 600
        )

        assertEquals(StreamDimensions(800, 600), selection.preview)
        assertNull(selection.analysis)
    }

    @Test
    fun noPreviewSizesFallsBackToDiagnosticVga() {
        val selection = CameraStreamSelector.select(emptyList(), emptyList(), 0, 0)

        assertEquals(StreamDimensions(640, 480), selection.preview)
        assertNull(selection.analysis)
    }
}
