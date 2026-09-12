package com.example.pocketwiki

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.example.pocketwiki.device.ApConnectMonitor
import com.example.pocketwiki.device.BleProvisioner
import com.example.pocketwiki.device.PackCatalog
import com.example.pocketwiki.device.PhoneWifi
import com.example.pocketwiki.device.WifiScanner
import com.example.pocketwiki.theme.PocketWikiTheme
import com.example.pocketwiki.ui.main.MainScreen
import com.example.pocketwiki.ui.main.OnboardingScreen
import com.example.pocketwiki.ui.main.SetupPermission
import com.example.pocketwiki.ui.main.SetupStep
import com.example.pocketwiki.ui.main.effectsMotion
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class MainActivity : ComponentActivity() {
  private lateinit var ble: BleProvisioner
  private lateinit var phoneWifi: PhoneWifi
  private lateinit var wifiScanner: WifiScanner
  private lateinit var packCatalog: PackCatalog
  private lateinit var apMonitor: ApConnectMonitor

  /** True once a PocketWiki has been set up on this phone. */
  private var configured by mutableStateOf(false)
  private var step by mutableStateOf(SetupStep.Permissions)
  private var permissionsReady by mutableStateOf(false)

  private val setupPermissionLauncher =
    registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
      permissionsReady = appPermissionsReady()
      if (permissionsReady) ble.startAutoScan()
    }

  private val permissionLauncher =
    registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
      if (it.values.all { granted -> granted }) ble.scan()
    }

  private val wifiScanPermissionLauncher =
    registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
      if (granted) wifiScanner.scan()
    }

  private val packPicker = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
    if (uri == null) return@registerForActivityResult
    lifecycleScope.launch {
      val file = withContext(Dispatchers.IO) {
        val name = uri.lastPathSegment?.substringAfterLast('/')?.substringBeforeLast('.') ?: "library"
        val target = File(cacheDir, "picked.pwp")
        contentResolver.openInputStream(uri)?.use { input -> target.outputStream().use { output -> input.copyTo(output) } }
        target to name
      }
      ble.upload(file.first, file.second)
    }
  }

  /** Every permission the app needs, chosen per Android version. */
  private fun appPermissions(): Array<String> = buildList {
    if (Build.VERSION.SDK_INT >= 33) {
      add(Manifest.permission.BLUETOOTH_SCAN)
      add(Manifest.permission.BLUETOOTH_CONNECT)
      add(Manifest.permission.NEARBY_WIFI_DEVICES)
    } else if (Build.VERSION.SDK_INT >= 31) {
      add(Manifest.permission.BLUETOOTH_SCAN)
      add(Manifest.permission.BLUETOOTH_CONNECT)
      add(Manifest.permission.ACCESS_FINE_LOCATION)
    } else {
      add(Manifest.permission.ACCESS_FINE_LOCATION)
    }
  }.toTypedArray()

  private fun appPermissionsReady(): Boolean =
    appPermissions().all { ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED }

  private fun allGranted(vararg perms: String): Boolean =
    perms.all { ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED }

  /** Rows shown on the guided setup screen, reflecting live grant state. */
  private fun setupPermissionRows(): List<SetupPermission> {
    val bluetooth = listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    return if (Build.VERSION.SDK_INT >= 33) {
      listOf(
        SetupPermission("Bluetooth", "Find and set up your PocketWiki.", allGranted(*bluetooth.toTypedArray())),
        SetupPermission("Wi-Fi and nearby devices", "Read Wi-Fi names and join networks.", allGranted(Manifest.permission.NEARBY_WIFI_DEVICES)),
      )
    } else if (Build.VERSION.SDK_INT >= 31) {
      listOf(
        SetupPermission("Bluetooth", "Find and set up your PocketWiki.", allGranted(*bluetooth.toTypedArray())),
        SetupPermission("Location", "Required by Android to read Wi-Fi names.", allGranted(Manifest.permission.ACCESS_FINE_LOCATION)),
      )
    } else {
      listOf(
        SetupPermission("Location", "Find Bluetooth devices and read Wi-Fi names.", allGranted(Manifest.permission.ACCESS_FINE_LOCATION)),
      )
    }
  }

  private fun openUrl(url: String) {
    runCatching {
      startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
  }

  private fun openAppSettings() {
    runCatching {
      startActivity(
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null))
          .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
      )
    }
  }

  private fun markConfigured() {
    configured = true
    getSharedPreferences("pocketwiki", MODE_PRIVATE).edit().putBoolean("configured", true).apply()
  }

  /**
   * PocketWiki's whole feel is motion, but nothing makes the platform render it
   * at the panel's rate on its own: without a request, SurfaceFlinger treats the
   * window as ordinary content and only lifts the refresh rate while a finger is
   * moving — so a transition that plays after the touch ends is delivered at
   * 60 Hz on a 120 Hz screen. Asking the view hierarchy for the display's own
   * rate keeps the whole app, animation included, at the rate the user picked in
   * Settings, and drops back automatically when they pick a slower one.
   */
  @Suppress("DEPRECATION")
  private fun requestDisplayRefreshRate() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
    val rate = display?.mode?.refreshRate ?: return
    if (rate <= 0f) return
    if (Build.VERSION.SDK_INT >= 34) {
      window.decorView.setRequestedFrameRate(rate)
    } else {
      window.attributes = window.attributes.apply { preferredRefreshRate = rate }
    }
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    phoneWifi = PhoneWifi(this)
    ble = BleProvisioner(this, lifecycleScope, phoneWifi)
    wifiScanner = WifiScanner(this)
    packCatalog = PackCatalog(this, lifecycleScope)
    apMonitor = ApConnectMonitor(this, ble.knownApSsid, ble.transferActive) { openUrl("http://192.168.4.1") }
    apMonitor.start()
    configured = getSharedPreferences("pocketwiki", MODE_PRIVATE).getBoolean("configured", false)
    permissionsReady = appPermissionsReady()
    step = if (permissionsReady) SetupStep.Device else SetupStep.Permissions
    if (permissionsReady) ble.startAutoScan()
    enableEdgeToEdge()

    setContent {
      val bleState by ble.state.collectAsState()
      val networkState by ble.libraryState.collectAsState()
      val wifiStatus by ble.wifiStatus.collectAsState()
      val wifiScanState by wifiScanner.state.collectAsState()
      val packCatalogState by packCatalog.state.collectAsState()
      val phoneNetwork by phoneWifi.state.collectAsState()
      val snackbarHost = remember { SnackbarHostState() }
      val scope = rememberCoroutineScope()
      val notify: (String) -> Unit = { message ->
        scope.launch {
          snackbarHost.currentSnackbarData?.dismiss()
          snackbarHost.showSnackbar(message)
        }
      }

      PocketWikiTheme {
        val rootFade = effectsMotion<Float>()
        AnimatedContent(
          targetState = configured,
          transitionSpec = { fadeIn(rootFade) togetherWith fadeOut(rootFade) },
          label = "root",
        ) { onboarded ->
          if (!onboarded) {
            LaunchedEffect(permissionsReady) {
              if (permissionsReady && step == SetupStep.Permissions) step = SetupStep.Device
            }
            OnboardingScreen(
              step = step,
              permissions = setupPermissionRows(),
              ble = bleState,
              wifi = wifiStatus,
              phone = phoneNetwork,
              scanState = wifiScanState,
              library = networkState,
              snackbarHost = snackbarHost,
              onStep = { step = it },
              onRequestPermissions = { setupPermissionLauncher.launch(appPermissions()) },
              onOpenAppSettings = ::openAppSettings,
              onConnect = ble::ensureConnect,
              onScanWifi = {
                if (wifiScanner.hasPermission()) wifiScanner.scan()
                else wifiScanPermissionLauncher.launch(wifiScanner.permission)
              },
              onSaveWifi = ble::sendCredentials,
              onOpenUrl = ::openUrl,
              onNotify = notify,
              onFinish = ::markConfigured,
            )
          } else {
            MainScreen(
              ble = bleState,
              library = networkState,
              wifi = wifiStatus,
              phone = phoneNetwork,
              scanState = wifiScanState,
              catalog = packCatalogState,
              permissionsGranted = permissionsReady,
              snackbarHost = snackbarHost,
              onConnect = ble::ensureConnect,
              onCancelInstall = ble::cancelInstall,
              onOpenUrl = ::openUrl,
              onRemove = { ble.action("delete", it) },
              onRefresh = ble::refresh,
              onSyncCatalog = packCatalog::refresh,
              onInstall = ble::install,
              onChooseFile = { packPicker.launch("application/octet-stream") },
              onInstallUrl = ble::downloadThenUpload,
              onScanWifi = {
                if (wifiScanner.hasPermission()) wifiScanner.scan()
                else wifiScanPermissionLauncher.launch(wifiScanner.permission)
              },
              onSaveWifi = ble::sendCredentials,
              onOpenAppSettings = ::openAppSettings,
              onNotify = notify,
            )
          }
        }
      }
    }
  }

  override fun onResume() {
    super.onResume()
    requestDisplayRefreshRate()
    // The user may have changed permissions in system settings while away.
    permissionsReady = appPermissionsReady()
    phoneWifi.refresh()
    if (permissionsReady) ble.startAutoScan()
  }

  override fun onDestroy() {
    ble.close()
    phoneWifi.close()
    wifiScanner.close()
    apMonitor.stop()
    super.onDestroy()
  }
}
