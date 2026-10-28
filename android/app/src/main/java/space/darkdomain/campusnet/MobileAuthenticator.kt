package space.darkdomain.campusnet

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.CookieManager
import java.net.CookiePolicy
import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.InetAddress
import java.net.URI
import java.net.URL

class MobileAuthenticator(private val context: Context) {
    suspend fun login(user: String, password: String): AuthResult = withContext(Dispatchers.IO) {
        if (user.isBlank() || password.isEmpty()) return@withContext AuthResult(false, "请填写校园网账号和密码。")
        val manager = context.getSystemService(ConnectivityManager::class.java)
        val selection = CampusNetworkSelector(manager).select()
        if (selection !is NetworkSelection.Selected) return@withContext AuthResult(false, "请连接唯一的校园 Wi-Fi 后重试。")
        val network = selection.network
        if (manager.getNetworkCapabilities(network)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) != true)
            return@withContext AuthResult(false, "选中的网络已不再是 Wi-Fi。")

        try {
            val session = MobilePortalSession(network)
            val page = session.get(URL("http://1.1.1.1/"))
            if (page.status !in 200..299) return@withContext AuthResult(false, "移动门户返回 HTTP ${page.status}。")
            val form = parseMobileForm(page.body)
                ?: return@withContext AuthResult(false, "页面没有 goLoginForm，移动 HTTP 流程无法处理；请稍后使用浏览器登录。")
            val pageUri = URI(page.finalUrl)
            val action = safeMobileAction(pageUri, form)
                ?: return@withContext AuthResult(false, "门户提交地址不在当前来源，已停止发送账号密码。")
            val post = session.post(URL(action), buildMobileBody(form, user, password), page.finalUrl)
            if (post.status !in 200..299) return@withContext AuthResult(false, "移动门户提交失败（HTTP ${post.status}）。")
            val ip = verifyInternet(network)
            if (ip != null) AuthResult(true, "校园网认证成功。", ip)
            else AuthResult(false, "门户已收到登录请求，但校园 Wi-Fi 外网验证未通过。")
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: IOException) {
            AuthResult(false, "移动门户无法连接，请确认已连接校园 Wi-Fi。")
        } catch (_: SecurityException) {
            AuthResult(false, "系统拒绝访问当前 Wi-Fi 网络。")
        } catch (_: Exception) {
            AuthResult(false, "移动登录失败，门户页面格式可能已变化。")
        }
    }

    private fun verifyInternet(network: Network): String? {
        val result = runCatching { MobilePortalSession(network).get(URL("https://api64.ipify.org/")) }.getOrNull() ?: return null
        if (result.status == 200) result.body.trim().takeIf { it.length <= 45 && it.matches(Regex("[0-9a-fA-F:.]+")) }?.let { return it }
        val noContent = runCatching { MobilePortalSession(network).get(URL("https://connect.rom.miui.com/generate_204")) }.getOrNull()
        return "外网已连通".takeIf { noContent?.status == 204 }
    }
}

private data class MobileHttpResult(val status: Int, val body: String, val finalUrl: String, val location: String?)

private class MobilePortalSession(private val network: Network) {
    private val cookies = CookieManager(null, CookiePolicy.ACCEPT_ALL)

    fun get(initial: URL): MobileHttpResult {
        var current = initial
        repeat(6) { attempt ->
            val response = execute(current, "GET", null, null)
            if (response.status !in 300..399) return response
            val location = response.location ?: return response
            val next = runCatching { current.toURI().resolve(location).toURL() }.getOrNull() ?: return response
            if (next.protocol !in setOf("http", "https") || attempt == 5) return response
            if (!isAllowedPortalHost(next.host)) return response
            current = next
        }
        throw IOException("Too many redirects")
    }

    fun post(url: URL, body: String, referer: String): MobileHttpResult = execute(url, "POST", body, referer)

    private fun execute(url: URL, method: String, body: String?, referer: String?): MobileHttpResult {
        val uri = url.toURI()
        val connection = network.openConnection(url) as HttpURLConnection
        connection.connectTimeout = 8000
        connection.readTimeout = 10000
        connection.instanceFollowRedirects = false
        connection.requestMethod = method
        connection.setRequestProperty("User-Agent", "CampusNetAndroid/0.1")
        referer?.let { connection.setRequestProperty("Referer", it) }
        cookies.get(uri, emptyMap()).forEach { (name, values) -> values.forEach { connection.addRequestProperty(name, it) } }
        if (body != null) {
            connection.doOutput = true
            connection.setRequestProperty("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8")
            connection.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
        }
        try {
            val status = connection.responseCode
            val location = connection.getHeaderField("Location")
            cookies.put(uri, connection.headerFields.filterKeys { it != null })
            val stream = if (status >= 400) connection.errorStream else connection.inputStream
            val output = ByteArrayOutputStream()
            stream?.use { input ->
                val buffer = ByteArray(4096)
                var total = 0
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    total += count
                    if (total > 1024 * 1024) throw IOException("Portal page too large")
                    output.write(buffer, 0, count)
                }
            }
            return MobileHttpResult(status, output.toByteArray().toString(Charsets.UTF_8), url.toString(), location)
        } finally { connection.disconnect() }
    }
}

private fun isAllowedPortalHost(host: String): Boolean {
    if (host == "1.1.1.1") return true
    val octets = host.split('.')
    if (octets.size != 4 || octets.any { it.toIntOrNull()?.let { n -> n in 0..255 } != true }) return false
    val address = runCatching { InetAddress.getByName(host) as? Inet4Address }.getOrNull() ?: return false
    return address.isSiteLocalAddress && !address.isLoopbackAddress && !address.isLinkLocalAddress
}
