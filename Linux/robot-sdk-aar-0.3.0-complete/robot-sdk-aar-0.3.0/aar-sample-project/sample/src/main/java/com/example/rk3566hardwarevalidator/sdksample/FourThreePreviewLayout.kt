package com.example.rk3566hardwarevalidator.sdksample

import android.content.Context
import android.util.AttributeSet
import android.widget.FrameLayout

/** Keeps the sample preview at 4:3 on screens wider or narrower than the RK3566 panel. */
class FourThreePreviewLayout @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : FrameLayout(context, attrs, defStyleAttr) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val width = MeasureSpec.getSize(widthMeasureSpec)
        val height = (width * 3 + 2) / 4
        super.onMeasure(
            widthMeasureSpec,
            MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY)
        )
    }
}
