package space.darkdomain.campusnet

import org.junit.Assert.*
import org.junit.Test
import java.net.URI

class PortalOriginTest {
    @Test fun credentialsStayOnExactOrigin() {
        val origin = URI("http://10.10.12.13:80/login")
        assertTrue(sameOrigin(origin, URI("http://10.10.12.13/submit")))
        assertFalse(sameOrigin(origin, URI("http://10.10.12.13:8888/submit")))
        assertFalse(sameOrigin(origin, URI("https://10.10.12.13/submit")))
        assertFalse(sameOrigin(origin, URI("http://10.10.12.13.evil.example/")))
        assertFalse(sameOrigin(origin, URI("http://user@10.10.12.13/")))
    }
}
