package space.darkdomain.campusnet

import org.junit.Assert.*
import org.junit.Test

class SettingsTest {
    @Test fun carrierDefaultsToTelecom() {
        assertEquals(Carrier.TELECOM, Carrier.fromStored(null))
        assertEquals(Carrier.TELECOM, Carrier.fromStored("unknown"))
        assertEquals(Carrier.MOBILE, Carrier.fromStored("MOBILE"))
    }
}
