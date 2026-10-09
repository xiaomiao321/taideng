package com.example.rk3566hardwarevalidator.hardware.camera

import org.junit.Assert.*
import org.junit.Test

class CameraOwnershipTest {
    @Test fun staleCloseCannotReleaseAnotherOwnersLease() {
        val first = Any()
        val second = Any()
        assertTrue(CameraOwnership.acquire("ownership-test", first))
        assertFalse(CameraOwnership.acquire("ownership-test", second))
        CameraOwnership.release("ownership-test", second)
        assertFalse(CameraOwnership.acquire("ownership-test", second))
        CameraOwnership.release("ownership-test", first)
        assertTrue(CameraOwnership.acquire("ownership-test", second))
        CameraOwnership.release("ownership-test", first)
        assertFalse(CameraOwnership.acquire("ownership-test", first))
        CameraOwnership.release("ownership-test", second)
    }
}
