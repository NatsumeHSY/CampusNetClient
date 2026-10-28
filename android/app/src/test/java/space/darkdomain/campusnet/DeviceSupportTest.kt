package space.darkdomain.campusnet

import org.junit.Assert.*
import org.junit.Test

class DeviceSupportTest {
    @Test fun supportedOnlyOnAndroid12Arm() {
        assertTrue(DeviceSupport.isSupported(31, listOf("arm64-v8a")))
        assertTrue(DeviceSupport.isSupported(36, listOf("armeabi-v7a")))
        assertFalse(DeviceSupport.isSupported(30, listOf("arm64-v8a")))
        assertFalse(DeviceSupport.isSupported(36, listOf("x86_64")))
    }
}
