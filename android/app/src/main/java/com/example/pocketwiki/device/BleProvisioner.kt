package com.example.pocketwiki.device

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CancellableContinuation
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.withContext
import org.json.JSONObject
import kotlin.coroutines.cancellation.CancellationException
import kotlin.coroutines.coroutineContext
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID
import java.util.zip.CRC32
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

sealed interface BleState {
  data object Idle : BleState
  data object Scanning : BleState
  data class Found(val name: String) : BleState
  data object Connecting : BleState
  data object Connected : BleState
  data object Sending : BleState
  data object Sent : BleState
  /** The link dropped on its own and the app is putting it back without being
   *  asked. Distinct from [Connecting] because the user did nothing. */
  data object Reconnecting : BleState
  data class Error(val message: String) : BleState
}

data class DeviceWifiStatus(
  val connected: Boolean,
  val ssid: String,
  val ip: String,
  val apSsid: String = "",
  val apIp: String = "",
)

class BleProvisioner(
  private val context: Context,
  private val scope: CoroutineScope,
  private val phoneWifi: PhoneWifi,
) {
  companion object {
    private const val SCAN_TIMEOUT_MS = 10_000L
    private const val PRESENCE_INTERVAL_MS = 7_000L
    private const val REQUESTED_MTU = 247
    /** How long one automatic reconnect attempt may take before the next one. */
    private const val RECONNECT_WINDOW_MS = 16_000L
    /** Gaps between automatic reconnect attempts. Five tries, then stop and
     *  hand the decision back to the user rather than scanning forever. */
    private val RECONNECT_DELAYS_MS = longArrayOf(400L, 1_200L, 2_500L, 5_000L, 8_000L)
    private const val OP_INFO: Byte = 0x01
    private const val OP_PACK_AT: Byte = 0x02
    private const val OP_BEGIN: Byte = 0x10
    private const val OP_COMMIT: Byte = 0x11
    private const val OP_ABORT: Byte = 0x12
    private const val OP_DELETE: Byte = 0x20

    val SERVICE_UUID: UUID = UUID.fromString("7a1e0001-7b45-4c8f-9d10-8a5ef1a0c301")
    val WIFI_COMMAND_UUID: UUID = UUID.fromString("7a1e0002-7b45-4c8f-9d10-8a5ef1a0c301")
    val STATUS_UUID: UUID = UUID.fromString("7a1e0003-7b45-4c8f-9d10-8a5ef1a0c301")
    val CONTROL_UUID: UUID = UUID.fromString("7a1e0004-7b45-4c8f-9d10-8a5ef1a0c301")
    val DATA_UUID: UUID = UUID.fromString("7a1e0005-7b45-4c8f-9d10-8a5ef1a0c301")
    val RESPONSE_UUID: UUID = UUID.fromString("7a1e0006-7b45-4c8f-9d10-8a5ef1a0c301")
  }

  private val bluetoothManager = context.getSystemService(BluetoothManager::class.java)
  private val wifiTransfer = WifiPackTransfer(context)
  private val handler = Handler(Looper.getMainLooper())
  private val adapter get() = bluetoothManager?.adapter
  private val _state = MutableStateFlow<BleState>(BleState.Idle)
  val state: StateFlow<BleState> = _state
  private val _libraryState = MutableStateFlow<DeviceNetworkState>(DeviceNetworkState.Idle)
  val libraryState: StateFlow<DeviceNetworkState> = _libraryState
  private val _wifiStatus = MutableStateFlow<DeviceWifiStatus?>(null)
  val wifiStatus: StateFlow<DeviceWifiStatus?> = _wifiStatus
  /** The PocketWiki access-point name the phone should join to reach the
   *  dashboard directly. Defaults to the firmware default and is refined from
   *  the device status once available (so the captive-portal monitor matches
   *  a custom SSID too). */
  private val _knownApSsid = MutableStateFlow<String?>("PocketWiki")
  val knownApSsid: StateFlow<String?> = _knownApSsid
  private val operationMutex = Mutex()

  /* At most one pack install runs at a time: starting a second install while
   * one is downloading or transferring is ignored until the first completes
   * (or is cancelled). The slot is cleared only after the job has fully
   * unwound, so a cancelled install still releases the BLE upload session
   * before another one may begin. */
  @Volatile
  private var installJob: Job? = null

  private var scanResult: ScanResult? = null
  private var gatt: BluetoothGatt? = null
  private var wifiCommand: BluetoothGattCharacteristic? = null
  private var status: BluetoothGattCharacteristic? = null
  private var control: BluetoothGattCharacteristic? = null
  private var data: BluetoothGattCharacteristic? = null
  private var response: BluetoothGattCharacteristic? = null
  private var negotiatedMtu = 23
  private var writeContinuation: CancellableContinuation<Unit>? = null
  private var readContinuation: CancellableContinuation<ByteArray>? = null
  /** Set when a connect was requested before the device was found, so the
  *  first scan result connects immediately (single-press setup). */
  @Volatile
  private var pendingAutoConnect = false
  /** True while a Wi‑Fi pack transfer (including the programmatic SoftAP hop)
   *  is in flight. The captive‑portal monitor suppresses its auto‑open while
   *  this is true so it does not yank the browser open mid‑upload. */
  private val _transferActive = MutableStateFlow(false)
  val transferActive: StateFlow<Boolean> = _transferActive
  private var scanTimeoutRunnable: Runnable? = null
  @Volatile
  private var autoScan = false
  private var presenceJob: Job? = null
  /** True once a link has been established by the user. While it is true the
   *  app restores a dropped link on its own instead of waiting to be asked. */
  @Volatile
  private var linkWanted = false
  private var reconnectJob: Job? = null

  fun permissions(): Array<String> = if (Build.VERSION.SDK_INT >= 31) {
    arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
  } else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

  fun hasPermissions(): Boolean = permissions().all {
    ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
  }

  /** Scan for the device. When `autoConnect` is set, the first device found is
   *  connected to immediately, collapsing the old find-then-connect two-step
   *  into one press. */
  @SuppressLint("MissingPermission")
  fun scan(autoConnect: Boolean = false) {
    linkWanted = true
    stopReconnect()
    pendingAutoConnect = autoConnect
    scanInternal(announce = true)
  }

  /** Background presence scan used by [startAutoScan]. It never touches the
   *  visible state (so the UI does not flicker between "scanning" and "find"),
   *  but promotes Idle/Error to Found as soon as the device appears. */
  @SuppressLint("MissingPermission")
  fun scanPresence() {
    if (_state.value !is BleState.Idle && _state.value !is BleState.Error) return
    scanInternal(announce = false)
  }

  @SuppressLint("MissingPermission")
  private fun scanInternal(announce: Boolean) {
    if (!hasPermissions()) {
      if (announce) _state.value = BleState.Error("Nearby-device permission is required")
      return
    }
    val scanner = adapter?.bluetoothLeScanner
    if (scanner == null) {
      if (announce) _state.value = BleState.Error("Bluetooth is unavailable or turned off")
      return
    }
    scanResult = null
    if (announce) {
      _state.value = BleState.Scanning
      _libraryState.value = DeviceNetworkState.Requesting
    }
    // The background presence scan may still be running; starting a second scan
    // on the same callback fails with SCAN_FAILED_ALREADY_STARTED.
    stopScan()
    val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
    scanner.startScan(
      listOf(filter),
      ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
      scanCallback,
    )
    val runnable = Runnable { scanTimedOut(announce) }
    scanTimeoutRunnable = runnable
    handler.removeCallbacks(runnable)
    handler.postDelayed(runnable, SCAN_TIMEOUT_MS)
  }

  private fun scanTimedOut(announce: Boolean) {
    if (_state.value != BleState.Scanning) return
    stopScan()
    pendingAutoConnect = false
    if (announce) {
      _state.value = BleState.Error("No PocketWiki found. Keep it powered on and nearby, then try again.")
      _libraryState.value = DeviceNetworkState.Error("No PocketWiki found over Bluetooth")
    }
  }

  /** Periodically hunt for the PocketWiki in the background so the user rarely
   *  has to press anything: once found, the connect button needs a single tap.
   *  The loop idles whenever a GATT connection exists. */
  fun startAutoScan() {
    autoScan = true
    if (presenceJob != null) return
    presenceJob = scope.launch(Dispatchers.IO) {
      while (autoScan && isActive) {
        if (hasPermissions() && gatt == null) {
          handler.post { scanPresence() }
        }
        delay(PRESENCE_INTERVAL_MS)
      }
    }
  }

  fun stopAutoScan() {
    autoScan = false
    presenceJob?.cancel()
    presenceJob = null
  }

  @SuppressLint("MissingPermission")
  fun connect() {
    pendingAutoConnect = false
    val result = scanResult ?: run { scan(); return }
    stopScan()
    _state.value = BleState.Connecting
    _libraryState.value = DeviceNetworkState.Requesting
    gatt?.close()
    gatt = result.device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
  }

  /** One-press connect. Connects now if the device is already found; otherwise
   *  scans and connects as soon as it appears. Fixes the old behaviour where a
   *  first tap only discovered the device and a second tap was needed. */
  fun ensureConnect() {
    linkWanted = true
    stopReconnect()
    when (_state.value) {
      is BleState.Found -> connect()
      BleState.Connecting, BleState.Connected, BleState.Sending, BleState.Sent -> { /* already progressing */ }
      else -> scan(autoConnect = true)
    }
  }

  fun sendCredentials(ssid: String, password: String) {
    if (ssid.isBlank() || ssid.length > 32 || password.length > 64) {
      _state.value = BleState.Error("Enter a valid 2.4 GHz network name and password")
      return
    }
    scope.launch(Dispatchers.IO) {
      try {
        _state.value = BleState.Sending
        write(wifiCommand ?: error("Provisioning service was not found"), "$ssid\n$password".toByteArray())
        _state.value = BleState.Sent
        refreshWifiStatus()
      } catch (error: Exception) {
        _state.value = BleState.Error(error.message ?: "PocketWiki did not accept the Wi-Fi details")
      }
    }
  }

  fun refresh() {
    scope.launch(Dispatchers.IO) { refreshNow() }
  }

  private suspend fun refreshNow() {
    try {
      _libraryState.value = DeviceNetworkState.Busy("Reading library over Bluetooth…")
      val info = command(byteArrayOf(OP_INFO))
      require(info.size >= 16) { "PocketWiki returned incomplete storage information" }
      val buffer = info.leBuffer()
      buffer.position(2)
      val total = buffer.unsignedInt()
      val used = buffer.unsignedInt()
      val installable = buffer.unsignedInt()
      val count = buffer.short.toInt() and 0xffff
      val packs = buildList {
        for (index in 0 until count) {
          val reply = command(ByteBuffer.allocate(3).order(ByteOrder.LITTLE_ENDIAN)
            .put(OP_PACK_AT).putShort(index.toShort()).array())
          require(reply.size >= 13) { "PocketWiki returned an incomplete pack entry" }
          val item = reply.leBuffer()
          item.position(4)
          val articles = item.int
          val bytes = item.unsignedInt()
          val nameLength = reply[12].toInt() and 0xff
          require(13 + nameLength <= reply.size) { "PocketWiki returned a bad pack name" }
          val name = reply.copyOfRange(13, 13 + nameLength).toString(Charsets.UTF_8)
          add(PackInfo(name, articles, bytes, true))
        }
      }
      _libraryState.value = DeviceNetworkState.Ready(packs, used, total, installable)
    } catch (error: Exception) {
      _libraryState.value = DeviceNetworkState.Error(error.message ?: "PocketWiki did not respond over Bluetooth")
    }
  }

  fun upload(file: File, name: String) {
    startInstall { uploadNow(file, name) }
  }

  fun cancelInstall() {
    installJob?.cancel()
  }

  private fun startInstall(block: suspend () -> Unit) {
    if (installJob != null) return
    installJob = scope.launch(Dispatchers.IO) {
      try {
        block()
      } finally {
        installJob = null
      }
    }
  }

  private suspend fun uploadNow(file: File, requestedName: String) {
    val safeName = safePackName(requestedName)
    try {
      require(file.length() in 1..MAX_PACK_BYTES) { "Pack must be 2 MB or smaller" }
      val available = (_libraryState.value as? DeviceNetworkState.Ready)?.installable
      if (available != null) require(file.length() <= available) {
        "Not enough PocketWiki storage: ${formatBytes(file.length())} needed, ${formatBytes(available)} available"
      }
      /* Wi-Fi is the fast path: the device is already a station on the
       * phone's network, or the phone can hop onto the PocketWiki access
       * point for the transfer. BLE stays the fallback. */
      if (transferViaWifi(file, safeName)) return
      _libraryState.value = DeviceNetworkState.Busy("Preparing $safeName…", 0f, cancellable = true)
      val crc = CRC32()
      file.inputStream().use { input ->
        val chunk = ByteArray(8192)
        while (true) {
          val count = input.read(chunk)
          if (count < 0) break
          crc.update(chunk, 0, count)
        }
      }
      val nameBytes = safeName.toByteArray(Charsets.UTF_8)
      val begin = ByteBuffer.allocate(10 + nameBytes.size).order(ByteOrder.LITTLE_ENDIAN)
        .put(OP_BEGIN).putInt(file.length().toInt()).putInt(crc.value.toInt())
        .put(nameBytes.size.toByte()).put(nameBytes).array()
      command(begin)

      val chunkSize = (negotiatedMtu - 7).coerceIn(16, 240)
      var offset = 0L
      file.inputStream().use { input ->
        val chunk = ByteArray(chunkSize)
        while (true) {
          val count = input.read(chunk)
          if (count < 0) break
          val packet = ByteBuffer.allocate(4 + count).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(offset.toInt()).put(chunk, 0, count).array()
          write(data ?: error("BLE pack transfer is unavailable; update PocketWiki firmware"), packet)
          offset += count
          coroutineContext.ensureActive()
          _libraryState.value = DeviceNetworkState.Busy(
            "Installing ${safeName.replace('-', ' ')} over Bluetooth…",
            (offset.toFloat() / file.length()).coerceIn(0f, 1f),
            cancellable = true,
          )
        }
      }
      val committed = command(byteArrayOf(OP_COMMIT))
      require(committed.size >= 6) { "PocketWiki could not confirm the installed pack" }
      refreshNow()
    } catch (error: CancellationException) {
      /* The BLE upload session (if any) must be aborted and the library
       * state restored before the cancelled job finishes. */
      withContext(NonCancellable) {
        runCatching { command(byteArrayOf(OP_ABORT)) }
        refreshNow()
      }
      throw error
    } catch (error: Exception) {
      runCatching { command(byteArrayOf(OP_ABORT)) }
      _libraryState.value = DeviceNetworkState.Error(error.message ?: "Bluetooth pack install failed")
    }
  }

  /** Try the Wi-Fi transports for a ready local pack. Returns true when the
   *  upload succeeded (library already refreshed). Falls through to BLE when
   *  the device is unreachable over Wi-Fi; a device rejection is surfaced. */
  private suspend fun transferViaWifi(file: File, safeName: String): Boolean {
    _transferActive.value = true
    try {
      refreshWifiStatusNow()
      val status = _wifiStatus.value ?: return false
      val label = safeName.replace('-', ' ')
      val progress: (Float) -> Unit = { p ->
        _libraryState.value = DeviceNetworkState.Busy(
          "Installing $label over Wi-Fi…", p, cancellable = true,
        )
      }
      fun settle(result: WifiUploadResult): Boolean = when (result) {
        is WifiUploadResult.Success -> true
        is WifiUploadResult.Rejected -> throw IllegalStateException(result.message)
        WifiUploadResult.Unreachable -> false
      }

      /* Refresh before choosing a route. Android may report a connected Wi-Fi
       * transport while withholding its SSID unless the relevant permission or
       * location setting is enabled. Unknown is not proof of same-network:
       * attempting the station IP in that case leaves the install apparently
       * stuck and prevents the AP fallback from being tried promptly. */
      phoneWifi.refresh()
      val phone = phoneWifi.state.value
      val sameNetwork = status.connected && status.ip.isNotBlank() &&
        !status.ssid.isNullOrBlank() &&
        phone.ssid?.equals(status.ssid, ignoreCase = true) == true
      if (sameNetwork) {
        _libraryState.value = DeviceNetworkState.Busy(
          "Installing $label over Wi-Fi…", 0f, cancellable = true,
        )
        if (settle(wifiTransfer.uploadTo(status.ip, file, safeName, progress))) {
          refreshNow()
          return true
        }
      }

      /* SoftAP hop: join the PocketWiki access point through Android's
       * system network request, then open the HTTP connection explicitly on
       * that Network. This works even though the AP has no internet route. */
      if (status.apSsid.isNotBlank() && status.apIp.isNotBlank()) {
        _libraryState.value = DeviceNetworkState.Busy(
          "Connecting to PocketWiki Wi-Fi…", 0f, cancellable = true,
        )
        val ok = if (phone.ssid?.equals(status.apSsid, ignoreCase = true) == true) {
          settle(wifiTransfer.uploadTo(status.apIp, file, safeName, progress))
        } else {
          settle(wifiTransfer.joinApAndUpload(status.apSsid, status.apIp, file, safeName, progress))
        }
        if (ok) {
          refreshNow()
          return true
        }
      }
      return false
    } finally {
      _transferActive.value = false
    }
  }

  /** Fresh device network status for the transport decision; the fire-and-
   *  forget variant below keeps the card updated on connect. */
  private suspend fun refreshWifiStatusNow() {
    runCatching {
      val characteristic = status ?: return@runCatching
      val reply = read(characteristic)
      val json = JSONObject(reply.toString(Charsets.UTF_8))
      val apSsid = json.optString("ap_ssid")
      if (apSsid.isNotBlank()) _knownApSsid.value = apSsid
      _wifiStatus.value = DeviceWifiStatus(
        connected = json.optBoolean("connected"),
        ssid = json.optString("ssid"),
        ip = json.optString("ip"),
        apSsid = apSsid,
        apIp = json.optString("ap_ip"),
      )
    }
  }

  fun action(action: String, name: String) {
    if (action != "delete") return
    scope.launch(Dispatchers.IO) {
      try {
        _libraryState.value = DeviceNetworkState.Busy("Removing pack over Bluetooth…")
        val bytes = name.toByteArray(Charsets.UTF_8)
        require(bytes.isNotEmpty() && bytes.size <= 40) { "Invalid pack name" }
        command(byteArrayOf(OP_DELETE, bytes.size.toByte()) + bytes)
        refreshNow()
      } catch (error: Exception) {
        _libraryState.value = DeviceNetworkState.Error(error.message ?: "Could not remove pack")
      }
    }
  }

  fun downloadThenUpload(url: String) {
    val name = url.substringAfterLast('/').substringBeforeLast('.').ifBlank { "library" }
    startInstall { downloadThenUploadNow(url, name, null, null) }
  }

  fun install(pack: CatalogPack) {
    startInstall { downloadThenUploadNow(pack.url, pack.id, pack.bytes, pack.sha256) }
  }

  private suspend fun downloadThenUploadNow(url: String, name: String, expectedBytes: Long?, expectedSha256: String?) {
    var target: File? = null
    var uploadStarted = false
    try {
      if (expectedBytes != null) require(expectedBytes <= MAX_PACK_BYTES) { "Pack is larger than 2 MB" }
      _libraryState.value = DeviceNetworkState.Busy("Downloading ${name.replace('-', ' ')}…", 0f, cancellable = true)
      val file = File(context.cacheDir, "download-${name.take(40)}.pwp")
      target = file
      val connection = URL(url).openConnection() as HttpURLConnection
      connection.connectTimeout = 15_000
      connection.readTimeout = 30_000
      connection.setRequestProperty("User-Agent", "PocketWiki-Android/1")
      require(connection.responseCode in 200..299) { "Pack download returned ${connection.responseCode}" }
      require(connection.contentLengthLong <= MAX_PACK_BYTES) { "Pack is larger than 2 MB" }
      val digest = MessageDigest.getInstance("SHA-256")
      var total = 0L
      connection.inputStream.use { input ->
        file.outputStream().use { output ->
          val chunk = ByteArray(8192)
          val length = expectedBytes ?: connection.contentLengthLong.takeIf { it > 0 }
          while (true) {
            val count = input.read(chunk)
            if (count < 0) break
            total += count
            require(total <= MAX_PACK_BYTES) { "Pack is larger than 2 MB" }
            digest.update(chunk, 0, count)
            output.write(chunk, 0, count)
            coroutineContext.ensureActive()
            if (length != null) {
              _libraryState.value = DeviceNetworkState.Busy(
                "Downloading ${name.replace('-', ' ')}…",
                (total.toFloat() / length).coerceIn(0f, 1f),
                cancellable = true,
              )
            }
          }
        }
      }
      if (expectedBytes != null) require(total == expectedBytes) { "Pack size did not match the catalogue" }
      if (expectedSha256 != null) {
        val actual = digest.digest().joinToString("") { "%02x".format(it) }
        require(actual == expectedSha256) { "Pack checksum did not match the catalogue" }
      }
      uploadStarted = true
      uploadNow(file, name)
    } catch (error: CancellationException) {
      if (!uploadStarted) withContext(NonCancellable) { refreshNow() }
      throw error
    } catch (error: Exception) {
      _libraryState.value = DeviceNetworkState.Error(error.message ?: "Pack download failed")
    } finally {
      target?.delete()
    }
  }

  @SuppressLint("MissingPermission")
  fun close() {
    linkWanted = false
    stopReconnect()
    stopAutoScan()
    stopScan()
    failPending("PocketWiki connection closed")
    _wifiStatus.value = null
    gatt?.close()
    gatt = null
  }

  private suspend fun command(value: ByteArray): ByteArray {
    val expectedOpcode = value.first()
    write(control ?: error("BLE management is unavailable; update PocketWiki firmware"), value)
    val reply = read(response ?: error("BLE management is unavailable; update PocketWiki firmware"))
    require(reply.size >= 2 && reply[0] == expectedOpcode) { "PocketWiki returned an unexpected BLE response" }
    val status = reply[1].toInt() and 0xff
    require(status == 0) {
      when (status) {
        2 -> "PocketWiki rejected invalid pack details"
        3 -> "Not enough PocketWiki storage"
        4 -> "PocketWiki storage write failed"
        5 -> "The transferred pack is invalid or incomplete"
        6 -> "The pack is no longer installed"
        7 -> "Bluetooth transfer offset did not match"
        else -> "PocketWiki rejected the BLE command ($status)"
      }
    }
    return reply
  }

  @SuppressLint("MissingPermission")
  private suspend fun write(characteristic: BluetoothGattCharacteristic, value: ByteArray) {
    operationMutex.withLock {
      suspendCancellableCoroutine { continuation ->
        writeContinuation = continuation
        val active = gatt
        val started = if (active == null) false else if (Build.VERSION.SDK_INT >= 33) {
          active.writeCharacteristic(characteristic, value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothGatt.GATT_SUCCESS
        } else {
          @Suppress("DEPRECATION")
          characteristic.value = value
          @Suppress("DEPRECATION")
          active.writeCharacteristic(characteristic)
        }
        if (!started) {
          writeContinuation = null
          continuation.resumeWithException(IllegalStateException("Could not start Bluetooth write"))
        }
      }
    }
  }

  @SuppressLint("MissingPermission")
  private suspend fun read(characteristic: BluetoothGattCharacteristic): ByteArray {
    return operationMutex.withLock {
      suspendCancellableCoroutine { continuation ->
        readContinuation = continuation
        val started = gatt?.readCharacteristic(characteristic) == true
        if (!started) {
          readContinuation = null
          continuation.resumeWithException(IllegalStateException("Could not start Bluetooth read"))
        }
      }
    }
  }

  private val scanCallback = object : ScanCallback() {
    @SuppressLint("MissingPermission")
    override fun onScanResult(callbackType: Int, result: ScanResult) {
      scanResult = result
      stopScan()
      _state.value = BleState.Found(result.device.name ?: "PocketWiki Setup")
      _libraryState.value = DeviceNetworkState.Idle
      // Connect on sight whenever the user already had this device linked, so a
      // link lost to Wi-Fi/Bluetooth radio sharing comes back by itself the
      // moment the device advertises again — including after the user has been
      // away and returned with Bluetooth or the device toggled off.
      if (pendingAutoConnect || (linkWanted && gatt == null)) {
        pendingAutoConnect = false
        connect()
      }
    }

    override fun onScanFailed(errorCode: Int) {
      scanTimeoutRunnable?.let { handler.removeCallbacks(it) }
      if (_state.value == BleState.Scanning) {
        _state.value = BleState.Error("Bluetooth scanning failed. Turn Bluetooth off and on, then try again.")
        _libraryState.value = DeviceNetworkState.Error("PocketWiki could not be found over Bluetooth")
      }
      pendingAutoConnect = false
    }
  }

  @SuppressLint("MissingPermission")
  private fun stopScan() {
    scanTimeoutRunnable?.let { handler.removeCallbacks(it) }
    scanTimeoutRunnable = null
    adapter?.bluetoothLeScanner?.stopScan(scanCallback)
  }

  private val gattCallback = object : BluetoothGattCallback() {
    @SuppressLint("MissingPermission")
    override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
      if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) {
        _state.value = BleState.Connecting
        gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
        gatt.discoverServices()
      } else if (newState == BluetoothProfile.STATE_DISCONNECTED || status != BluetoothGatt.GATT_SUCCESS) {
        failPending("PocketWiki disconnected")
        this@BleProvisioner.gatt = null
        _wifiStatus.value = null
        _state.value = BleState.Error("PocketWiki disconnected")
        _libraryState.value = DeviceNetworkState.Error("PocketWiki Bluetooth connection was lost")
        // PocketWiki shares one radio between Wi-Fi and Bluetooth, so a pack
        // install or removal can cost the link even though nothing is wrong.
        // Put it back without making the user notice.
        restoreLink()
      }
    }

    @SuppressLint("MissingPermission")
    override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
      val service: BluetoothGattService? = gatt.getService(SERVICE_UUID)
      wifiCommand = service?.getCharacteristic(WIFI_COMMAND_UUID)
      this@BleProvisioner.status = service?.getCharacteristic(STATUS_UUID)
      control = service?.getCharacteristic(CONTROL_UUID)
      data = service?.getCharacteristic(DATA_UUID)
      response = service?.getCharacteristic(RESPONSE_UUID)
      if (status != BluetoothGatt.GATT_SUCCESS || wifiCommand == null) {
        _state.value = BleState.Error("PocketWiki provisioning service was not found")
        return
      }
      if (!gatt.requestMtu(REQUESTED_MTU)) connectionReady()
    }

    override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
      if (status == BluetoothGatt.GATT_SUCCESS) negotiatedMtu = mtu
      connectionReady()
    }

    override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
      val continuation = writeContinuation ?: return
      writeContinuation = null
      if (status == BluetoothGatt.GATT_SUCCESS) continuation.resume(Unit)
      else continuation.resumeWithException(IllegalStateException("Bluetooth write failed ($status)"))
    }

    @Deprecated("Used through Android 12")
    override fun onCharacteristicRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
      @Suppress("DEPRECATION")
      finishRead(characteristic.value ?: byteArrayOf(), status)
    }

    override fun onCharacteristicRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
      finishRead(value, status)
    }
  }

  private fun refreshWifiStatus() {
    scope.launch(Dispatchers.IO) {
      refreshWifiStatusNow()
    }
  }

  fun connectionReady() {
    linkWanted = true
    stopReconnect()
    _state.value = BleState.Connected
    if (control == null || data == null || response == null) {
      _libraryState.value = DeviceNetworkState.Error("Update PocketWiki firmware to enable BLE library management")
    } else refresh()
    refreshWifiStatus()
  }

  /**
   * Put a dropped link back without the user asking. PocketWiki runs Wi-Fi and
   * Bluetooth on one radio, so committing or removing a pack can cost the link
   * even though both sides are healthy; before this, the app sat in an error
   * state until the user pressed Connect again.
   *
   * Attempts are spaced so a device that is mid-restart is not hammered, and
   * the loop stops after a handful so a device that is genuinely gone ends in a
   * plain error the user can act on instead of an endless scan.
   */
  private fun restoreLink() {
    if (!linkWanted || !autoScan) return
    if (reconnectJob?.isActive == true) return
    reconnectJob = scope.launch(Dispatchers.IO) {
      var attempt = 0
      while (isActive && linkWanted && gatt == null && attempt < RECONNECT_DELAYS_MS.size) {
        _state.value = BleState.Reconnecting
        _libraryState.value = DeviceNetworkState.Idle
        delay(RECONNECT_DELAYS_MS[attempt])
        attempt++
        if (!isActive || !linkWanted || gatt != null) break
        _state.value = BleState.Reconnecting
        handler.post { if (linkWanted && gatt == null) reconnectAttempt() }
        val settled = withTimeoutOrNull(RECONNECT_WINDOW_MS) {
          _state.first { it is BleState.Connected || it is BleState.Error }
        }
        if (settled is BleState.Connected || _state.value is BleState.Connected) return@launch
      }
      if (gatt == null && _state.value !is BleState.Connected) {
        _state.value = BleState.Error("PocketWiki isn't answering over Bluetooth")
        _libraryState.value = DeviceNetworkState.Error("PocketWiki's Bluetooth link dropped")
      }
    }
  }

  /** One silent reconnect attempt: scan without announcing, connect on sight. */
  @SuppressLint("MissingPermission")
  private fun reconnectAttempt() {
    pendingAutoConnect = true
    scanInternal(announce = false)
  }

  private fun stopReconnect() {
    reconnectJob?.cancel()
    reconnectJob = null
  }

  private fun finishRead(value: ByteArray, status: Int) {
    val continuation = readContinuation ?: return
    readContinuation = null
    if (status == BluetoothGatt.GATT_SUCCESS) continuation.resume(value)
    else continuation.resumeWithException(IllegalStateException("Bluetooth read failed ($status)"))
  }

  private fun failPending(message: String) {
    writeContinuation?.resumeWithException(IllegalStateException(message))
    readContinuation?.resumeWithException(IllegalStateException(message))
    writeContinuation = null
    readContinuation = null
  }

  private fun ByteArray.leBuffer(): ByteBuffer = ByteBuffer.wrap(this).order(ByteOrder.LITTLE_ENDIAN)
  private fun ByteBuffer.unsignedInt(): Long = int.toLong() and 0xffffffffL

  private fun formatBytes(bytes: Long): String = when {
    bytes >= 1_048_576 -> "%.1f MB".format(bytes / 1_048_576.0)
    bytes >= 1024 -> "%.0f KB".format(bytes / 1024.0)
    else -> "$bytes B"
  }
}
