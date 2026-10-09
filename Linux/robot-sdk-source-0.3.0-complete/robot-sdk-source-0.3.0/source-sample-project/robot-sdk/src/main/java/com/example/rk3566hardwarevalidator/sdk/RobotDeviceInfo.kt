package com.example.rk3566hardwarevalidator.sdk

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.usb.UsbManager
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.SystemClock
import org.json.JSONArray
import org.json.JSONObject

data class UsbDeviceDescriptor(val sessionDeviceId: Int, val vendorId: Int, val productId: Int, val permissionGranted: Boolean)
data class UsbAudioDescriptor(val sessionDeviceId: Int, val type: Int, val name: String, val input: Boolean, val output: Boolean)

/** Read-only hardware/permission metadata. Feature flags don't prove radio enabled or peer connected. */
data class RobotDeviceInfo(
    val sdkVersion: String,
    val observedAtUnixMs: Long,
    val observedAtElapsedMs: Long,
    val manufacturer: String,
    val model: String,
    val androidVersion: String,
    val apiLevel: Int,
    val abis: List<String>,
    val cameraPermissionGranted: Boolean,
    val audioPermissionGranted: Boolean,
    val bluetoothClassicSupported: Boolean,
    val bluetoothLeSupported: Boolean,
    val usbDevices: List<UsbDeviceDescriptor>,
    val usbAudioDevices: List<UsbAudioDescriptor>
) {
    fun toJson(): String = JSONObject().put("schemaVersion", 1).put("sdkVersion", sdkVersion)
        .put("observedAtUnixMs", observedAtUnixMs).put("observedAtElapsedMs", observedAtElapsedMs)
        .put("manufacturer", manufacturer).put("model", model).put("androidVersion", androidVersion)
        .put("apiLevel", apiLevel).put("abis", JSONArray(abis))
        .put("cameraPermissionGranted", cameraPermissionGranted).put("audioPermissionGranted", audioPermissionGranted)
        .put("bluetoothClassicSupported", bluetoothClassicSupported).put("bluetoothLeSupported", bluetoothLeSupported)
        .put("usbDevices", JSONArray().apply { usbDevices.forEach { device ->
            put(JSONObject().put("sessionDeviceId", device.sessionDeviceId).put("vendorId", device.vendorId)
                .put("productId", device.productId).put("permissionGranted", device.permissionGranted))
        } }).put("usbAudioDevices", JSONArray().apply { usbAudioDevices.forEach { device ->
            put(JSONObject().put("sessionDeviceId", device.sessionDeviceId).put("type", device.type)
                .put("name", device.name).put("input", device.input).put("output", device.output))
        } }).toString()

    companion object {
        /** Does not request permission, read USB serial numbers, turn on Bluetooth or connect anything. */
        fun capture(context: Context): RobotDeviceInfo {
            val app = context.applicationContext
            val usb = app.getSystemService(UsbManager::class.java)
            val audio = app.getSystemService(AudioManager::class.java)
            return RobotDeviceInfo(RobotSdkInfo.VERSION, System.currentTimeMillis(), SystemClock.elapsedRealtime(),
                Build.MANUFACTURER, Build.MODEL, Build.VERSION.RELEASE, Build.VERSION.SDK_INT, Build.SUPPORTED_ABIS.toList(),
                app.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED,
                app.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED,
                app.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH),
                app.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE),
                usb?.deviceList?.values?.map { UsbDeviceDescriptor(it.deviceId, it.vendorId, it.productId, usb.hasPermission(it)) }.orEmpty(),
                (audio?.getDevices(AudioManager.GET_DEVICES_INPUTS).orEmpty().toList() +
                    audio?.getDevices(AudioManager.GET_DEVICES_OUTPUTS).orEmpty().toList()).distinctBy { it.id }.filter {
                    it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET
                }.map { UsbAudioDescriptor(it.id, it.type, it.productName?.toString().orEmpty(), it.isSource, it.isSink) })
        }
    }
}
