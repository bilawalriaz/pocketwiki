package com.bilawalriaz.pocketwiki.ui.main

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import com.bilawalriaz.pocketwiki.device.BleState
import com.bilawalriaz.pocketwiki.device.DeviceNetworkState
import com.bilawalriaz.pocketwiki.device.DeviceWifiStatus
import com.bilawalriaz.pocketwiki.device.PackCatalogState
import com.bilawalriaz.pocketwiki.device.PhoneNetwork
import com.bilawalriaz.pocketwiki.device.WifiScanState
import com.bilawalriaz.pocketwiki.device.CatalogPack
import com.bilawalriaz.pocketwiki.ui.icons.PocketIcons

/*
 * The app shell: three destinations — what the device holds, what can be added,
 * and how it is connected. Material's navigation bar on a phone, a rail once
 * there is width for one, and system Back returning to Library from anywhere.
 */

private enum class AppTab(val label: String, val icon: ImageVector) {
  Library("Library", PocketIcons.Library),
  Packs("Packs", PocketIcons.AddPacks),
  Device("Device", PocketIcons.Device),
}

/** Content inset for every scrolling screen, so cards align with the app bar. */
private val ScreenPadding = PaddingValues(16.dp)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(
  ble: BleState,
  library: DeviceNetworkState,
  wifi: DeviceWifiStatus?,
  phone: PhoneNetwork?,
  scanState: WifiScanState,
  catalog: PackCatalogState,
  permissionsGranted: Boolean,
  snackbarHost: SnackbarHostState,
  onConnect: () -> Unit,
  onCancelInstall: () -> Unit,
  onOpenUrl: (String) -> Unit,
  onRemove: (String) -> Unit,
  onRefresh: () -> Unit,
  onSyncCatalog: () -> Unit,
  onInstall: (List<CatalogPack>) -> Unit,
  onChooseFile: () -> Unit,
  onInstallUrl: (String) -> Unit,
  onScanWifi: () -> Unit,
  onSaveWifi: (String, String) -> Unit,
  onOpenAppSettings: () -> Unit,
  onNotify: (String) -> Unit,
) {
  var index by rememberSaveable { mutableStateOf(0) }
  val tab = AppTab.entries[index.coerceIn(AppTab.entries.indices)]
  val linked = isLinked(ble)
  /* The chosen packs live here, not in the Packs screen: switching tabs
   * disposes that screen, and a selection the user just built must survive a
   * look at another destination. */
  var chosenPacks by rememberSaveable { mutableStateOf(emptySet<String>()) }

  BackHandler(enabled = index != 0) { index = 0 }

  BoxWithConstraints(Modifier.fillMaxSize()) {
    val expanded = maxWidth >= 640.dp

    Scaffold(
      containerColor = MaterialTheme.colorScheme.background,
      snackbarHost = { SnackbarHost(snackbarHost) },
      topBar = {
        TopAppBar(
          title = { Text(tab.label) },
          colors = TopAppBarDefaults.topAppBarColors(
            containerColor = MaterialTheme.colorScheme.background,
            titleContentColor = MaterialTheme.colorScheme.onSurface,
            actionIconContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
          ),
          actions = {
            if (tab == AppTab.Library) {
              IconButton(onClick = onRefresh, enabled = linked) {
                Icon(PocketIcons.Refresh, "Re-read the library from PocketWiki", Modifier.size(22.dp))
              }
            }
            if (tab == AppTab.Packs) {
              IconButton(onClick = onSyncCatalog, enabled = !catalog.loading) {
                Icon(PocketIcons.Sync, "Sync the pack catalogue", Modifier.size(22.dp))
              }
            }
          },
        )
      },
      bottomBar = {
        if (!expanded) {
          NavigationBar(containerColor = MaterialTheme.colorScheme.surfaceContainerLow) {
            AppTab.entries.forEachIndexed { i, item ->
              NavigationBarItem(
                selected = i == index,
                onClick = { index = i },
                icon = { Icon(item.icon, null, Modifier.size(24.dp)) },
                label = { Text(item.label) },
              )
            }
          }
        }
      },
    ) { insets ->
      Row(Modifier.fillMaxSize().padding(insets)) {
        if (expanded) {
          NavigationRail(containerColor = MaterialTheme.colorScheme.surfaceContainerLow) {
            AppTab.entries.forEachIndexed { i, item ->
              NavigationRailItem(
                selected = i == index,
                onClick = { index = i },
                icon = { Icon(item.icon, item.label, Modifier.size(24.dp)) },
                label = { Text(item.label) },
              )
            }
          }
        }
        Box(Modifier.weight(1f).fillMaxHeight()) {
          AnimatedContent(
            targetState = tab,
            transitionSpec = { fadeThrough() },
            contentAlignment = Alignment.TopStart,
            label = "destination",
          ) { destination ->
            when (destination) {
              AppTab.Library -> LibraryScreen(
                ble = ble,
                library = library,
                wifi = wifi,
                catalog = catalog.packs,
                contentPadding = ScreenPadding,
                onConnect = onConnect,
                onCancelInstall = onCancelInstall,
                onOpenUrl = onOpenUrl,
                onRemove = onRemove,
                onBrowsePacks = { index = AppTab.Packs.ordinal },
              )

              AppTab.Packs -> PacksScreen(
                ble = ble,
                library = library,
                catalog = catalog,
                chosen = chosenPacks,
                onChoose = { chosenPacks = it },
                contentPadding = ScreenPadding,
                onConnect = onConnect,
                onSync = onSyncCatalog,
                onInstall = onInstall,
                onChooseFile = onChooseFile,
                onInstallUrl = onInstallUrl,
                onCancelInstall = onCancelInstall,
              )

              AppTab.Device -> DeviceScreen(
                ble = ble,
                wifi = wifi,
                phone = phone,
                scanState = scanState,
                library = library,
                permissionsGranted = permissionsGranted,
                contentPadding = ScreenPadding,
                onConnect = onConnect,
                onScanWifi = onScanWifi,
                onSaveWifi = onSaveWifi,
                onOpenUrl = onOpenUrl,
                onOpenAppSettings = onOpenAppSettings,
                onNotify = onNotify,
              )
            }
          }
        }
      }
    }
  }
}
