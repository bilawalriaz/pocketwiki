package com.bilawalriaz.pocketwiki.device

import android.Manifest
import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.wifi.ScanResult
import android.net.wifi.WifiManager
import android.location.LocationManager
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

data class WifiNetwork(val ssid: String, val level: Int, val secured: Boolean)

sealed interface WifiScanState {
  data object Idle : WifiScanState
  data object Scanning : WifiScanState
  data class Ready(val networks: List<WifiNetwork>, val cached: Boolean = false) : WifiScanState
  data class Error(val message: String) : WifiScanState
}

class WifiScanner(context: Context) {
  private val appContext = context.applicationContext
  private val wifi = appContext.getSystemService(WifiManager::class.java)
  private val location = appContext.getSystemService(LocationManager::class.java)
  private val _state = MutableStateFlow<WifiScanState>(WifiScanState.Idle)
  val state: StateFlow<WifiScanState> = _state
  private var registered = false

  val permission: String = Manifest.permission.ACCESS_FINE_LOCATION

  fun hasPermission(): Boolean =
    ContextCompat.checkSelfPermission(appContext, permission) == PackageManager.PERMISSION_GRANTED

  @SuppressLint("MissingPermission")
  @Suppress("DEPRECATION")
  fun scan() {
    if (!hasPermission()) {
      _state.value = WifiScanState.Error("Location permission is needed to list nearby Wi-Fi networks. You can still enter the name manually.")
      return
    }
    if (!wifi.isWifiEnabled) {
      _state.value = WifiScanState.Error("Turn on Wi-Fi to scan nearby networks, or enter the name manually.")
      return
    }
    if (!location.isLocationEnabled) {
      _state.value = WifiScanState.Error("Turn on Location to scan Wi-Fi network names, or enter the name manually.")
      return
    }
    register()
    _state.value = WifiScanState.Scanning
    try {
      if (!wifi.startScan()) publishResults(cached = true)
    } catch (_: SecurityException) {
      _state.value = WifiScanState.Error("Allow precise location and turn on Location to scan Wi-Fi networks. Manual entry still works.")
    }
  }

  fun close() {
    if (registered) runCatching { appContext.unregisterReceiver(receiver) }
    registered = false
  }

  private fun register() {
    if (registered) return
    ContextCompat.registerReceiver(
      appContext,
      receiver,
      IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION),
      ContextCompat.RECEIVER_NOT_EXPORTED,
    )
    registered = true
  }

  private val receiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context?, intent: Intent?) {
      val updated = intent?.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, false) == true
      publishResults(cached = !updated)
    }
  }

  @SuppressLint("MissingPermission")
  @Suppress("DEPRECATION")
  private fun publishResults(cached: Boolean) {
    try {
      val networks = wifi.scanResults
        .asSequence()
        .filter { it.SSID.isNotBlank() && it.frequency in 2400..2500 }
        .groupBy(ScanResult::SSID)
        .mapNotNull { (ssid, accessPoints) ->
          accessPoints.maxByOrNull(ScanResult::level)?.let { result ->
            WifiNetwork(ssid, result.level, result.capabilities.contains(Regex("WEP|WPA|SAE|OWE")))
          }
        }
        .sortedByDescending(WifiNetwork::level)
      _state.value = WifiScanState.Ready(networks, cached)
    } catch (_: SecurityException) {
      _state.value = WifiScanState.Error("Allow precise location and turn on Location to scan Wi-Fi networks. Manual entry still works.")
    }
  }
}
