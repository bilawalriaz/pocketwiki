package com.example.pocketwiki.device

import android.annotation.SuppressLint
import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** The phone's own network, shown so the user can see whether the phone and
 *  PocketWiki share the same 2.4 GHz network (or the PocketWiki access point). */
data class PhoneNetwork(val ssid: String?, val wifiConnected: Boolean, val frequency: Int? = null) {
  val bandLabel: String? get() = frequency?.let { when {
    it < 3000 -> "2.4 GHz"
    it < 6000 -> "5 GHz"
    else -> "6 GHz"
  } }
}

/** Observes the phone's current network. SSID needs the fine-location
 *  permission (already requested for Wi-Fi scanning); without it the SSID is
 *  null and only the Wi-Fi/mobile-data distinction is reported. */
class PhoneWifi(context: Context) {
  private val appContext = context.applicationContext
  private val connectivity = appContext.getSystemService(ConnectivityManager::class.java)
  private val wifi = appContext.getSystemService(WifiManager::class.java)
  private val _state = MutableStateFlow(read())
  val state: StateFlow<PhoneNetwork> = _state
  private var registered = false

  init { register() }

  private fun register() {
    if (registered) return
    runCatching { connectivity.registerDefaultNetworkCallback(networkCallback) }
    registered = true
  }

  fun refresh() {
    _state.value = read()
  }

  @SuppressLint("MissingPermission")
  @Suppress("DEPRECATION")
  private fun read(): PhoneNetwork {
    val active = connectivity.activeNetwork
    val onWifi = active != null &&
      connectivity.getNetworkCapabilities(active)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
    if (!onWifi) return PhoneNetwork(null, false)
    var ssid: String? = null
    var frequency: Int? = null
    try {
      val info = wifi.connectionInfo
      ssid = info.ssid
        ?.takeIf { it != "<unknown ssid>" && it != "0x" && it.isNotBlank() }
        ?.removePrefix("\"")
        ?.removeSuffix("\"")
      frequency = if (info.frequency in 2400..7000) info.frequency else null
    } catch (_: SecurityException) {
      ssid = null
    }
    return PhoneNetwork(ssid, true, frequency)
  }

  private val networkCallback = object : ConnectivityManager.NetworkCallback() {
    override fun onAvailable(network: Network) { refresh() }
    override fun onLost(network: Network) { refresh() }
  }

  fun close() {
    if (registered) runCatching { connectivity.unregisterNetworkCallback(networkCallback) }
    registered = false
  }
}
