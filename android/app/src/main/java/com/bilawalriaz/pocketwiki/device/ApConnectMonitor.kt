package com.bilawalriaz.pocketwiki.device

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.wifi.WifiManager
import kotlinx.coroutines.flow.StateFlow

/**
 * Watches the phone's Wi‑Fi association and, when it joins the PocketWiki
 * access point, fires [onJoin]. This gives the "captive portal" feel on the
 * app side: instead of the system intercepting traffic, PocketWiki notices the
 * join and opens the dashboard for the user.
 *
 * Matching uses the live [knownApSsid] (the firmware default, refined from the
 * device once BLE reports it) and fires once per join so it does not spam the
 * browser on every network‑state broadcast.
 *
 * [suppressed] is held true while the app itself is driving a Wi‑Fi transfer
 * (the installer's programmatic SoftAP hop): opening the browser then would
 * compete for the access point and can drop the upload, so we stay quiet.
 */
class ApConnectMonitor(
  context: Context,
  private val knownApSsid: StateFlow<String?>,
  private val suppressed: StateFlow<Boolean>,
  private val onJoin: () -> Unit,
) {
  private val appContext = context.applicationContext
  private val wifi = appContext.getSystemService(WifiManager::class.java)
  private var lastJoined: String? = null

  private val receiver = object : BroadcastReceiver() {
    override fun onReceive(c: Context?, intent: Intent?) {
      if (intent?.action != WifiManager.NETWORK_STATE_CHANGED_ACTION) return
      if (suppressed.value) return
      val raw = runCatching { wifi?.connectionInfo?.ssid }.getOrNull() ?: return
      if (raw == "<unknown ssid>" || raw == "0x" || raw.isBlank()) return
      val ssid = raw.removePrefix("\"").removeSuffix("\"")
      val known = knownApSsid.value ?: "PocketWiki"
      if (ssid.equals(known, ignoreCase = true)) {
        if (lastJoined != ssid) {
          lastJoined = ssid
          onJoin()
        }
      } else {
        lastJoined = null
      }
    }
  }

  fun start() {
    runCatching {
      appContext.registerReceiver(receiver, IntentFilter(WifiManager.NETWORK_STATE_CHANGED_ACTION))
    }
  }

  fun stop() {
    runCatching { appContext.unregisterReceiver(receiver) }
  }
}
