package space.darkdomain.campusnet

object DeviceSupport {
    fun isSupported(api: Int, abis: List<String>): Boolean =
        api >= 31 && abis.any { it == "arm64-v8a" || it == "armeabi-v7a" }
}
