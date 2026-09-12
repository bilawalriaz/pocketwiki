package com.example.pocketwiki.ui.main

import com.example.pocketwiki.device.BleState
import com.example.pocketwiki.device.DeviceWifiStatus
import com.example.pocketwiki.device.PhoneNetwork

/*
 * One reading of "where is PocketWiki right now", derived once and shared by
 * the library, packs, device, and setup surfaces so they can never disagree.
 */

data class DeviceSummary(val title: String, val detail: String, val tone: StatusTone)

fun deviceSummary(ble: BleState, wifi: DeviceWifiStatus?): DeviceSummary = when (ble) {
  BleState.Idle -> DeviceSummary("Not connected", "Turn PocketWiki on and keep it nearby", StatusTone.Working)
  BleState.Scanning -> DeviceSummary("Looking for PocketWiki", "Searching nearby", StatusTone.Working)
  is BleState.Found -> DeviceSummary("Ready to connect", "${ble.name} is nearby over Bluetooth", StatusTone.Working)
  BleState.Connecting -> DeviceSummary("Connecting", "Over Bluetooth", StatusTone.Working)
  BleState.Sending -> DeviceSummary("Saving Wi-Fi details", "Over Bluetooth", StatusTone.Working)
  BleState.Reconnecting -> DeviceSummary("Reconnecting", "The Bluetooth link dropped", StatusTone.Working)
  BleState.Sent -> DeviceSummary("Wi-Fi details saved", "PocketWiki is joining your network", StatusTone.Good)
  is BleState.Error -> DeviceSummary("Not connected", ble.message, StatusTone.Attention)
  BleState.Connected -> when {
    wifi?.connected == true && wifi.ssid.isNotBlank() ->
      DeviceSummary("On your Wi-Fi", "“${wifi.ssid}”${ipSuffix(wifi.ip)}", StatusTone.Good)
    wifi != null && wifi.apSsid.isNotBlank() ->
      DeviceSummary("Access point ready", "PocketWiki Wi-Fi: “${wifi.apSsid}”", StatusTone.Good)
    else -> DeviceSummary("Connected over Bluetooth", "Reading status…", StatusTone.Good)
  }
}

private fun ipSuffix(ip: String): String = if (ip.isBlank()) "" else " · $ip"

/** True once the app can talk to the device's library over Bluetooth. */
fun isLinked(ble: BleState): Boolean =
  ble is BleState.Connected || ble is BleState.Sent || ble is BleState.Sending

/** The address the dashboard is served from, following the device's own report. */
fun dashboardUrl(wifi: DeviceWifiStatus?): String = when {
  wifi?.connected == true && wifi.ip.isNotBlank() -> "http://${wifi.ip}"
  wifi?.apIp?.isNotBlank() == true -> "http://${wifi.apIp}"
  else -> "http://192.168.4.1"
}

/** The dashboard address, worded for a button label. */
fun dashboardLabel(wifi: DeviceWifiStatus?): String = when {
  wifi?.connected == true && wifi.ip.isNotBlank() -> "Open ${wifi.ip}"
  wifi?.apIp?.isNotBlank() == true -> "Open ${wifi.apIp}"
  else -> "Open 192.168.4.1"
}

/** How the phone's own network reads next to the device's. */
fun phoneNetworkLabel(network: PhoneNetwork?): String = when {
  network == null -> "Checking…"
  !network.wifiConnected -> "Mobile data"
  network.ssid == null -> "Wi-Fi (name hidden)"
  else -> network.ssid + (network.bandLabel?.let { " · $it" } ?: "")
}
