package space.darkdomain.campusnet

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkAddress
import android.net.Network
import android.net.NetworkCapabilities
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.URL

data class AuthResult(val success: Boolean, val detail: String, val publicIp: String? = null)

class TelecomAuthenticator(private val context: Context) {
    suspend fun login(user: String, password: String, suppliedMac: String): AuthResult = withContext(Dispatchers.IO) {
        if (user.isBlank() || password.isEmpty()) return@withContext AuthResult(false, "请填写校园网账号和密码。")
        val manager = context.getSystemService(ConnectivityManager::class.java)
        val selection = CampusNetworkSelector(manager).select()
        if (selection !is NetworkSelection.Selected) return@withContext AuthResult(false, when (selection) {
            NetworkSelection.NoWifi -> "未发现 Wi-Fi 网络。"
            NetworkSelection.Ambiguous -> "检测到多个 Wi-Fi 网络，请断开多余网络后重试。"
            NetworkSelection.PermissionRequired -> "缺少读取网络状态的权限。"
            else -> "未找到可用校园网 Wi-Fi。"
        })
        val network = selection.network
        val caps = manager.getNetworkCapabilities(network)
        if (caps?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) != true) return@withContext AuthResult(false, "选中的网络已不再是 Wi-Fi。")
        val ipv4 = manager.getLinkProperties(network)?.linkAddresses.orEmpty()
            .asSequence().map(LinkAddress::getAddress).filterIsInstance<Inet4Address>()
            .firstOrNull { !it.isLoopbackAddress && !it.isLinkLocalAddress }?.hostAddress
            ?: return@withContext AuthResult(false, "无法读取校园 Wi-Fi 的 IPv4 地址。")
        val query = buildTelecomQuery(ipv4, suppliedMac)
            ?: return@withContext AuthResult(false, "请从手机 Wi-Fi 详情中填写该网络当前使用的 MAC 地址。")

        try {
            val root = request(network, URL("http://10.10.12.13/"))
            if (root.status in 200..299 && isAlreadyOnline(root.body)) {
                return@withContext probeInternet(network)?.let { AuthResult(true, "网络已认证。", it) }
                    ?: AuthResult(false, "门户显示已认证，但校园 Wi-Fi 外网验证未通过。")
            }
            val body = buildTelecomBody(user, password, query)
                ?: return@withContext AuthResult(false, "门户认证参数不完整。")
            val response = request(network, URL("http://10.10.12.13/eportal/InterFace.do?method=login"), body)
            when (classifyTelecomResponse(response.status, response.body)) {
                TelecomResponse.ACCEPTED -> {
                    val ip = probeInternet(network)
                    if (ip != null) AuthResult(true, "校园网认证成功。", ip)
                    else AuthResult(false, "门户接受了登录请求，但校园 Wi-Fi 外网验证未通过。")
                }
                TelecomResponse.REJECTED -> AuthResult(false, "门户拒绝登录，请检查套餐、账号或密码。")
                TelecomResponse.UNEXPECTED -> AuthResult(false, "门户返回了无法识别的响应（HTTP ${response.status}）。")
            }
        } catch (_: IOException) {
            AuthResult(false, "校园网门户无法连接，请确认已连接校园 Wi-Fi。")
        } catch (_: SecurityException) {
            AuthResult(false, "系统拒绝访问当前 Wi-Fi 网络。")
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
            AuthResult(false, "登录失败，无法解析门户响应。")
        }
    }

    private data class HttpResult(val status: Int, val body: String)

    private fun request(network: Network, url: URL, form: String? = null): HttpResult {
        val connection = network.openConnection(url) as HttpURLConnection
        connection.connectTimeout = 8000
        connection.readTimeout = 10000
        connection.instanceFollowRedirects = false
        connection.requestMethod = if (form == null) "GET" else "POST"
        connection.setRequestProperty("User-Agent", "CampusNetAndroid/0.1")
        if (form != null) {
            connection.doOutput = true
            connection.setRequestProperty("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8")
            connection.outputStream.use { it.write(form.toByteArray(Charsets.UTF_8)) }
        }
        try {
            val status = connection.responseCode
            val stream = if (status >= 400) connection.errorStream else connection.inputStream
            val bytes = ByteArrayOutputStream()
            stream?.use { input ->
                val buffer = ByteArray(4096)
                var total = 0
                while (true) {
                    val read = input.read(buffer)
                    if (read < 0) break
                    total += read
                    if (total > 512 * 1024) throw IOException("Portal response too large")
                    bytes.write(buffer, 0, read)
                }
            }
            return HttpResult(status, bytes.toByteArray().toString(Charsets.UTF_8))
        } finally { connection.disconnect() }
    }

    private fun probeInternet(network: Network): String? {
        val probe204 = runCatching { request(network, URL("https://connect.rom.miui.com/generate_204")).status == 204 }.getOrDefault(false)
        val ip = runCatching { request(network, URL("https://api64.ipify.org/")).body.trim() }.getOrNull()
            ?.takeIf { it.length <= 45 && it.matches(Regex("[0-9a-fA-F:.]+")) }
        return ip ?: if (probe204) "外网可达" else null
    }
}
