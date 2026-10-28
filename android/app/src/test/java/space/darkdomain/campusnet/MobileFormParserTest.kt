package space.darkdomain.campusnet

import org.junit.Assert.*
import org.junit.Test
import java.net.URI

class MobileFormParserTest {
    @Test fun parsesHiddenInputsAndExternalDisabledUrlParameter() {
        val html = """
            <input id="urlParameter" value="sid=abc&amp;act=login" disabled>
            <form id="goLoginForm" action="/fallback">
              <input type="hidden" name="userId" value="">
              <input type="hidden" name="notice" value="a&amp;b">
              <input type="hidden" name="termsDesc" value="yes">
              <input type="hidden" name="locked" value="x" disabled>
              <input type="submit" name="submit" value="go">
            </form>
        """.trimIndent()
        val form = parseMobileForm(html)
        assertNotNull(form)
        assertEquals("sid=abc&act=login", form!!.urlParameter)
        assertEquals("http://10.0.0.8:8888/webauth.do?sid=abc&act=login", mobileAction(URI("http://10.0.0.8:8888/login"), form))
        val body = buildMobileBody(form, "user", "p@ss")
        assertTrue(body.contains("userId=user"))
        assertTrue(body.contains("notice=a%26b"))
        assertTrue(body.contains("passwd=p%40ss"))
        assertFalse(body.contains("locked"))
        assertFalse(body.contains("submit"))
    }

    @Test fun ignoresOtherFormsAndRejectsCrossOriginAction() {
        assertNull(parseMobileForm("<form id='other'><input name='userId'></form>"))
        val form = MobileLoginForm("https://evil.example/login", emptyList(), "")
        assertNull(safeMobileAction(URI("http://10.0.0.1/page"), form))
    }
}
