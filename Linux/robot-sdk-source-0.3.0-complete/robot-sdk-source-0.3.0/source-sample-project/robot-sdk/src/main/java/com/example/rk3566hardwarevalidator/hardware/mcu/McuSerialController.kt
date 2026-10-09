package com.example.rk3566hardwarevalidator.hardware.mcu

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.core.content.ContextCompat
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.util.SerialInputOutputManager
import java.io.IOException
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import java.util.concurrent.LinkedBlockingDeque
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.atomic.AtomicBoolean

sealed interface McuConnectionState {
    data class Scanning(
        val attachedUsbDeviceCount: Int,
        val supportedSerialDeviceCount: Int
    ) : McuConnectionState
    data class Disconnected(val reason: String) : McuConnectionState
    data class AwaitingPermission(val deviceName: String) : McuConnectionState
    data class Connected(val deviceName: String, val baudRate: Int) : McuConnectionState
    data class Error(val message: String) : McuConnectionState
}

/** Controls one USB-UART device. Camera and microphone USB devices are ignored by the serial prober. */
class McuSerialController(
    context: Context,
    private val onConnectionStateChanged: (McuConnectionState) -> Unit,
    private val onStatusChanged: (McuStatus) -> Unit,
    private val onProtocolMessage: (String) -> Unit,
    private val onCommandResult: (McuCommandResult) -> Unit = {},
    val capabilities: McuCapabilities = McuCapabilities.LEGACY_V2,
    val safetyOptions: McuSafetyOptions = McuSafetyOptions(),
    private val onExtendedStatusChanged: (McuExtendedStatus) -> Unit = {}
) : SerialInputOutputManager.Listener, AutoCloseable {

    private val appContext = context.applicationContext
    private val usbManager = appContext.getSystemService(Context.USB_SERVICE) as UsbManager
    private val mainHandler = Handler(Looper.getMainLooper())
    private val serialQueue = LinkedBlockingDeque<Runnable>()
    private val serialExecutor = ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, serialQueue) {
        runnable -> Thread(runnable, "McuSerial")
    }.apply { prestartCoreThread() }
    private val scheduler: ScheduledExecutorService =
        Executors.newSingleThreadScheduledExecutor { runnable ->
            Thread(runnable, "McuSerialMaintenance")
        }
    private val decoder = McuFrameDecoder()
    private val started = AtomicBoolean(false)
    private val closed = AtomicBoolean(false)
    private val tracker = McuCommandTracker(capabilities, safetyOptions,
        clock = { SystemClock.elapsedRealtime() }, onResult = ::publishCommandResult)

    private var activeDriver: UsbSerialDriver? = null
    private var serialPort: UsbSerialPort? = null
    private var ioManager: SerialInputOutputManager? = null
    private var lastStatusRequestAt = 0L
    private var lastDiscoveryAttemptAt = 0L
    private var pendingPermissionDeviceId: Int? = null
    private var permissionDeniedDeviceId: Int? = null
    private var lastPublishedConnectionState: McuConnectionState? = null

    private val permissionIntent: PendingIntent by lazy {
        PendingIntent.getBroadcast(
            appContext,
            0,
            Intent(ACTION_USB_PERMISSION).setPackage(appContext.packageName),
            PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
    }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION -> {
                    val device = intent.usbDeviceExtra()
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    if (device == null) return
                    enqueue {
                        pendingPermissionDeviceId = null
                        if (granted) {
                            permissionDeniedDeviceId = null
                            val driver = findDrivers().firstOrNull {
                                it.device.deviceId == device.deviceId
                            }
                            if (driver == null) {
                                publishState(McuConnectionState.Error("已授权设备不是受支持的 USB 串口"))
                            } else {
                                openDriver(driver)
                            }
                        } else {
                            permissionDeniedDeviceId = device.deviceId
                            publishState(
                                McuConnectionState.Disconnected(
                                    "USB 串口授权被拒绝，点击“刷新设备”可再次授权"
                                )
                            )
                        }
                    }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    val attached = intent.usbDeviceExtra()
                    enqueue {
                        if (attached?.deviceId == permissionDeniedDeviceId) {
                            permissionDeniedDeviceId = null
                        }
                        discoverAndOpen(reportScan = true)
                    }
                }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    val detached = intent.usbDeviceExtra()
                    enqueue {
                        if (detached?.deviceId == pendingPermissionDeviceId) {
                            pendingPermissionDeviceId = null
                        }
                        if (detached?.deviceId == permissionDeniedDeviceId) {
                            permissionDeniedDeviceId = null
                        }
                        if (detached?.deviceId == activeDriver?.device?.deviceId) {
                            closeTransport()
                            publishState(McuConnectionState.Disconnected("USB 串口已拔出"))
                            discoverAndOpen(reportScan = true)
                        } else if (serialPort == null) {
                            discoverAndOpen(reportScan = true)
                        }
                    }
                }
            }
        }
    }

    fun start() {
        if (!started.compareAndSet(false, true) || closed.get()) return
        val filter = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        ContextCompat.registerReceiver(
            appContext,
            usbReceiver,
            filter,
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
        enqueue { discoverAndOpen(reportScan = true) }
        scheduler.scheduleWithFixedDelay(
            { enqueue { runMaintenance() } },
            MAINTENANCE_INTERVAL_MS,
            MAINTENANCE_INTERVAL_MS,
            TimeUnit.MILLISECONDS
        )
    }

    fun setLight(enabled: Boolean, brightnessPercent: Int): McuCommandSubmission =
        dispatch(tracker.submitLight(enabled, brightnessPercent))

    fun setMotion(motion: McuMotion, speedPercent: Int = McuProtocol.DEFAULT_MOTION_SPEED): McuCommandSubmission =
        dispatch(tracker.submitMotion(motion, speedPercent), priority = motion == McuMotion.STOP)

    fun stopMotion(): McuCommandSubmission = setMotion(McuMotion.STOP, 0)

    /** Lifecycle pause never resumes motion automatically. */
    fun pause(): McuCommandSubmission = stopMotion()

    fun setLightMode(modeId: Int, brightnessPercent: Int = 100): McuCommandSubmission =
        dispatch(tracker.submitLightMode(modeId, brightnessPercent))

    fun setArmPosition(jointId: Int, positionMilliDegrees: Int,
                       speedMilliDegreesPerSecond: Int): McuCommandSubmission =
        dispatch(tracker.submitArmPosition(jointId, positionMilliDegrees, speedMilliDegreesPerSecond))

    fun isStatusStale(): Boolean = tracker.isStatusStale()

    val lastStatus: McuStatus? get() = tracker.lastStatus

    fun requestExtendedStatus(commandType: McuCommandType, selector: Int): McuStatusQuerySubmission {
        val query = tracker.prepareExtendedStatusQuery(commandType, selector)
        query.frame?.let { frame -> enqueue { sendUntrackedFrame(frame) } }
        return query.submission
    }

    fun jointSnapshots(): List<McuJointSnapshot> = tracker.jointSnapshots()

    fun extendedStatusSnapshot(commandType: McuCommandType, selector: Int): McuExtendedStatus? =
        tracker.extendedStatusSnapshot(commandType, selector)

    fun homeArm(jointId: Int): McuCommandSubmission = dispatch(tracker.submitArmHome(jointId))

    fun setLightZone(zoneId: Int, enabled: Boolean, brightnessPercent: Int): McuCommandSubmission =
        dispatch(tracker.submitLightZone(zoneId, enabled, brightnessPercent))

    fun setBuzzer(patternId: Int, volumePercent: Int = 100): McuCommandSubmission =
        dispatch(tracker.submitBuzzer(patternId, volumePercent))

    fun setIndicator(indicatorId: Int, enabled: Boolean): McuCommandSubmission =
        dispatch(tracker.submitIndicator(indicatorId, enabled))

    /** MCU firmware must stop all actuators before power removal. ACK is never proof of power-off. */
    fun requestSoftShutdown(): McuCommandSubmission = dispatch(tracker.submitSoftShutdown(), priority = true)

    fun requestStatus() {
        enqueue { sendStatusRequest() }
    }

    /** Re-enumerates USB devices and reconnects even when the previous transport is stale. */
    fun refreshDevices() {
        if (closed.get() || !started.get()) return
        enqueue {
            closeTransport()
            pendingPermissionDeviceId = null
            permissionDeniedDeviceId = null
            discoverAndOpen(reportScan = true)
        }
    }

    override fun onNewData(data: ByteArray) {
        val copy = data.copyOf()
        enqueue {
            decoder.feed(copy).forEach(::handleFrame)
        }
    }

    override fun onRunError(error: Exception) {
        enqueue {
            closeTransport()
            lastDiscoveryAttemptAt = 0L
            publishState(
                McuConnectionState.Error(
                    "USB 串口读取失败：${error.message ?: error.javaClass.simpleName}"
                )
            )
        }
    }

    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        // Invalidate queued commands synchronously before the IO thread gets another write.
        tracker.close()
        if (started.get()) runCatching { appContext.unregisterReceiver(usbReceiver) }
        scheduler.shutdownNow()
        val cleanup = Runnable {
            // A best-effort transport STOP; no claim of acknowledgement or physical stop.
            runCatching { serialPort?.write(McuProtocol.encode(McuProtocol.setMotion(0, McuMotion.STOP, 0)), WRITE_TIMEOUT_MS) }
            closeTransport()
        }
        serialQueue.addFirst(cleanup)
        serialExecutor.shutdown()
    }

    private fun discoverAndOpen(reportScan: Boolean = false) {
        if (closed.get() || serialPort != null) return
        lastDiscoveryAttemptAt = SystemClock.elapsedRealtime()
        val drivers = findDrivers()
        if (reportScan) {
            publishState(
                McuConnectionState.Scanning(
                    attachedUsbDeviceCount = usbManager.deviceList.size,
                    supportedSerialDeviceCount = drivers.size
                )
            )
        }
        if (drivers.isEmpty()) {
            val attachedCount = usbManager.deviceList.size
            val reason = if (attachedCount == 0) {
                "未检测到 USB 设备"
            } else {
                "检测到 $attachedCount 个 USB 设备，但没有受支持的 USB 串口"
            }
            publishState(McuConnectionState.Disconnected(reason))
            return
        }
        val driver = drivers.first()
        if (!usbManager.hasPermission(driver.device)) {
            if (permissionDeniedDeviceId == driver.device.deviceId) {
                publishState(
                    McuConnectionState.Disconnected(
                        "USB 串口授权被拒绝，点击“刷新设备”可再次授权"
                    )
                )
                return
            }
            publishState(McuConnectionState.AwaitingPermission(deviceLabel(driver.device)))
            if (pendingPermissionDeviceId != driver.device.deviceId) {
                pendingPermissionDeviceId = driver.device.deviceId
                usbManager.requestPermission(driver.device, permissionIntent)
            }
            return
        }
        pendingPermissionDeviceId = null
        openDriver(driver)
    }

    private fun findDrivers(): List<UsbSerialDriver> =
        UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)

    private fun openDriver(driver: UsbSerialDriver) {
        if (closed.get() || serialPort != null) return
        val connection = usbManager.openDevice(driver.device)
        if (connection == null) {
            publishState(McuConnectionState.Error("无法打开 USB 串口，请重新插拔设备"))
            return
        }
        val port = driver.ports.firstOrNull()
        if (port == null) {
            connection.close()
            publishState(McuConnectionState.Error("USB 串口设备没有可用端口"))
            return
        }
        try {
            port.open(connection)
            port.setParameters(
                BAUD_RATE,
                8,
                UsbSerialPort.STOPBITS_1,
                UsbSerialPort.PARITY_NONE
            )
            runCatching { port.dtr = true }
            runCatching { port.rts = true }
            activeDriver = driver
            serialPort = port
            decoder.reset()
            tracker.setConnected(true)
            ioManager = SerialInputOutputManager(port, this).also { it.start() }
            publishState(McuConnectionState.Connected(deviceLabel(driver.device), BAUD_RATE))
            sendStatusRequest()
        } catch (error: Exception) {
            tracker.setConnected(false)
            runCatching { port.close() }
            connection.close()
            activeDriver = null
            serialPort = null
            publishState(
                McuConnectionState.Error(
                    "打开 USB 串口失败：${error.message ?: error.javaClass.simpleName}"
                )
            )
        }
    }

    private fun runMaintenance() {
        val now = SystemClock.elapsedRealtime()
        val port = serialPort
        if (port == null) {
            if (
                pendingPermissionDeviceId == null &&
                permissionDeniedDeviceId == null &&
                now - lastDiscoveryAttemptAt >= RECONNECT_INTERVAL_MS
            ) {
                discoverAndOpen()
            }
            return
        }
        tracker.maintenance().forEach(::sendDispatch)
        tracker.awaitingExtensionConfirmation().forEach { dispatch ->
            tracker.statusRequest(dispatch)?.let(::sendUntrackedFrame)
        }
        if (now - lastStatusRequestAt >= STATUS_INTERVAL_MS) sendStatusRequest()
    }

    private fun handleFrame(frame: McuFrame) {
        if (frame.version != McuProtocol.VERSION) {
            publishMessage("收到不支持的协议版本 ${frame.version}")
            return
        }
        when (frame.command) {
            McuProtocol.COMMAND_ACK -> {
                if (tracker.onAck(frame)) {
                    sendStatusRequest()
                    tracker.awaitingExtensionConfirmation().forEach {
                        tracker.statusRequest(it)?.let(::sendUntrackedFrame)
                    }
                }
            }
            McuProtocol.COMMAND_STATUS -> tracker.onStatus(frame)?.let(::publishStatus)
            McuProtocol.COMMAND_EXTENDED_STATUS -> tracker.onExtendedStatus(frame)?.let { status ->
                mainHandler.post { if (!closed.get()) onExtendedStatusChanged(status) }
            }
        }
    }

    private fun sendStatusRequest() {
        if (serialPort == null) return
        lastStatusRequestAt = SystemClock.elapsedRealtime()
        tracker.statusRequest()?.let(::sendUntrackedFrame)
    }

    private fun dispatch(command: McuCommandTracker.Dispatch, priority: Boolean = false): McuCommandSubmission {
        if (command.submission.accepted) enqueue(priority) { sendDispatch(command) }
        return command.submission
    }

    private fun sendDispatch(dispatch: McuCommandTracker.Dispatch) {
        // Epoch and pending membership are checked at execution, so a queued old FORWARD
        // cannot follow a newer STOP, even if it was accepted on another thread earlier.
        if (!tracker.canSend(dispatch)) return
        val port = serialPort ?: return
        try {
            port.write(McuProtocol.encode(dispatch.frame), WRITE_TIMEOUT_MS)
            tracker.markSent(dispatch)
        } catch (error: IOException) {
            tracker.writeFailed(dispatch)
            handleWriteFailure(error)
        }
    }

    private fun sendUntrackedFrame(frame: McuFrame) {
        if (closed.get()) return
        try { serialPort?.write(McuProtocol.encode(frame), WRITE_TIMEOUT_MS) }
        catch (error: IOException) { handleWriteFailure(error) }
    }

    /** All late USB callbacks/public calls safely decline work after shutdown. */
    private fun enqueue(priority: Boolean = false, action: () -> Unit) {
        if (closed.get()) return
        val runnable = Runnable { if (!closed.get()) action() }
        try {
            if (priority) serialQueue.addFirst(runnable) else serialExecutor.execute(runnable)
        } catch (_: RejectedExecutionException) { /* close raced with submission */ }
    }

    private fun handleWriteFailure(error: IOException) {
        closeTransport()
        lastDiscoveryAttemptAt = 0L
        publishState(
            McuConnectionState.Error(
                "USB 串口写入失败：${error.message ?: error.javaClass.simpleName}"
            )
        )
    }

    private fun closeTransport() {
        tracker.setConnected(false)
        ioManager?.let { manager ->
            runCatching { manager.stop() }
            runCatching { manager.listener = null }
        }
        ioManager = null
        runCatching { serialPort?.close() }
        serialPort = null
        activeDriver = null
        decoder.reset()
    }

    private fun publishState(state: McuConnectionState) {
        if (state == lastPublishedConnectionState) return
        lastPublishedConnectionState = state
        Log.i(TAG, "Connection state: $state")
        mainHandler.post { if (!closed.get()) onConnectionStateChanged(state) }
    }

    private fun publishStatus(status: McuStatus) {
        mainHandler.post { if (!closed.get()) onStatusChanged(status) }
    }

    private fun publishMessage(message: String) {
        mainHandler.post { if (!closed.get()) onProtocolMessage(message) }
    }

    private fun publishCommandResult(result: McuCommandResult) {
        // Final CLOSED results must remain observable during lifecycle cleanup.
        mainHandler.post { onCommandResult(result) }
    }

    private fun deviceLabel(device: UsbDevice): String {
        val name = device.productName?.takeIf { it.isNotBlank() } ?: "USB 串口"
        return "$name (${device.vendorId.toString(16).padStart(4, '0')}:" +
            "${device.productId.toString(16).padStart(4, '0')})"
    }

    @Suppress("DEPRECATION")
    private fun Intent.usbDeviceExtra(): UsbDevice? =
        getParcelableExtra(UsbManager.EXTRA_DEVICE) as? UsbDevice

    private companion object {
        const val TAG = "McuSerial"
        const val ACTION_USB_PERMISSION =
            "com.example.rk3566hardwarevalidator.USB_PERMISSION"
        const val BAUD_RATE = 115_200
        const val WRITE_TIMEOUT_MS = 1_000
        const val STATUS_INTERVAL_MS = 2_000L
        const val MAINTENANCE_INTERVAL_MS = 100L
        const val RECONNECT_INTERVAL_MS = 3_000L
    }
}
