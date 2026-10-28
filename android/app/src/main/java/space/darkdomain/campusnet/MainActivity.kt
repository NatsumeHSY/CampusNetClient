package space.darkdomain.campusnet

import android.os.Build
import android.os.Bundle
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val supported = DeviceSupport.isSupported(Build.VERSION.SDK_INT, Build.SUPPORTED_ABIS.toList())
        val info = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val battery = batteryState(info?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1, info?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1)
        val environment = detectEnvironment(Build.BRAND, Build.DISPLAY, Build.FINGERPRINT, "")
        val repository = SettingsRepository(applicationContext)
        setContent { CampusApp(supported, battery, environment, repository) }
    }
}

@Composable
fun CampusApp(supported: Boolean, battery: BatteryState, environment: EnvironmentKind, repository: SettingsRepository? = null) {
    MaterialTheme(colorScheme = if (isSystemInDarkTheme()) darkColorScheme() else lightColorScheme()) {
        Surface(modifier = Modifier.fillMaxSize()) {
            Column(modifier = Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                Text("广航海校园网自动登录", style = MaterialTheme.typography.headlineSmall)
                Text(if (supported) "HTTP 登录流程已接入，尚未在手机和校园网实测。" else "仅支持 Android 12 及以上 ARM 设备。")
                if (environment == EnvironmentKind.HARMONY || environment == EnvironmentKind.SUSPECTED_COMPATIBILITY) {
                    Text("可能运行在鸿蒙或卓易通环境，部分功能可能不支持或不生效，后续不考虑适配。")
                }
                if (battery != BatteryState.OK) Text(if (battery == BatteryState.LOW) "电量不高于 30%，自动登录不可用。" else "无法读取电量，自动登录不可用。")
                if (supported && repository != null) SettingsForm(repository)
            }
        }
    }
}

@Composable
private fun SettingsForm(repository: SettingsRepository) {
    val saved by repository.settings.collectAsState(initial = SavedSettings())
    var selected by remember { mutableStateOf(Carrier.TELECOM) }
    var username by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var wifiMac by remember { mutableStateOf("") }
    var message by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var publicIp by remember { mutableStateOf("") }
    var expanded by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    LaunchedEffect(saved) {
        selected = saved.carrier
        wifiMac = saved.mac
        saved.credentials?.let { username = it.user; password = it.password }
        if (saved.credentialsUnreadable) message = "无法读取已保存的凭据，请重新输入。"
    }
    Box {
        OutlinedButton(onClick = { expanded = true }) { Text(if (selected == Carrier.TELECOM) "电信" else "移动（测试中）") }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            DropdownMenuItem(text = { Text("电信") }, onClick = { selected = Carrier.TELECOM; expanded = false })
            DropdownMenuItem(text = { Text("移动（测试中）") }, onClick = { selected = Carrier.MOBILE; expanded = false })
        }
    }
    OutlinedTextField(value = username, onValueChange = { username = it }, label = { Text("校园网账号") }, singleLine = true)
    OutlinedTextField(value = password, onValueChange = { password = it }, label = { Text("校园网密码") }, visualTransformation = PasswordVisualTransformation(), singleLine = true)
    Text("门户使用 HTTP，认证期间账号密码会以明文经过校园 Wi-Fi。")
    OutlinedTextField(value = wifiMac, onValueChange = { wifiMac = it }, label = { Text("校园 Wi-Fi MAC 地址") }, supportingText = { Text("从手机 Wi-Fi 网络详情中查看；Android 不允许应用直接读取硬件 MAC。") }, singleLine = true)
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Button(onClick = {
            scope.launch {
                message = try { repository.save(selected, username, password, wifiMac); "已保存" } catch (_: Exception) { "保存失败，请重试。" }
            }
        }) { Text("保存") }
        OutlinedButton(onClick = {
            scope.launch {
                message = try { repository.clearCredentials(); username = ""; password = ""; "已清除凭据" } catch (_: Exception) { "清除失败，请重试。" }
            }
        }) { Text("清除") }
    }
    Button(enabled = !busy && username.isNotBlank() && password.isNotEmpty() && (selected == Carrier.MOBILE || wifiMac.isNotBlank()),
        onClick = {
            scope.launch {
                busy = true
                publicIp = ""
                val result = try {
                    if (selected == Carrier.TELECOM) TelecomAuthenticator(repository.context).login(username, password, wifiMac)
                    else MobileAuthenticator(repository.context).login(username, password)
                }
                catch (_: Exception) { AuthResult(false, "登录失败，请检查网络和设置。") }
                message = result.detail
                publicIp = result.publicIp.orEmpty()
                busy = false
            }
        }) {
        if (busy) CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp) else Text("登录")
    }
    if (selected == Carrier.MOBILE) Text("移动套餐流程基于门户表单快速路径，仍在测试。")
    if (publicIp.isNotEmpty()) Text("外网地址：$publicIp")
    if (message.isNotEmpty()) Text(message)
}
