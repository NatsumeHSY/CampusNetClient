package space.darkdomain.campusnet

import org.jsoup.Jsoup
import java.net.URI
import java.net.URLEncoder

data class MobileInput(val name: String, val value: String, val disabled: Boolean, val type: String, val checked: Boolean)
data class MobileLoginForm(val action: String, val fields: List<MobileInput>, val urlParameter: String)

fun parseMobileForm(html: String): MobileLoginForm? {
    if (html.length > 1024 * 1024) return null
    val document = Jsoup.parse(html)
    val form = document.select("form").firstOrNull {
        it.id().equals("goLoginForm", true) || it.attr("name").equals("goLoginForm", true)
    } ?: return null
    val fields = form.select("input[name]").map { element ->
        MobileInput(element.attr("name"), element.attr("value"), element.hasAttr("disabled"),
            element.attr("type").ifBlank { "text" }.lowercase(), element.hasAttr("checked"))
    }
    val urlParameter = fields.firstOrNull { it.name.equals("urlParameter", true) && !it.disabled }?.value
        ?: document.select("input").firstOrNull {
            (it.id().equals("urlParameter", true) || it.attr("name").equals("urlParameter", true)) && it.hasAttr("value")
        }?.attr("value").orEmpty()
    return MobileLoginForm(form.attr("action"), fields, urlParameter)
}

fun mobileAction(page: URI, form: MobileLoginForm): String = safeMobileAction(page, form) ?: ""

fun safeMobileAction(page: URI, form: MobileLoginForm): String? {
    val raw = if (form.urlParameter.isNotBlank()) "/webauth.do?${form.urlParameter}"
        else form.action.ifBlank { "/webauth.do" }
    val action = runCatching { page.resolve(raw) }.getOrNull() ?: return null
    return action.toString().takeIf { sameOrigin(page, action) && action.path.startsWith("/") }
}

fun buildMobileBody(form: MobileLoginForm, user: String, password: String): String {
    val values = mutableListOf<Pair<String, String>>()
    var userAdded = false
    var passwordAdded = false
    for (field in form.fields) {
        if (field.disabled || field.type in setOf("submit", "button", "image", "reset") ||
            field.type in setOf("checkbox", "radio") && !field.checked) continue
        val key = field.name.lowercase()
        val value = when (key) {
            "userid", "username" -> { userAdded = true; user }
            "passwd", "passwd1", "password" -> {
                if (key == "passwd") passwordAdded = true
                password
            }
            else -> field.value
        }
        values += field.name to value
    }
    if (!userAdded) values += "userId" to user
    if (!passwordAdded) values += "passwd" to password
    return values.joinToString("&") { (key, value) -> "${encodeForm(key)}=${encodeForm(value)}" }
}

private fun encodeForm(value: String): String = URLEncoder.encode(value, Charsets.UTF_8.name()).replace("+", "%20")
