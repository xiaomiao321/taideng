package com.example.rk3566hardwarevalidator.sdk

import android.content.Context
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import java.security.MessageDigest

/** Camera2 enumeration contains only IDs the HAL exposes, not all attached USB devices. */
class CameraInventory(context: Context, private val identityProvider: CameraIdentityProvider? = null) {
    private val manager = context.applicationContext.getSystemService(CameraManager::class.java)

    fun enumerate(): List<CameraDescriptor> = manager.cameraIdList.map { id ->
        val info = manager.getCameraCharacteristics(id)
        val streams = info.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val preview = streams?.getOutputSizes(SurfaceTexture::class.java).orEmpty()
            .map { CameraResolution(it.width, it.height) }.sortedWith(compareBy({ it.width }, { it.height }))
        val analysis = streams?.getOutputSizes(ImageFormat.YUV_420_888).orEmpty()
            .map { CameraResolution(it.width, it.height) }.sortedWith(compareBy({ it.width }, { it.height }))
        val facing = info.get(CameraCharacteristics.LENS_FACING)
        val fps = info.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES).orEmpty()
            .map { "${it.lower}-${it.upper}" }.sorted()
        val diagnostic = "$facing|$preview|$analysis|$fps|${info.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)}"
        val fingerprint = MessageDigest.getInstance("SHA-256").digest(diagnostic.toByteArray())
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        val descriptor = CameraDescriptor(id, facing, preview, analysis, fps, fingerprint)
        descriptor.copy(stableKey = identityProvider?.stableKey(descriptor)?.takeIf { it.isNotBlank() })
    }
}

class SharedPreferencesCameraRoleBindingStore(
    context: Context,
    preferencesName: String = "robot_sdk_camera_roles"
) : CameraRoleBindingStore {
    private val preferences = context.applicationContext.getSharedPreferences(preferencesName, Context.MODE_PRIVATE)
    override fun load(): Map<CameraRole, String> = CameraRole.entries.mapNotNull { role ->
        preferences.getString("stable_identity_${role.name}", null)?.let { role to it }
    }.toMap()
    override fun save(bindings: Map<CameraRole, String>) {
        preferences.edit().apply {
            CameraRole.entries.forEach { role ->
                val key = "stable_identity_${role.name}"
                bindings[role]?.let { putString(key, it) } ?: remove(key)
            }
        }.apply()
    }
}
