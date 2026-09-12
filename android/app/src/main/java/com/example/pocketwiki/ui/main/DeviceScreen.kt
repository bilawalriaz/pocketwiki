package com.example.pocketwiki.ui.main

import android.content.ClipData
import android.content.ClipboardManager
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.example.pocketwiki.device.BleState
import com.example.pocketwiki.device.DeviceNetworkState
import com.example.pocketwiki.device.DeviceWifiStatus
import com.example.pocketwiki.device.PhoneNetwork
import com.example.pocketwiki.device.WifiScanState
import com.example.pocketwiki.ui.icons.PocketIcons

/*
 * Device: the only place radios, addresses, and credentials are named. It is a
 * settings surface, so it stays quiet and factual and never shows an address
 * the device did not report.
 */

@Composable
fun DeviceScreen(
  ble: BleState,
  wifi: DeviceWifiStatus?,
  phone: PhoneNetwork?,
  scanState: WifiScanState,
  library: DeviceNetworkState,
  permissionsGranted: Boolean,
  contentPadding: PaddingValues,
  onConnect: () -> Unit,
  onScanWifi: () -> Unit,
  onSaveWifi: (String, String) -> Unit,
  onOpenUrl: (String) -> Unit,
  onOpenAppSettings: () -> Unit,
  onNotify: (String) -> Unit,
) {
  val context = LocalContext.current
  var editing by remember { mutableStateOf(false) }
  var ssid by remember { mutableStateOf("") }
  var password by remember { mutableStateOf("") }
  val linked = isLinked(ble)
  val working = ble is BleState.Scanning || ble is BleState.Connecting ||
    ble is BleState.Reconnecting || ble is BleState.Sending

  LazyColumn(
    modifier = Modifier.fillMaxSize(),
    contentPadding = contentPadding,
    verticalArrangement = Arrangement.spacedBy(12.dp),
  ) {
    if (!permissionsGranted) {
      item("permissions") {
        NoticeCard(
          icon = PocketIcons.Warning,
          title = "Missing permission",
          body = "Android is blocking Bluetooth or Wi-Fi scanning, so PocketWiki can't be found.",
          iconTint = MaterialTheme.colorScheme.error,
          action = { Button(onClick = onOpenAppSettings, shape = MaterialTheme.shapes.medium) { Text("Open app settings") } },
        )
      }
    }

    item("connection-heading") { SectionHeading("Connection") }
    item("connection") {
      QuietCard {
        InfoRow(
          when (ble) {
            is BleState.Error -> PocketIcons.BluetoothOff
            BleState.Scanning, BleState.Reconnecting -> PocketIcons.BluetoothSearching
            else -> PocketIcons.Bluetooth
          },
          "Bluetooth",
          bleLabel(ble),
          iconTint = when (ble) {
            is BleState.Error -> MaterialTheme.colorScheme.error
            BleState.Connected, BleState.Sent -> MaterialTheme.colorScheme.primary
            else -> MaterialTheme.colorScheme.onSurfaceVariant
          },
        )
        if (working || !linked) {
          CardDivider()
          Row(
            Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
          ) {
            if (working) {
              CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.5.dp)
              Text(bleLabel(ble), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
              Button(onClick = onConnect, shape = MaterialTheme.shapes.medium) {
                Text(if (ble is BleState.Error) "Try again" else "Connect")
              }
            }
          }
        }
      }
    }

    item("network-heading") { SectionHeading("PocketWiki's network") }
    item("network") {
      QuietCard {
        InfoRow(
          PocketIcons.Wifi,
          when {
            wifi == null && !linked -> "Network"
            wifi == null -> "Reading"
            wifi.connected -> "Joined"
            else -> "Access point"
          },
          deviceNetworkValue(wifi, linked),
          iconTint = if (wifi?.connected == true) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
        )
        CardDivider()
        Row(
          Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
          verticalAlignment = Alignment.CenterVertically,
          horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
          Button(onClick = { onOpenUrl(dashboardUrl(wifi)) }, shape = MaterialTheme.shapes.medium) {
            Icon(PocketIcons.OpenInNew, null, Modifier.size(18.dp))
            Spacer(Modifier.width(8.dp))
            Text(dashboardLabel(wifi))
          }
          Spacer(Modifier.weight(1f))
          IconButton(onClick = {
            val clipboard = context.getSystemService(ClipboardManager::class.java)
            clipboard?.setPrimaryClip(ClipData.newPlainText("PocketWiki", dashboardUrl(wifi)))
            onNotify("Dashboard address copied")
          }) {
            Icon(PocketIcons.Copy, "Copy dashboard address", Modifier.size(20.dp))
          }
        }
      }
    }

    item("phone-heading") { SectionHeading("Your phone") }
    item("phone") {
      QuietCard {
        InfoRow(PocketIcons.Phone, "Current connection", phoneNetworkLabel(phone))
      }
    }

    item("wifi-toggle") {
      QuietCard {
        Row(
          Modifier
            .fillMaxWidth()
            .clickable(enabled = linked) {
              if (!editing) {
                ssid = wifi?.ssid.orEmpty()
                password = ""
              }
              editing = !editing
            }
            .padding(horizontal = 16.dp, vertical = 16.dp),
          verticalAlignment = Alignment.CenterVertically,
        ) {
          Icon(
            PocketIcons.Settings,
            null,
            Modifier.size(20.dp),
            tint = if (linked) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.outline,
          )
          Spacer(Modifier.width(14.dp))
          Column(Modifier.weight(1f)) {
            Text(
              if (editing) "Hide network setup" else "Change network",
              style = MaterialTheme.typography.bodyLarge,
              color = if (linked) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.outline,
            )
            if (!linked) {
              Text(
                "Connect over Bluetooth first",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
              )
            }
          }
          Icon(
            if (editing) PocketIcons.ExpandLess else PocketIcons.ExpandMore,
            null,
            Modifier.size(18.dp),
            tint = MaterialTheme.colorScheme.outline,
          )
        }
      }
    }

    item("wifi-editor") {
      AnimatedVisibility(
        visible = editing && linked,
        enter = expandVertically(disclosureTiming(), expandFrom = Alignment.Top) + fadeIn(effectsMotion()),
        exit = shrinkVertically(disclosureTiming(), shrinkTowards = Alignment.Top) + fadeOut(effectsMotion()),
        label = "wifiEditor",
      ) {
        Column(Modifier.fillMaxWidth()) {
          NetworkPicker(
            scanState = scanState,
            phoneSsid = phone?.ssid,
            ssid = ssid,
            onSsidChange = { ssid = it },
            password = password,
            onPasswordChange = { password = it },
            enabled = ble !is BleState.Sending,
            errorMessage = (ble as? BleState.Error)?.message,
            savedMessage = if (ble is BleState.Sent) "Saved." else null,
            onScan = onScanWifi,
          )
          Spacer(Modifier.height(12.dp))
          QuietCard {
            Column(Modifier.fillMaxWidth().padding(16.dp)) {
              Button(
                onClick = { onSaveWifi(ssid, password) },
                modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
                enabled = ssid.isNotBlank() && ble !is BleState.Sending,
              ) {
                Text(
                  if (ble is BleState.Sending) "Saving…" else "Save Wi-Fi details",
                  style = MaterialTheme.typography.titleMedium,
                )
              }
            }
          }
        }
      }
    }

    if (library is DeviceNetworkState.Ready) {
      item("storage") {
        QuietCard { StorageMeter(library.used, library.total, library.installable) }
      }
    }
  }
}

private fun bleLabel(ble: BleState): String = when (ble) {
  BleState.Idle -> "Not connected"
  BleState.Scanning -> "Searching for PocketWiki…"
  is BleState.Found -> "Nearby: ${ble.name}"
  BleState.Connecting -> "Connecting…"
  BleState.Reconnecting -> "Reconnecting…"
  BleState.Connected -> "Connected"
  BleState.Sending -> "Sending Wi-Fi details…"
  BleState.Sent -> "Wi-Fi details saved"
  is BleState.Error -> ble.message
}

private fun deviceNetworkValue(wifi: DeviceWifiStatus?, linked: Boolean): String = when {
  wifi?.connected == true && wifi.ssid.isNotBlank() ->
    "“${wifi.ssid}”${if (wifi.ip.isBlank()) "" else " · ${wifi.ip}"}"
  wifi?.apSsid?.isNotBlank() == true ->
    "“${wifi.apSsid}”${if (wifi.apIp.isBlank()) "" else " · ${wifi.apIp}"}"
  linked -> "Reading…"
  else -> "Connect over Bluetooth to read this"
}
