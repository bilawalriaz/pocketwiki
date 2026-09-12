package com.example.pocketwiki.ui.main

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
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.example.pocketwiki.device.BleState
import com.example.pocketwiki.device.DeviceNetworkState
import com.example.pocketwiki.device.PackCatalogState
import com.example.pocketwiki.device.CatalogPack
import com.example.pocketwiki.ui.icons.PocketIcons

/*
 * Packs: the catalogue of installable libraries, then the two manual routes.
 * Installs need the Bluetooth link, so that is stated once at the top rather
 * than discovered through a failure.
 */

@Composable
fun PacksScreen(
  ble: BleState,
  library: DeviceNetworkState,
  catalog: PackCatalogState,
  contentPadding: PaddingValues,
  onConnect: () -> Unit,
  onSync: () -> Unit,
  onInstall: (CatalogPack) -> Unit,
  onChooseFile: () -> Unit,
  onInstallUrl: (String) -> Unit,
  onCancelInstall: () -> Unit,
) {
  val linked = isLinked(ble)
  val busy = library as? DeviceNetworkState.Busy
  val ready = library as? DeviceNetworkState.Ready
  val installedNames = ready?.packs.orEmpty().mapTo(mutableSetOf()) { it.name }

  LazyColumn(
    modifier = Modifier.fillMaxSize(),
    contentPadding = contentPadding,
    verticalArrangement = Arrangement.spacedBy(12.dp),
  ) {
    if (!linked) {
      item("link") {
        NoticeCard(
          modifier = Modifier.animateItem(),
          icon = PocketIcons.BluetoothOff,
          title = "Not connected",
          action = { Button(onClick = onConnect, shape = MaterialTheme.shapes.medium) { Text("Connect") } },
        )
      }
    }

    busy?.let {
      item("busy") {
        QuietCard(Modifier.animateItem()) {
          BusyRow(it.message, it.progress)
          if (it.cancellable) {
            CardDivider()
            TextButton(onClick = onCancelInstall, Modifier.padding(horizontal = 8.dp, vertical = 4.dp)) {
              Text("Cancel install")
            }
          }
        }
      }
    }

    item("catalogue-heading") {
      SectionHeading(
        "Available packs",
        when {
          catalog.loading -> "Syncing the catalogue…"
          catalog.syncedAt != null -> "Catalogue synced at ${catalog.syncedAt}"
          else -> catalog.message
        },
      )
    }

    if (catalog.packs.isEmpty()) {
      item("catalogue-empty") {
        QuietCard(Modifier.animateItem()) {
          EmptyState(
            PocketIcons.CloudDownload,
            "No catalogue yet",
            "Connect to the internet and sync to see available packs.",
          ) {
            Button(onClick = onSync, enabled = !catalog.loading) { Text("Sync catalogue") }
          }
        }
      }
    } else {
      items(catalog.packs, key = { it.id }) { pack ->
        QuietCard(Modifier.animateItem()) {
          CatalogueRow(
            pack = pack,
            installed = pack.id in installedNames,
            fits = ready == null || pack.bytes <= ready.installable,
            enabled = linked && busy == null,
            onInstall = { onInstall(pack) },
          )
        }
      }
    }

    item("other-heading") { SectionHeading("Other ways to install") }

    item("other") {
      QuietCard {
        ManualRow(
          icon = PocketIcons.FolderOpen,
          title = "Choose a .pwp file",
          detail = "A .pwp saved on this phone",
          enabled = linked && busy == null,
          onClick = onChooseFile,
        )
        CardDivider()
        UrlRow(enabled = linked && busy == null, onSubmit = onInstallUrl)
      }
    }
  }
}

@Composable
private fun CatalogueRow(
  pack: CatalogPack,
  installed: Boolean,
  fits: Boolean,
  enabled: Boolean,
  onInstall: () -> Unit,
) {
  Row(
    Modifier.fillMaxWidth().padding(16.dp),
    verticalAlignment = Alignment.Top,
  ) {
    Column(Modifier.weight(1f)) {
      Row(verticalAlignment = Alignment.CenterVertically) {
        Text(pack.name, style = MaterialTheme.typography.bodyLarge)
        if (installed) {
          Spacer(Modifier.width(8.dp))
          Icon(PocketIcons.CheckCircle, "Installed", Modifier.size(16.dp), tint = MaterialTheme.colorScheme.primary)
        }
      }
      Spacer(Modifier.height(2.dp))
      Text(
        "${pack.articles} articles · v${pack.version} · ${formatBytes(pack.bytes)}",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
      Spacer(Modifier.height(4.dp))
      Text(
        pack.description,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        maxLines = 2,
        overflow = TextOverflow.Ellipsis,
      )
    }
    Spacer(Modifier.width(8.dp))
    if (!fits) {
      Text(
        "No space",
        Modifier.padding(start = 8.dp, top = 10.dp),
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    } else {
      TextButton(onClick = onInstall, enabled = enabled) {
        Text(if (installed) "Update" else "Install")
      }
    }
  }
}

@Composable
private fun ManualRow(
  icon: androidx.compose.ui.graphics.vector.ImageVector,
  title: String,
  detail: String,
  enabled: Boolean,
  onClick: () -> Unit,
) {
  Row(
    Modifier
      .fillMaxWidth()
      .clickable(enabled = enabled, onClick = onClick)
      .padding(horizontal = 16.dp, vertical = 16.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    Icon(
      icon,
      null,
      Modifier.size(20.dp),
      tint = if (enabled) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.outline,
    )
    Spacer(Modifier.width(14.dp))
    Column(Modifier.weight(1f)) {
      Text(title, style = MaterialTheme.typography.bodyLarge, color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.outline)
      Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
    Icon(PocketIcons.ChevronRight, null, Modifier.size(18.dp), tint = MaterialTheme.colorScheme.outline)
  }
}

@Composable
private fun UrlRow(enabled: Boolean, onSubmit: (String) -> Unit) {
  var expanded by remember { mutableStateOf(false) }
  var url by remember { mutableStateOf("") }
  Column(Modifier.fillMaxWidth()) {
    Row(
      Modifier
        .fillMaxWidth()
        .clickable(enabled = enabled) { expanded = !expanded }
        .padding(horizontal = 16.dp, vertical = 16.dp),
      verticalAlignment = Alignment.CenterVertically,
    ) {
      Icon(
        PocketIcons.Link,
        null,
        Modifier.size(20.dp),
        tint = if (enabled) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.outline,
      )
      Spacer(Modifier.width(14.dp))
      Column(Modifier.weight(1f)) {
        Text("From a web address", style = MaterialTheme.typography.bodyLarge, color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.outline)
        Text("From a link", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
      }
      Icon(
        if (expanded) PocketIcons.ExpandLess else PocketIcons.ExpandMore,
        null,
        Modifier.size(18.dp),
        tint = MaterialTheme.colorScheme.outline,
      )
    }
    AnimatedVisibility(
      visible = expanded,
      enter = expandVertically(disclosureTiming(), expandFrom = Alignment.Top) + fadeIn(effectsMotion()),
      exit = shrinkVertically(disclosureTiming(), shrinkTowards = Alignment.Top) + fadeOut(effectsMotion()),
      label = "urlFields",
    ) {
      Column(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 16.dp)) {
        OutlinedTextField(
          value = url,
          onValueChange = { url = it },
          modifier = Modifier.fillMaxWidth(),
          label = { Text("Pack URL") },
          placeholder = { Text("https://…/library.pwp") },
          singleLine = true,
        )
        Spacer(Modifier.height(12.dp))
        Button(
          onClick = { onSubmit(url.trim()) },
          modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
          enabled = enabled && url.startsWith("https://"),
        ) {
          Text("Download and install", style = MaterialTheme.typography.titleMedium)
        }
      }
    }
  }
}
