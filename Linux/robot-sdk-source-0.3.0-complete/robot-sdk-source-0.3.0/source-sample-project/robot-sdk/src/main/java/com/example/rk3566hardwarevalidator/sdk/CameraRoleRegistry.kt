package com.example.rk3566hardwarevalidator.sdk

enum class CameraRole { POSTURE, QUESTION }
enum class CameraRoleStatus { BOUND, SESSION_BOUND, UNBOUND, MISSING, AMBIGUOUS, CONFLICT }

data class CameraResolution(val width: Int, val height: Int)

data class CameraDescriptor(
    val cameraId: String,
    val facing: Int? = null,
    val previewSizes: List<CameraResolution> = emptyList(),
    val analysisSizes: List<CameraResolution> = emptyList(),
    val fpsRanges: List<String> = emptyList(),
    /** Display/diagnostics only. Equal capabilities do not prove equal physical identity. */
    val capabilityFingerprint: String = "",
    /** Only supplied by an explicitly installed, verified identity provider. */
    val stableKey: String? = null
)

/**
 * Return a stable physical identity only when the vendor/host mapping is verified across boots.
 * Camera2 IDs, UsbDevice.deviceId and capability fingerprints alone MUST NOT be returned.
 * Android's public Camera2 API has no general UVC serial -> Camera2 ID mapping.
 */
fun interface CameraIdentityProvider {
    fun stableKey(descriptor: CameraDescriptor): String?
}

data class CameraRoleResolution(
    val role: CameraRole,
    val status: CameraRoleStatus,
    val cameraId: String? = null,
    val message: String = ""
)

/** Stores verified identities only; implementations must never store Camera2 or USB device IDs. */
interface CameraRoleBindingStore {
    fun load(): Map<CameraRole, String>
    fun save(bindings: Map<CameraRole, String>)
}

class InMemoryCameraRoleBindingStore : CameraRoleBindingStore {
    private var values: Map<CameraRole, String> = emptyMap()
    override fun load(): Map<CameraRole, String> = values.toMap()
    override fun save(bindings: Map<CameraRole, String>) { values = bindings.toMap() }
}

/** Pure resolver, usable in JVM tests and host provisioning tools. */
object CameraRoleResolver {
    fun resolve(
        descriptors: List<CameraDescriptor>,
        stableBindings: Map<CameraRole, String>,
        sessionBindings: Map<CameraRole, String> = emptyMap()
    ): Map<CameraRole, CameraRoleResolution> {
        val resolved = CameraRole.entries.associateWith { role ->
            val stableKey = stableBindings[role]
            val sessionId = sessionBindings[role]
            when {
                sessionId != null -> {
                    val candidates = descriptors.filter { it.cameraId == sessionId }
                    when (candidates.size) {
                        0 -> CameraRoleResolution(role, CameraRoleStatus.MISSING, message = "Confirmed session camera is missing")
                        1 -> CameraRoleResolution(role, CameraRoleStatus.SESSION_BOUND, sessionId, "Manually confirmed for this session")
                        else -> CameraRoleResolution(role, CameraRoleStatus.AMBIGUOUS, message = "Duplicate Camera2 IDs")
                    }
                }
                stableKey != null -> {
                    val candidates = descriptors.filter { it.stableKey == stableKey }
                    when (candidates.size) {
                        0 -> CameraRoleResolution(role, CameraRoleStatus.MISSING, message = "Bound physical camera is missing")
                        1 -> CameraRoleResolution(role, CameraRoleStatus.BOUND, candidates.single().cameraId, "Verified physical identity matched")
                        else -> CameraRoleResolution(role, CameraRoleStatus.AMBIGUOUS, message = "Stable identity matches multiple cameras")
                    }
                }
                descriptors.isEmpty() -> CameraRoleResolution(role, CameraRoleStatus.MISSING, message = "No cameras enumerated")
                descriptors.filter { it.stableKey == null && it.capabilityFingerprint.isNotBlank() }
                    .groupBy { it.capabilityFingerprint }.any { it.value.size > 1 } ->
                    CameraRoleResolution(role, CameraRoleStatus.AMBIGUOUS, message = "Identical cameras lack verified identities; confirm manually")
                else -> CameraRoleResolution(role, CameraRoleStatus.UNBOUND, message = "First setup requires manual confirmation")
            }
        }.toMutableMap()
        val duplicates = resolved.values.filter { it.cameraId != null }.groupBy { it.cameraId }
            .filterValues { it.size > 1 }
        duplicates.values.flatten().forEach { resolution ->
            resolved[resolution.role] = CameraRoleResolution(
                resolution.role, CameraRoleStatus.CONFLICT, message = "Both roles resolve to the same camera"
            )
        }
        return resolved
    }
}

/**
 * [refresh] creates a new enumeration session and invalidates temporary Camera2 ID confirmations.
 * The host must refresh after a topology change, then manually confirm unresolved roles.
 */
class CameraRoleRegistry(private val store: CameraRoleBindingStore = InMemoryCameraRoleBindingStore()) {
    private var descriptors: List<CameraDescriptor> = emptyList()
    private val stableBindings = store.load().filterValues { it.isNotBlank() }.toMutableMap()
    private val sessionBindings = mutableMapOf<CameraRole, String>()

    @Synchronized
    fun refresh(descriptors: List<CameraDescriptor>): Map<CameraRole, CameraRoleResolution> {
        this.descriptors = descriptors.toList()
        sessionBindings.clear()
        return resolutions()
    }

    @Synchronized
    fun resolve(role: CameraRole): CameraRoleResolution = resolutions().getValue(role)

    /** Explicit user confirmation of an ID from the CURRENT enumeration, never auto-selected. */
    @Synchronized
    fun bind(role: CameraRole, cameraId: String): CameraRoleResolution {
        val matching = descriptors.filter { it.cameraId == cameraId }
        if (matching.isEmpty()) return CameraRoleResolution(role, CameraRoleStatus.MISSING, message = "Camera ID is not in the current enumeration")
        if (matching.size != 1) return CameraRoleResolution(role, CameraRoleStatus.AMBIGUOUS, message = "Duplicate Camera2 IDs")
        val descriptor = matching.single()
        val other = CameraRole.entries.first { it != role }
        if (resolve(other).cameraId == cameraId ||
            (descriptor.stableKey != null && stableBindings[other] == descriptor.stableKey)) {
            return CameraRoleResolution(role, CameraRoleStatus.CONFLICT, message = "Camera is already bound to $other")
        }
        val key = descriptor.stableKey?.takeIf { it.isNotBlank() }
        if (key != null && descriptors.count { it.stableKey == key } != 1) {
            return CameraRoleResolution(role, CameraRoleStatus.AMBIGUOUS, message = "Stable identity matches multiple cameras")
        }
        if (key == null) {
            stableBindings.remove(role)
            sessionBindings[role] = cameraId
        } else {
            stableBindings[role] = key
            sessionBindings.remove(role)
        }
        store.save(stableBindings.toMap())
        return resolve(role)
    }

    @Synchronized
    fun unbind(role: CameraRole) {
        stableBindings.remove(role)
        sessionBindings.remove(role)
        store.save(stableBindings.toMap())
    }

    private fun resolutions() = CameraRoleResolver.resolve(descriptors, stableBindings, sessionBindings)
}
