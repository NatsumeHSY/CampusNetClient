package space.darkdomain.campusnet

import java.net.URI

fun sameOrigin(first: URI, second: URI): Boolean {
    fun port(uri: URI): Int = if (uri.port != -1) uri.port else when (uri.scheme?.lowercase()) {
        "http" -> 80
        "https" -> 443
        else -> -1
    }
    val scheme = first.scheme?.lowercase()
    return scheme in listOf("http", "https") && second.scheme.equals(scheme, true) &&
        first.host != null && second.host != null && first.host.equals(second.host, true) &&
        first.userInfo == null && second.userInfo == null && port(first) == port(second)
}
