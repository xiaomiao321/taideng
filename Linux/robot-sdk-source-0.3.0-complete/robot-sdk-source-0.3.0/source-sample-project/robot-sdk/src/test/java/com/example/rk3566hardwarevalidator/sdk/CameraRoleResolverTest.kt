package com.example.rk3566hardwarevalidator.sdk

import org.junit.Assert.*
import org.junit.Test

class CameraRoleResolverTest {
    private fun camera(id: String, key: String? = null, fingerprint: String = "uvc") =
        CameraDescriptor(id, capabilityFingerprint = fingerprint, stableKey = key)

    @Test fun firstSetupNeverAutoAssignsEvenWithStableIdentities() {
        val result = CameraRoleResolver.resolve(listOf(camera("0", "serial-a"), camera("1", "serial-b")), emptyMap())
        assertTrue(result.values.all { it.status == CameraRoleStatus.UNBOUND && it.cameraId == null })
    }

    @Test fun coldBootAndChangedIdsFollowVerifiedIdentity() {
        val store = InMemoryCameraRoleBindingStore()
        val first = CameraRoleRegistry(store)
        first.refresh(listOf(camera("0", "serial-a"), camera("1", "serial-b")))
        assertEquals(CameraRoleStatus.BOUND, first.bind(CameraRole.POSTURE, "0").status)
        first.bind(CameraRole.QUESTION, "1")
        val boot = CameraRoleRegistry(store)
        boot.refresh(listOf(camera("7", "serial-b"), camera("9", "serial-a")))
        assertEquals("9", boot.resolve(CameraRole.POSTURE).cameraId)
        assertEquals("7", boot.resolve(CameraRole.QUESTION).cameraId)
    }

    @Test fun identicalCamerasWithoutSerialAreAmbiguousAndSessionOnly() {
        val store = InMemoryCameraRoleBindingStore()
        val registry = CameraRoleRegistry(store)
        registry.refresh(listOf(camera("0"), camera("1")))
        assertEquals(CameraRoleStatus.AMBIGUOUS, registry.resolve(CameraRole.POSTURE).status)
        assertEquals(CameraRoleStatus.SESSION_BOUND, registry.bind(CameraRole.POSTURE, "0").status)
        assertTrue(store.load().isEmpty())
        registry.refresh(listOf(camera("0"), camera("1")))
        assertEquals(CameraRoleStatus.AMBIGUOUS, registry.resolve(CameraRole.POSTURE).status)
    }

    @Test fun duplicateVerifiedIdentitiesAreNeverChosen() {
        val result = CameraRoleResolver.resolve(listOf(camera("0", "same"), camera("1", "same")), mapOf(CameraRole.POSTURE to "same"))
        assertEquals(CameraRoleStatus.AMBIGUOUS, result.getValue(CameraRole.POSTURE).status)
        assertNull(result.getValue(CameraRole.POSTURE).cameraId)
        val registry = CameraRoleRegistry()
        registry.refresh(listOf(camera("0", "same"), camera("1", "same")))
        assertEquals(CameraRoleStatus.AMBIGUOUS, registry.bind(CameraRole.POSTURE, "0").status)
    }

    @Test fun missingDeviceDoesNotUseRemainingCamera() {
        val result = CameraRoleResolver.resolve(listOf(camera("0", "serial-b")), mapOf(CameraRole.POSTURE to "serial-a", CameraRole.QUESTION to "serial-b"))
        assertEquals(CameraRoleStatus.MISSING, result.getValue(CameraRole.POSTURE).status)
        assertNull(result.getValue(CameraRole.POSTURE).cameraId)
        assertEquals("0", result.getValue(CameraRole.QUESTION).cameraId)
    }

    @Test fun bothRolesCannotUseSamePhysicalDevice() {
        val registry = CameraRoleRegistry()
        registry.refresh(listOf(camera("0", "serial-a")))
        registry.bind(CameraRole.POSTURE, "0")
        assertEquals(CameraRoleStatus.CONFLICT, registry.bind(CameraRole.QUESTION, "0").status)
        assertEquals("0", registry.resolve(CameraRole.POSTURE).cameraId)
        assertNull(registry.resolve(CameraRole.QUESTION).cameraId)
        val result = CameraRoleResolver.resolve(listOf(camera("5", "serial-a")), CameraRole.entries.associateWith { "serial-a" })
        assertTrue(result.values.all { it.status == CameraRoleStatus.CONFLICT && it.cameraId == null })
    }

    @Test fun missingOrDuplicateSessionIdsNeverBind() {
        val registry = CameraRoleRegistry()
        registry.refresh(listOf(camera("0"), camera("0")))
        assertEquals(CameraRoleStatus.AMBIGUOUS, registry.bind(CameraRole.POSTURE, "0").status)
        assertEquals(CameraRoleStatus.MISSING, registry.bind(CameraRole.QUESTION, "99").status)
    }

    @Test fun differentCapabilitiesDoNotBecomePersistentIdentities() {
        val store = InMemoryCameraRoleBindingStore()
        val registry = CameraRoleRegistry(store)
        registry.refresh(listOf(camera("0", fingerprint = "wide"), camera("1", fingerprint = "tele")))
        registry.bind(CameraRole.POSTURE, "0")
        val boot = CameraRoleRegistry(store)
        boot.refresh(listOf(camera("4", fingerprint = "wide"), camera("7", fingerprint = "tele")))
        assertEquals(CameraRoleStatus.UNBOUND, boot.resolve(CameraRole.POSTURE).status)
    }
}
