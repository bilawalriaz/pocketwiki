package com.bilawalriaz.pocketwiki.ui.main

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.BorderStroke
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
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
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
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.bilawalriaz.pocketwiki.device.BleState
import com.bilawalriaz.pocketwiki.device.DeviceNetworkState
import com.bilawalriaz.pocketwiki.device.PackCatalogState
import com.bilawalriaz.pocketwiki.device.CatalogPack
import com.bilawalriaz.pocketwiki.ui.icons.PocketIcons

/*
 * Packs: the catalogue of installable libraries, then the two manual routes.
 * Installs need the Bluetooth link, so that is stated once at the top rather
 * than discovered through a failure.
 *
 * Packs are chosen, not fired: the catalogue is a selection list and one
 * action installs the lot. The device stores tens of packs but has room for a
 * few, so the running total against the free space it reported is on screen
 * before anything starts, and a pack that cannot fit cannot be chosen.
 */

@Composable
fun PacksScreen(
  ble: BleState,
  library: DeviceNetworkState,
  catalog: PackCatalogState,
  chosen: Set<String>,
  onChoose: (Set<String>) -> Unit,
  contentPadding: PaddingValues,
  onConnect: () -> Unit,
  onSync: () -> Unit,
  onInstall: (List<CatalogPack>) -> Unit,
  onChooseFile: () -> Unit,
  onInstallUrl: (String) -> Unit,
  onCancelInstall: () -> Unit,
) {
  val linked = isLinked(ble)
  val busy = library as? DeviceNetworkState.Busy
  val ready = library as? DeviceNetworkState.Ready
  val installedNames = ready?.packs.orEmpty().mapTo(mutableSetOf()) { it.name }
  val installable = ready?.installable
  val catalogPacks = catalog.packs
  val listState = rememberLazyListState()
  /* Only packs the catalogue still offers can be installed, so the selection
   * is read back through the catalogue rather than trusted on its own. */
  val selected = catalogPacks.filter { it.id in chosen }
  val selectedBytes = selected.sumOf { it.bytes }
  val fits = installable == null || selectedBytes <= installable
  val choosing = linked && busy == null

  /* Work starts from the bottom of a long list — the install bar is the last
   * thing on screen — while its progress card is the first thing in it. Bring
   * the list back to the top so the progress, and the Cancel button under it,
   * are in view rather than several hundred rows above the user. */
  LaunchedEffect(busy != null) {
    if (busy != null) listState.animateScrollToItem(0)
  }

  fun toggle(id: String) {
    onChoose(if (id in chosen) chosen - id else chosen + id)
  }

  Column(Modifier.fillMaxSize()) {
    LazyColumn(
      state = listState,
      modifier = Modifier.weight(1f),
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

      if (catalogPacks.isEmpty()) {
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
        items(catalogPacks, key = { it.id }) { pack ->
          QuietCard(Modifier.animateItem()) {
            CatalogueRow(
              pack = pack,
              installed = pack.id in installedNames,
              /* Each pack is judged on its own against the space left, so a
               * pack that fits today stays installable on its own even when
               * the current selection no longer leaves room for it. */
              fits = installable == null || pack.bytes <= installable,
              chosen = pack.id in chosen,
              enabled = choosing,
              onToggle = { toggle(pack.id) },
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
            enabled = choosing,
            onClick = onChooseFile,
          )
          CardDivider()
          UrlRow(enabled = choosing, onSubmit = onInstallUrl)
        }
      }
    }

    if (selected.isNotEmpty()) {
      SelectionBar(
        count = selected.size,
        bytes = selectedBytes,
        available = installable,
        fits = fits,
        enabled = choosing,
        onClear = { onChoose(emptySet()) },
        onInstall = {
          val batch = selected
          onChoose(emptySet())
          onInstall(batch)
        },
      )
    }
  }
}

/** The running total for a selection, and the one action that spends it. */
@Composable
private fun SelectionBar(
  count: Int,
  bytes: Long,
  available: Long?,
  fits: Boolean,
  enabled: Boolean,
  onClear: () -> Unit,
  onInstall: () -> Unit,
) {
  Surface(
    color = MaterialTheme.colorScheme.surfaceContainerLow,
    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
  ) {
    Row(
      Modifier.fillMaxWidth().padding(start = 16.dp, end = 12.dp, top = 10.dp, bottom = 10.dp),
      verticalAlignment = Alignment.CenterVertically,
    ) {
      Column(Modifier.weight(1f)) {
        Text(
          if (count == 1) "1 pack · ${formatBytes(bytes)}" else "$count packs · ${formatBytes(bytes)}",
          style = MaterialTheme.typography.bodyMedium,
        )
        if (available != null) {
          Text(
            if (fits) {
              "${formatBytes(available)} available"
            } else {
              "${formatBytes(bytes - available)} more than the ${formatBytes(available)} available"
            },
            style = MaterialTheme.typography.bodySmall,
            color = if (fits) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.error,
          )
        }
      }
      TextButton(onClick = onClear, enabled = enabled) { Text("Clear") }
      Button(
        onClick = onInstall,
        enabled = enabled && fits,
        shape = MaterialTheme.shapes.medium,
      ) {
        Text(if (count == 1) "Install" else "Install $count")
      }
    }
  }
}

@Composable
private fun CatalogueRow(
  pack: CatalogPack,
  installed: Boolean,
  fits: Boolean,
  chosen: Boolean,
  enabled: Boolean,
  onToggle: () -> Unit,
) {
  Row(
    Modifier
      .fillMaxWidth()
      .clickable(enabled = enabled && fits, onClick = onToggle)
      .padding(start = 8.dp, end = 16.dp, top = 8.dp, bottom = 8.dp),
    verticalAlignment = Alignment.Top,
  ) {
    Checkbox(checked = chosen, onCheckedChange = { onToggle() }, enabled = enabled && fits)
    Spacer(Modifier.width(6.dp))
    Column(Modifier.weight(1f).padding(top = 12.dp, bottom = 12.dp)) {
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
        if (fits) pack.description else "Needs ${formatBytes(pack.bytes)}; PocketWiki has no room for it.",
        style = MaterialTheme.typography.bodySmall,
        color = if (fits) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.error,
        maxLines = 2,
        overflow = TextOverflow.Ellipsis,
      )
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
