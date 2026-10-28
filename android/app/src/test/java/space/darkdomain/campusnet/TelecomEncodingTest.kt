package space.darkdomain.campusnet

import org.junit.Assert.*
import org.junit.Test

class TelecomEncodingTest {
    @Test fun fieldsUseJavascriptCompatibleDoubleEncoding() {
        assertEquals("a%2540b", encodeTwice("a@b"))
        assertEquals("%25E7%2594%25B5%25E4%25BF%25A1", encodeTwice("电信"))
        assertEquals("a%2520b%252B", encodeTwice("a b+"))
        assertEquals("!'()*-._~", encodeTwice("!'()*-._~"))
    }
    @Test fun missingParametersCannotBeSubmitted() {
        assertNull(buildTelecomBody("user", "pass", ""))
        assertNull(buildTelecomBody("", "pass", "known"))
        assertNull(buildTelecomBody("user", "pass", "wlanuserip=1&mac=2"))
        assertTrue(buildTelecomBody("user", "pass", "wlanuserip=1&wlanacname=ac&t=auth&nasip=2&mac=3")!!.contains("queryString=wlanuserip%253D1%2526wlanacname%253Dac"))
    }
    @Test fun portalIdentityAndJsonSuccessAreValidated() {
        assertNull(buildTelecomQuery("10.0.0.1", "02:00:00:00:00:00"))
        assertNull(buildTelecomQuery("not-an-ip", "00:11:22:AA:BB:CC"))
        assertNull(buildTelecomQuery("0.0.0.0", "00:11:22:AA:BB:CC"))
        assertEquals("wlanuserip=10.0.0.2&wlanacname=HHXY_XXL2F_HUIJU&t=wireless-v2-plain&nasip=2.2.2.230&mac=001122aabbcc",
            buildTelecomQuery("10.0.0.2", "00:11:22:AA:BB:CC"))
        assertTrue(isTelecomSuccess("{\"result\":\"success\"}"))
        assertFalse(isTelecomSuccess("{\"message\":\"success\"}"))
        assertTrue(isAlreadyOnline("<title>登录成功</title>"))
        assertEquals(TelecomResponse.ACCEPTED, classifyTelecomResponse(200, "{\"result\":\"success\"}"))
        assertEquals(TelecomResponse.REJECTED, classifyTelecomResponse(200, "{\"result\":\"fail\"}"))
        assertEquals(TelecomResponse.UNEXPECTED, classifyTelecomResponse(302, "{\"result\":\"success\"}"))
    }
}
