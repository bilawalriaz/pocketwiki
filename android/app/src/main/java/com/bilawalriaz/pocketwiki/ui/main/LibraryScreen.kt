package com.bilawalriaz.pocketwiki.ui.main

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.bilawalriaz.pocketwiki.device.BleState
import com.bilawalriaz.pocketwiki.device.DeviceNetworkState
import com.bilawalriaz.pocketwiki.device.DeviceWifiStatus
import com.bilawalriaz.pocketwiki.device.PackInfo
import com.bilawalriaz.pocketwiki.device.CatalogPack
import com.bilawalriaz.pocketwiki.ui.icons.PocketIcons

/*
 * Library: what the device is holding. The device's own state leads, the
 * dashboard hand-off follows, and the installed packs are the content.
 */

@Composable
fun LibraryScreen(
  ble: BleState,
  library: DeviceNetworkState,
  wifi: DeviceWifiStatus?,
  catalog: List<CatalogPack>,
  contentPadding: PaddingValues,
  onConnect: () -> Unit,
  onCancelInstall: () -> Unit,
  onOpenUrl: (String) -> Unit,
  onRemove: (String) -> Unit,
  onBrowsePacks: () -> Unit,
) {
  var pendingRemoval by remember { mutableStateOf<PackInfo?>(null) }
  val linked = isLinked(ble)

  // With nothing connected there is nothing to list, so the screen becomes one
  // centred invitation instead of a card restating the connection state.
  val empty = !linked && library !is DeviceNetworkState.Busy
  AnimatedContent(
    targetState = empty,
    transitionSpec = { fadeThrough() },
    contentAlignment = Alignment.Center,
    modifier = Modifier.fillMaxSize(),
    label = "libraryBody",
  ) { showEmpty ->
    if (showEmpty) {
      LibraryEmptyState(ble, contentPadding, onConnect)
    } else {
      LibraryBody(
        ble, library, wifi, catalog, contentPadding,
        onConnect, onCancelInstall, onOpenUrl, onBrowsePacks,
        onAskToRemove = { pendingRemoval = it },
      )
    }
  }

  pendingRemoval?.let { pack ->
    RemovePackDialog(
      pack = pack,
      displayName = displayPackName(pack.name, catalog),
      onDismiss = { pendingRemoval = null },
      onConfirm = {
        pendingRemoval = null
        onRemove(pack.name)
      },
    )
  }
}

@Composable
private fun LibraryBody(
  ble: BleState,
  library: DeviceNetworkState,
  wifi: DeviceWifiStatus?,
  catalog: List<CatalogPack>,
  contentPadding: PaddingValues,
  onConnect: () -> Unit,
  onCancelInstall: () -> Unit,
  onOpenUrl: (String) -> Unit,
  onBrowsePacks: () -> Unit,
  onAskToRemove: (PackInfo) -> Unit,
) {
  val linked = isLinked(ble)
  LazyColumn(
    modifier = Modifier.fillMaxSize(),
    contentPadding = contentPadding,
    verticalArrangement = Arrangement.spacedBy(12.dp),
  ) {
    item("device") {
      DeviceCard(ble, wifi, onConnect, onOpenUrl)
    }

    when {
      library is DeviceNetworkState.Busy -> item("busy") {
        QuietCard {
          BusyRow(library.message, library.progress)
          if (library.cancellable) {
            CardDivider()
            TextButton(onClick = onCancelInstall, Modifier.padding(horizontal = 8.dp, vertical = 4.dp)) {
              Text("Cancel install")
            }
          }
        }
      }

      library is DeviceNetworkState.Ready -> {
        item("storage") {
          QuietCard { StorageMeter(library.used, library.total, library.installable) }
        }
        item("packs-heading") {
          SectionHeading("Installed packs", "${library.packs.size} packs")
        }
        items(library.packs, key = { it.name }) { pack ->
          QuietCard(Modifier.animateItem()) { PackRow(pack, catalog, onRemove = { onAskToRemove(pack) }) }
        }
        item("browse") {
          OutlinedButton(
            onClick = onBrowsePacks,
            modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
            shape = MaterialTheme.shapes.medium,
          ) {
            Icon(PocketIcons.AddPacks, null, Modifier.size(20.dp))
            Spacer(Modifier.width(10.dp))
            Text("Get another pack", style = MaterialTheme.typography.titleMedium)
          }
        }
      }

      linked && library is DeviceNetworkState.Error -> item("error") {
        NoticeCard(
          icon = PocketIcons.Error,
          title = "PocketWiki didn't answer",
          body = library.message,
          iconTint = MaterialTheme.colorScheme.error,
          action = { TextButton(onClick = onConnect) { Text("Read the library again") } },
        )
      }

      linked -> item("reading") {
        QuietCard { BusyRow("Reading your library…") }
      }
    }
  }
}

@Composable
private fun LibraryEmptyState(ble: BleState, contentPadding: PaddingValues, onConnect: () -> Unit) {
  val summary = deviceSummary(ble, null)
  val working = ble is BleState.Scanning || ble is BleState.Connecting || ble is BleState.Reconnecting
  Box(
    Modifier.fillMaxSize().padding(contentPadding).padding(horizontal = 24.dp),
    contentAlignment = Alignment.Center,
  ) {
    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
      BrandMark(64.dp)
      Spacer(Modifier.height(24.dp))
      Text(
        summary.title,
        style = MaterialTheme.typography.headlineSmall,
        color = summary.tone.color(),
        textAlign = TextAlign.Center,
      )
      Spacer(Modifier.height(10.dp))
      Text(
        summary.detail,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = TextAlign.Center,
      )
      Spacer(Modifier.height(24.dp))
      if (working) {
        CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.5.dp)
      } else {
        Button(
          onClick = onConnect,
          modifier = Modifier.heightIn(min = 52.dp),
          shape = MaterialTheme.shapes.medium,
        ) {
          Text(if (ble is BleState.Error) "Try again" else "Connect", style = MaterialTheme.typography.titleMedium)
        }
      }
    }
  }
}

@Composable
private fun DeviceCard(
  ble: BleState,
  wifi: DeviceWifiStatus?,
  onConnect: () -> Unit,
  onOpenUrl: (String) -> Unit,
) {
  val summary = deviceSummary(ble, wifi)
  val working = ble is BleState.Scanning || ble is BleState.Connecting ||
    ble is BleState.Reconnecting || ble is BleState.Sending
  QuietCard {
    Row(
      Modifier.fillMaxWidth().padding(16.dp),
      verticalAlignment = Alignment.CenterVertically,
    ) {
      BrandMark(46.dp)
      Spacer(Modifier.width(14.dp))
      Column(Modifier.weight(1f)) {
        Text("PocketWiki", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(2.dp))
        Text(summary.title, style = MaterialTheme.typography.bodyMedium, color = summary.tone.animatedColor())
        Text(
          summary.detail,
          style = MaterialTheme.typography.bodySmall,
          color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
      }
      if (working) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.5.dp)
    }
    CardDivider()
    Row(
      Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
      verticalAlignment = Alignment.CenterVertically,
      horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
      when {
        isLinked(ble) -> Button(onClick = { onOpenUrl(dashboardUrl(wifi)) }, shape = MaterialTheme.shapes.medium) {
          Icon(PocketIcons.OpenInNew, null, Modifier.size(18.dp))
          Spacer(Modifier.width(8.dp))
          Text("Read on PocketWiki")
        }
        working -> StatusLine(summary.title, summary.tone, Modifier.padding(start = 8.dp))
        else -> Button(onClick = onConnect, shape = MaterialTheme.shapes.medium) {
          Text(if (ble is BleState.Error) "Try again" else "Connect")
        }
      }
    }
  }
}

@Composable
private fun PackRow(pack: PackInfo, catalog: List<CatalogPack>, onRemove: () -> Unit) {
  Row(
    Modifier.fillMaxWidth().padding(start = 16.dp, end = 4.dp, top = 10.dp, bottom = 10.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    Column(Modifier.weight(1f)) {
      Text(displayPackName(pack.name, catalog), style = MaterialTheme.typography.bodyLarge)
      Text(
        if (pack.bytes > 0) "${pack.articles} articles · ${formatBytes(pack.bytes)}" else "${pack.articles} articles · read from flash",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    }
    if (pack.name == STARTER_PACK) {
      Text(
        "Built in",
        Modifier.padding(horizontal = 12.dp),
        style = MaterialTheme.typography.labelMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    } else {
      IconButton(onClick = onRemove) {
        Icon(PocketIcons.Delete, "Remove ${displayPackName(pack.name, catalog)}", Modifier.size(20.dp))
      }
    }
  }
}

@Composable
private fun RemovePackDialog(pack: PackInfo, displayName: String, onDismiss: () -> Unit, onConfirm: () -> Unit) {
  AlertDialog(
    onDismissRequest = onDismiss,
    title = { Text("Remove “$displayName”?", style = MaterialTheme.typography.titleLarge) },
    text = {
      Text(
        "Frees ${formatBytes(pack.bytes)}. You can install it again from Packs.",
        style = MaterialTheme.typography.bodyMedium,
      )
    },
    confirmButton = {
      TextButton(onClick = onConfirm) {
        Text("Remove", color = MaterialTheme.colorScheme.error)
      }
    },
    dismissButton = { TextButton(onClick = onDismiss) { Text("Keep") } },
  )
}
