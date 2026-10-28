package space.darkdomain.campusnet

import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities

sealed interface NetworkSelection {
    data class Selected(val network: Network) : NetworkSelection
    data object NoWifi : NetworkSelection
    data object Ambiguous : NetworkSelection
    data object PermissionRequired : NetworkSelection
}

class CampusNetworkSelector(private val connectivity: ConnectivityManager) {
    fun select(): NetworkSelection = try {
        val networks = connectivity.allNetworks.filter { network ->
            connectivity.getNetworkCapabilities(network)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        }
        when (networks.size) {
            0 -> NetworkSelection.NoWifi
            1 -> NetworkSelection.Selected(networks.single())
            else -> NetworkSelection.Ambiguous
        }
    } catch (_: SecurityException) {
        NetworkSelection.PermissionRequired
    }
}
