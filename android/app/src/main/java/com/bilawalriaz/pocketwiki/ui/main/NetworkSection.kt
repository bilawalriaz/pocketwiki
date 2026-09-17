package com.bilawalriaz.pocketwiki.ui.main

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import com.bilawalriaz.pocketwiki.device.WifiNetwork
import com.bilawalriaz.pocketwiki.device.WifiScanState
import com.bilawalriaz.pocketwiki.ui.icons.PocketIcons

/*
 * Choosing the network PocketWiki should join: the scanned 2.4 GHz list and the
 * two credential fields. Shared by first-run setup and the device screen, so
 * both teach the same thing. The caller owns the credentials and decides where
 * the save action lives.
 */

/** How many scanned rows are worth showing before the fields would fall off-screen. */
private const val VISIBLE_NETWORKS = 6

@Composable
fun NetworkPicker(
  scanState: WifiScanState,
  phoneSsid: String?,
  ssid: String,
  onSsidChange: (String) -> Unit,
  password: String,
  onPasswordChange: (String) -> Unit,
  enabled: Boolean,
  errorMessage: String?,
  savedMessage: String?,
  onScan: () -> Unit,
) {
  LaunchedEffect(Unit) { onScan() }

  QuietCard {
    Row(
      Modifier.fillMaxWidth().padding(start = 16.dp, end = 6.dp, top = 6.dp, bottom = 6.dp),
      verticalAlignment = Alignment.CenterVertically,
    ) {
      Text("Nearby 2.4 GHz networks", Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
      TextButton(onClick = onScan, enabled = scanState !is WifiScanState.Scanning) { Text("Rescan") }
    }
    NetworkList(scanState, phoneSsid, ssid) { onSsidChange(it); onPasswordChange("") }
  }

  Spacer(Modifier.height(12.dp))

  QuietCard {
    Column(Modifier.fillMaxWidth().padding(16.dp)) {
      Text("Network details", style = MaterialTheme.typography.titleMedium)
      Spacer(Modifier.height(14.dp))
      CredentialFields(
        ssid = ssid,
        onSsidChange = onSsidChange,
        password = password,
        onPasswordChange = onPasswordChange,
        enabled = enabled,
      )
      if (errorMessage != null) {
        Spacer(Modifier.height(12.dp))
        Text(errorMessage, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
      }
      if (savedMessage != null) {
        Spacer(Modifier.height(12.dp))
        Text(savedMessage, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
      }
    }
  }
}

/** The two fields, so the setup action bar and the device screen share one shape. */
@Composable
fun CredentialFields(
  ssid: String,
  onSsidChange: (String) -> Unit,
  password: String,
  onPasswordChange: (String) -> Unit,
  enabled: Boolean,
) {
  var reveal by remember { mutableStateOf(false) }
  OutlinedTextField(
    value = ssid,
    onValueChange = onSsidChange,
    modifier = Modifier.fillMaxWidth(),
    label = { Text("Network name") },
    singleLine = true,
    enabled = enabled,
  )
  Spacer(Modifier.height(12.dp))
  OutlinedTextField(
    value = password,
    onValueChange = onPasswordChange,
    modifier = Modifier.fillMaxWidth(),
    label = { Text("Password") },
    singleLine = true,
    enabled = enabled,
    visualTransformation = if (reveal) VisualTransformation.None else PasswordVisualTransformation(),
    trailingIcon = {
      IconButton(onClick = { reveal = !reveal }) {
        Icon(
          if (reveal) PocketIcons.Hidden else PocketIcons.Visible,
          contentDescription = if (reveal) "Hide password" else "Show password",
          modifier = Modifier.size(20.dp),
        )
      }
    },
  )
}

@Composable
private fun NetworkList(
  state: WifiScanState,
  phoneSsid: String?,
  selectedSsid: String,
  onSelect: (String) -> Unit,
) {
  when (state) {
    WifiScanState.Idle, WifiScanState.Scanning -> Row(
      Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 20.dp),
      verticalAlignment = Alignment.CenterVertically,
      horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
      CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.5.dp)
      Text(
        "Scanning 2.4 GHz networks…",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    }

    is WifiScanState.Error -> Text(
      state.message,
      Modifier.padding(horizontal = 16.dp, vertical = 20.dp),
      style = MaterialTheme.typography.bodyMedium,
      color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    is WifiScanState.Ready -> if (state.networks.isEmpty()) {
      Text(
        "No 2.4 GHz networks found — type the name below.",
        Modifier.padding(horizontal = 16.dp, vertical = 20.dp),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    } else {
      val shown = state.networks.take(VISIBLE_NETWORKS)
      Column(Modifier.fillMaxWidth()) {
        CardDivider()
        shown.forEach { network ->
          NetworkRow(network, phoneSsid, network.ssid == selectedSsid) { onSelect(network.ssid) }
        }
        if (state.networks.size > shown.size) {
          CardDivider()
          Text(
            "${shown.size} of ${state.networks.size} shown",
            Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
          )
        }
        if (state.cached) {
          Text(
            "Showing the last scan",
            Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
          )
        }
      }
    }
  }
}

@Composable
private fun NetworkRow(network: WifiNetwork, phoneSsid: String?, selected: Boolean, onSelect: () -> Unit) {
  Row(
    Modifier
      .fillMaxWidth()
      .clickable(onClick = onSelect)
      .background(if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceContainerLow)
      .padding(horizontal = 16.dp, vertical = 14.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    Icon(
      if (network.secured) PocketIcons.Lock else PocketIcons.Wifi,
      null,
      Modifier.size(18.dp),
      tint = if (selected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Spacer(Modifier.width(14.dp))
    Column(Modifier.weight(1f)) {
      Text(
        network.ssid,
        style = MaterialTheme.typography.bodyLarge,
        fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
      )
      Text(
        buildList {
          add(signalLabel(network.level))
          if (phoneSsid != null && network.ssid == phoneSsid) add("your phone's network")
          if (!network.secured) add("open")
        }.joinToString(" · "),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    }
    if (selected) {
      Icon(PocketIcons.Check, "Selected", Modifier.size(18.dp), tint = MaterialTheme.colorScheme.onPrimaryContainer)
    }
  }
}

/** Signal strength as words, because bars would need a second icon set. */
private fun signalLabel(level: Int): String = when {
  level >= -60 -> "Strong signal"
  level >= -75 -> "Fair signal"
  else -> "Weak signal"
}
