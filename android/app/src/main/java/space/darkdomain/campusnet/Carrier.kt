package space.darkdomain.campusnet

enum class Carrier {
    TELECOM, MOBILE;
    companion object {
        fun fromStored(value: String?): Carrier = entries.firstOrNull { it.name == value } ?: TELECOM
    }
}
