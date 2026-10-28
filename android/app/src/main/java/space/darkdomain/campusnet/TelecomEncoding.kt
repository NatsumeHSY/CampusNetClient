package space.darkdomain.campusnet

private fun encodeComponent(value: String): String = buildString {
    val allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()"
    for (byte in value.toByteArray(Charsets.UTF_8)) {
        val n = byte.toInt() and 0xff
        if (n < 128 && allowed.indexOf(n.toChar()) >= 0) append(n.toChar())
        else append('%').append("0123456789ABCDEF"[n shr 4]).append("0123456789ABCDEF"[n and 15])
    }
}

fun encodeTwice(value: String): String = encodeComponent(encodeComponent(value))

fun buildTelecomQuery(ipv4: String, macInput: String): String? {
    val octets = ipv4.split('.')
    if (octets.size != 4 || octets.any { it.toIntOrNull()?.let { n -> n in 0..255 } != true } ||
        ipv4.startsWith("127.") || ipv4.startsWith("169.254.") || ipv4 == "0.0.0.0" || ipv4.endsWith(".255")) return null
    val mac = macInput.replace(":", "").replace("-", "").lowercase()
    if (!mac.matches(Regex("[0-9a-f]{12}")) || mac == "020000000000" || mac == "000000000000") return null
    return "wlanuserip=$ipv4&wlanacname=HHXY_XXL2F_HUIJU&t=wireless-v2-plain&nasip=2.2.2.230&mac=$mac"
}

fun isTelecomSuccess(response: String): Boolean =
    Regex("\\\"result\\\"\\s*:\\s*\\\"success\\\"", RegexOption.IGNORE_CASE).containsMatchIn(response)

fun isAlreadyOnline(response: String): Boolean = listOf("登录成功", "success.jsp", "userIndex")
    .any { response.contains(it, ignoreCase = true) }

enum class TelecomResponse { ACCEPTED, REJECTED, UNEXPECTED }
fun classifyTelecomResponse(status: Int, body: String): TelecomResponse = when {
    status !in 200..299 -> TelecomResponse.UNEXPECTED
    isTelecomSuccess(body) -> TelecomResponse.ACCEPTED
    else -> TelecomResponse.REJECTED
}

fun buildTelecomBody(user: String, password: String, query: String): String? {
    if (user.isBlank() || password.isEmpty() || query.isBlank()) return null
    val fields = query.split('&').mapNotNull {
        val pair = it.split('=', limit = 2)
        if (pair.size == 2) pair[0] to pair[1] else null
    }.toMap()
    if (listOf("wlanuserip", "wlanacname", "t", "nasip", "mac").any { fields[it].isNullOrBlank() }) return null
    return "userId=${encodeTwice(user)}&password=${encodeTwice(password)}" +
        "&service=${encodeTwice("电信")}&queryString=${encodeTwice(query)}" +
        "&operatorPwd=&operatorUserId=&validcode=&passwordEncrypt=false"
}
