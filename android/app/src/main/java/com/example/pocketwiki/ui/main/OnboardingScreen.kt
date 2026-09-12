package com.example.pocketwiki.ui.main

import android.content.ClipData
import android.content.ClipboardManager
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.Crossfade
import androidx.compose.animation.animateColorAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.example.pocketwiki.device.BleState
import com.example.pocketwiki.device.DeviceNetworkState
import com.example.pocketwiki.device.DeviceWifiStatus
import com.example.pocketwiki.device.PhoneNetwork
import com.example.pocketwiki.device.WifiScanState
import com.example.pocketwiki.ui.icons.PocketIcons

/*
 * First run: one guided path from an unpowered box to a readable library.
 * Device, Wi-Fi, then Library, with the progress rail pinned under the header
 * and the single next action pinned above the navigation bar — neither scrolls
 * away, so the user always knows where they are and what happens next.
 */

enum class SetupStep(val label: String) {
  Permissions("Permissions"),
  Device("Device"),
  Wifi("Wi-Fi"),
  Library("Library"),
}

/** One permission the guided setup asks for, with its live grant state. */
data class SetupPermission(val label: String, val detail: String, val granted: Boolean)

@Composable
fun OnboardingScreen(
  step: SetupStep,
  permissions: List<SetupPermission>,
  ble: BleState,
  wifi: DeviceWifiStatus?,
  phone: PhoneNetwork?,
  scanState: WifiScanState,
  library: DeviceNetworkState,
  snackbarHost: SnackbarHostState,
  onStep: (SetupStep) -> Unit,
  onRequestPermissions: () -> Unit,
  onOpenAppSettings: () -> Unit,
  onConnect: () -> Unit,
  onScanWifi: () -> Unit,
  onSaveWifi: (String, String) -> Unit,
  onOpenUrl: (String) -> Unit,
  onNotify: (String) -> Unit,
  onFinish: () -> Unit,
) {
  var ssid by remember { mutableStateOf("") }
  var password by remember { mutableStateOf("") }

  val canGoBack = step == SetupStep.Wifi || step == SetupStep.Library
  BackHandler(enabled = canGoBack) {
    onStep(if (step == SetupStep.Library) SetupStep.Wifi else SetupStep.Device)
  }

  Scaffold(
    containerColor = MaterialTheme.colorScheme.background,
    snackbarHost = { SnackbarHost(snackbarHost) },
    topBar = {
      Column(Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.background).statusBarsPadding()) {
        SetupHeader(step, showBack = canGoBack) {
          onStep(if (step == SetupStep.Library) SetupStep.Wifi else SetupStep.Device)
        }
        if (step != SetupStep.Permissions) {
          StepRail(step, onStep)
        }
      }
    },
    bottomBar = {
      SetupActionBar(
        step = step,
        permissions = permissions,
        ble = ble,
        ssid = ssid,
        onRequestPermissions = onRequestPermissions,
        onOpenAppSettings = onOpenAppSettings,
        onConnect = onConnect,
        onSaveWifi = { onSaveWifi(ssid, password) },
        onContinue = { onStep(if (step == SetupStep.Device) SetupStep.Wifi else SetupStep.Library) },
        onFinish = onFinish,
      )
    },
  ) { insets ->
    AnimatedContent(
      targetState = step,
      transitionSpec = { sharedAxisX(forward = targetState.ordinal > initialState.ordinal) },
      contentAlignment = Alignment.TopStart,
      modifier = Modifier.fillMaxSize().padding(insets),
      label = "setupStep",
    ) { current ->
      Column(
        Modifier
          .fillMaxSize()
          .verticalScroll(rememberScrollState())
          .padding(horizontal = 24.dp),
      ) {
        Spacer(Modifier.height(20.dp))
        when (current) {
          SetupStep.Permissions -> PermissionsStep(permissions)
          SetupStep.Device -> DeviceStep(ble, wifi)
          SetupStep.Wifi -> WifiStep(scanState, phone, ssid, { ssid = it }, password, { password = it }, ble, onScanWifi)
          SetupStep.Library -> LibraryStep(ble, wifi, library, onOpenUrl, onNotify)
        }
        Spacer(Modifier.height(28.dp))
      }
    }
  }
}

@Composable
private fun SetupHeader(step: SetupStep, showBack: Boolean, onBack: () -> Unit) {
  Row(
    Modifier.fillMaxWidth().height(60.dp).padding(start = 6.dp, end = 24.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    if (showBack) {
      IconButton(onClick = onBack) { Icon(PocketIcons.ArrowBack, "Back") }
    } else {
      Spacer(Modifier.width(12.dp))
    }
    BrandMark(34.dp)
    Spacer(Modifier.width(12.dp))
    Column(Modifier.weight(1f)) {
      Text("PocketWiki", style = MaterialTheme.typography.titleMedium)
      Text(step.label, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
  }
}

/** Device → Wi-Fi → Library, always visible, with completed steps tappable. */
@Composable
private fun StepRail(current: SetupStep, onStep: (SetupStep) -> Unit) {
  val steps = listOf(SetupStep.Device, SetupStep.Wifi, SetupStep.Library)
  val index = steps.indexOf(current).coerceAtLeast(0)
  val fill = effectsMotion<Color>()
  val onColor = MaterialTheme.colorScheme.primary
  val offColor = MaterialTheme.colorScheme.surfaceVariant
  val onLabel = MaterialTheme.colorScheme.primary
  val offLabel = MaterialTheme.colorScheme.onSurfaceVariant
  Column(Modifier.fillMaxWidth().padding(start = 24.dp, end = 24.dp, top = 4.dp)) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
      steps.forEachIndexed { i, _ ->
        Box(
          Modifier
            .weight(1f)
            .height(4.dp)
            .background(
              animateColorAsState(if (i <= index) onColor else offColor, fill, label = "stepFill").value,
              CircleShape,
            ),
        )
      }
    }
    Spacer(Modifier.height(8.dp))
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
      steps.forEachIndexed { i, s ->
        val done = i < index
        Box(
          Modifier
            .weight(1f)
            .heightIn(min = 48.dp)
            .then(if (done) Modifier.clickable { onStep(s) } else Modifier),
          contentAlignment = Alignment.Center,
        ) {
          Text(
            s.label,
            style = MaterialTheme.typography.labelMedium,
            textAlign = TextAlign.Center,
            color = animateColorAsState(if (i == index) onLabel else offLabel, fill, label = "stepLabel").value,
          )
        }
      }
    }
  }
}

@Composable
private fun SetupActionBar(
  step: SetupStep,
  permissions: List<SetupPermission>,
  ble: BleState,
  ssid: String,
  onRequestPermissions: () -> Unit,
  onOpenAppSettings: () -> Unit,
  onConnect: () -> Unit,
  onSaveWifi: () -> Unit,
  onContinue: () -> Unit,
  onFinish: () -> Unit,
) {
  val granted = permissions.all { it.granted }
  val linked = isLinked(ble)
  SetupActionSurface {
    Button(
      onClick = when (step) {
        SetupStep.Permissions -> if (granted) onContinue else onRequestPermissions
        SetupStep.Device -> if (linked) onContinue else onConnect
        SetupStep.Wifi -> onSaveWifi
        SetupStep.Library -> onFinish
      },
      modifier = Modifier.fillMaxWidth().heightIn(min = 54.dp),
      enabled = when (step) {
        SetupStep.Permissions -> true
        SetupStep.Device -> ble !is BleState.Scanning && ble !is BleState.Connecting && ble !is BleState.Reconnecting
        SetupStep.Wifi -> linked && ssid.isNotBlank() && ble !is BleState.Sending
        SetupStep.Library -> true
      },
      shape = MaterialTheme.shapes.medium,
    ) {
      Crossfade(
        targetState = actionLabel(step, granted, linked, ble),
        animationSpec = effectsMotion(),
        label = "setupAction",
      ) { label ->
        Text(label, style = MaterialTheme.typography.titleMedium)
      }
    }
    if (step == SetupStep.Permissions && !granted) {
      Spacer(Modifier.height(4.dp))
      TextButton(onClick = onOpenAppSettings) { Text("Open app settings instead") }
    }
    if (step == SetupStep.Wifi) {
      if (ble is BleState.Error) {
        Spacer(Modifier.height(4.dp))
        TextButton(onClick = onConnect) { Text("Reconnect over Bluetooth") }
      }
      if (ssid.isBlank()) {
        Spacer(Modifier.height(8.dp))
        Text(
          "Choose a network above, or type its name.",
          Modifier.fillMaxWidth(),
          style = MaterialTheme.typography.bodySmall,
          color = MaterialTheme.colorScheme.onSurfaceVariant,
          textAlign = TextAlign.Center,
        )
      }
    }
  }
}

@Composable
private fun SetupActionSurface(content: @Composable () -> Unit) {
  Column(
    Modifier
      .fillMaxWidth()
      .background(MaterialTheme.colorScheme.background)
      .navigationBarsPadding()
      .padding(horizontal = 24.dp, vertical = 12.dp),
  ) { content() }
}

private fun actionLabel(step: SetupStep, granted: Boolean, linked: Boolean, ble: BleState): String = when (step) {
  SetupStep.Permissions -> if (granted) "Continue" else "Allow access"
  SetupStep.Device -> when {
    linked -> "Continue"
    ble is BleState.Scanning -> "Looking for PocketWiki…"
    ble is BleState.Reconnecting -> "Reconnecting…"
    ble is BleState.Connecting -> "Connecting…"
    ble is BleState.Found -> "Connect"
    ble is BleState.Error -> "Try again"
    else -> "Find PocketWiki"
  }
  SetupStep.Wifi -> if (ble is BleState.Sending) "Saving…" else "Save Wi-Fi details"
  SetupStep.Library -> "Finish setup"
}

@Composable
private fun PermissionsStep(permissions: List<SetupPermission>) {
  StepHeading(
    "A few permissions",
    "Used to find your PocketWiki and read Wi-Fi network names. Nothing leaves your phone but the details you type.",
  )
  Spacer(Modifier.height(20.dp))
  permissions.forEach { permission ->
    QuietCard {
      Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
          Text(permission.label, style = MaterialTheme.typography.bodyLarge)
          Spacer(Modifier.height(3.dp))
          Text(permission.detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Spacer(Modifier.width(12.dp))
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
          Icon(
            if (permission.granted) PocketIcons.CheckCircle else PocketIcons.Warning,
            null,
            Modifier.size(20.dp),
            tint = if (permission.granted) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
          )
          Spacer(Modifier.height(4.dp))
          Text(
            if (permission.granted) "Granted" else "Needed",
            style = MaterialTheme.typography.labelSmall,
            color = if (permission.granted) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
          )
        }
      }
    }
    Spacer(Modifier.height(12.dp))
  }
}

@Composable
private fun DeviceStep(ble: BleState, wifi: DeviceWifiStatus?) {
  val summary = deviceSummary(ble, wifi)
  val linked = isLinked(ble)
  val working = ble is BleState.Scanning || ble is BleState.Connecting || ble is BleState.Reconnecting
  StepHeading(
    "Connect your PocketWiki",
    if (linked) {
      "Next, choose the network PocketWiki should join."
    } else {
      "Turn it on and keep it nearby."
    },
  )
  Spacer(Modifier.height(20.dp))
  QuietCard {
    Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
      Icon(
        when {
          ble is BleState.Error -> PocketIcons.BluetoothOff
          working -> PocketIcons.BluetoothSearching
          wifi?.connected == true -> PocketIcons.Wifi
          else -> PocketIcons.Bluetooth
        },
        null,
        Modifier.size(22.dp),
        tint = if (ble is BleState.Error) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary,
      )
      Spacer(Modifier.width(14.dp))
      Column(Modifier.weight(1f)) {
        Text(summary.title, style = MaterialTheme.typography.bodyLarge)
        Text(summary.detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
      }
      if (working) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.5.dp)
    }
  }
}

@Composable
private fun WifiStep(
  scanState: WifiScanState,
  phone: PhoneNetwork?,
  ssid: String,
  onSsidChange: (String) -> Unit,
  password: String,
  onPasswordChange: (String) -> Unit,
  ble: BleState,
  onScan: () -> Unit,
) {
  StepHeading(
    "Wi-Fi for PocketWiki",
    "Pick the 2.4 GHz network PocketWiki should join.",
  )
  Spacer(Modifier.height(16.dp))
  QuietCard { InfoRow(PocketIcons.Phone, "Your phone", phoneNetworkLabel(phone)) }
  Spacer(Modifier.height(12.dp))
  NetworkPicker(
    scanState = scanState,
    phoneSsid = phone?.ssid,
    ssid = ssid,
    onSsidChange = onSsidChange,
    password = password,
    onPasswordChange = onPasswordChange,
    enabled = ble !is BleState.Sending,
    errorMessage = (ble as? BleState.Error)?.message,
    savedMessage = if (ble is BleState.Sent) "Saved." else null,
    onScan = onScan,
  )
}

@Composable
private fun LibraryStep(
  ble: BleState,
  wifi: DeviceWifiStatus?,
  library: DeviceNetworkState,
  onOpenUrl: (String) -> Unit,
  onNotify: (String) -> Unit,
) {
  val context = LocalContext.current
  val summary = deviceSummary(ble, wifi)
  val settled = summary.tone == StatusTone.Good
  val packs = (library as? DeviceNetworkState.Ready)?.packs
  val address = dashboardUrl(wifi)
  StepHeading(
    "Your library is ready",
    "Read it from any device on the same network.",
  )
  Spacer(Modifier.height(20.dp))
  QuietCard {
    Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
      BrandMark(46.dp)
      Spacer(Modifier.width(14.dp))
      Column(Modifier.weight(1f)) {
        Text(
          if (settled) summary.title else "Not connected",
          style = MaterialTheme.typography.bodyLarge,
        )
        Text(
          if (settled) summary.detail else "Manage it any time from the Device tab.",
          style = MaterialTheme.typography.bodySmall,
          color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
      }
      Icon(
        if (settled) PocketIcons.CheckCircle else PocketIcons.Bluetooth,
        null,
        Modifier.size(22.dp),
        tint = if (settled) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
      )
    }
    CardDivider()
    Column(Modifier.fillMaxWidth().padding(16.dp)) {
      Text(
        "Open this address:",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
      Spacer(Modifier.height(4.dp))
      Row(verticalAlignment = Alignment.CenterVertically) {
        Text(address, Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
        IconButton(onClick = {
          context.getSystemService(ClipboardManager::class.java)
            ?.setPrimaryClip(ClipData.newPlainText("PocketWiki", address))
          onNotify("Address copied")
        }) {
          Icon(PocketIcons.Copy, "Copy the dashboard address", Modifier.size(20.dp))
        }
      }
      Spacer(Modifier.height(12.dp))
      OutlinedButton(
        onClick = { onOpenUrl(address) },
        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
        shape = MaterialTheme.shapes.medium,
      ) {
        Icon(PocketIcons.OpenInNew, null, Modifier.size(18.dp))
        Spacer(Modifier.width(8.dp))
        Text("Open dashboard")
      }
    }
  }
  if (packs != null) {
    Spacer(Modifier.height(12.dp))
    QuietCard {
      InfoRow(
        PocketIcons.Library,
        "Installed now",
        when (packs.size) {
          0 -> "No packs yet — add one from the Packs tab"
          1 -> "1 pack · ${displayPackName(packs.first().name, emptyList())}"
          else -> "${packs.size} packs, starting with ${displayPackName(packs.first().name, emptyList())}"
        },
      )
    }
  }
}

@Composable
private fun StepHeading(title: String, body: String) {
  Text(title, style = MaterialTheme.typography.headlineMedium)
  Spacer(Modifier.height(10.dp))
  Text(body, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
}
