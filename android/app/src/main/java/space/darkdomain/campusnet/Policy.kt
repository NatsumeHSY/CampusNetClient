package space.darkdomain.campusnet

enum class BatteryState { OK, LOW, UNKNOWN }

fun batteryState(level: Int, scale: Int): BatteryState {
    if (level < 0 || scale <= 0 || level > scale) return BatteryState.UNKNOWN
    return if (level.toLong() * 100 <= scale.toLong() * 30) BatteryState.LOW else BatteryState.OK
}

enum class EnvironmentKind { EMUI, HARMONY, SUSPECTED_COMPATIBILITY, UNKNOWN }

fun detectEnvironment(brand: String, display: String, fingerprint: String, installer: String): EnvironmentKind {
    val signals = "$display $fingerprint".lowercase()
    return when {
        "droitong" in signals || "卓易通" in signals -> EnvironmentKind.SUSPECTED_COMPATIBILITY
        "harmonyos" in signals -> EnvironmentKind.HARMONY
        brand.equals("huawei", ignoreCase = true) && display.contains("emui", ignoreCase = true) -> EnvironmentKind.EMUI
        else -> EnvironmentKind.UNKNOWN
    }
}

data class NetworkCandidate<T>(val id: T, val wifi: Boolean)
fun <T> selectOnlyWifi(candidates: List<NetworkCandidate<T>>): T? =
    candidates.filter { it.wifi }.singleOrNull()?.id
