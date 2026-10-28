package space.darkdomain.campusnet

import org.junit.Assert.*
import org.junit.Test

class PolicyTest {
    @Test fun batteryIsFailClosedAndThirtyIsLow() {
        assertEquals(BatteryState.LOW, batteryState(30, 100))
        assertEquals(BatteryState.LOW, batteryState(29, 100))
        assertEquals(BatteryState.OK, batteryState(31, 100))
        assertEquals(BatteryState.UNKNOWN, batteryState(-1, 100))
        assertEquals(BatteryState.UNKNOWN, batteryState(1, 0))
    }

    @Test fun huaweiAloneIsNotHarmonyAndSignalsAreUncertain() {
        assertEquals(EnvironmentKind.EMUI, detectEnvironment("HUAWEI", "EMUI 12", "android", ""))
        assertEquals(EnvironmentKind.UNKNOWN, detectEnvironment("HUAWEI", "", "android", ""))
        assertEquals(EnvironmentKind.HARMONY, detectEnvironment("HUAWEI", "HarmonyOS 4", "", ""))
        assertEquals(EnvironmentKind.SUSPECTED_COMPATIBILITY, detectEnvironment("other", "", "droitong", ""))
        assertEquals(EnvironmentKind.UNKNOWN, detectEnvironment("other", "", "", "droitong"))
    }

    @Test fun wifiSelectionRejectsAmbiguityAndNonWifi() {
        assertEquals(null, selectOnlyWifi(listOf(NetworkCandidate("cell", false))))
        assertEquals("wifi", selectOnlyWifi(listOf(NetworkCandidate("cell", false), NetworkCandidate("wifi", true))))
        assertEquals(null, selectOnlyWifi(listOf(NetworkCandidate("a", true), NetworkCandidate("b", true))))
    }
}
