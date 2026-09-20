package com.bilawalriaz.pocketwiki.device

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
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
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
    private const val REQUESTED_MTU = 247
    /** How long one automatic reconnect attempt may take before the next one. */
    private const val RECONNECT_WINDOW_MS = 16_000L
    /** How long a queued install waits for the link to come back. */
    private const val LINK_WAIT_MS = 10_000L
    /** How often the supervisor re-checks a link that is already up. A drop
     *  wakes it immediately; this only bounds how long a missed event lasts. */
    private const val LINK_POLL_MS = 2_000L
    /** Gaps between automatic reconnect attempts. The last one repeats, so a
     *  device that comes back late is still picked up without the user having
     *  to ask. */
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

  /* Packs install one at a time: the device accepts a single upload session,
   * and its one radio is shared with Wi-Fi. A multi-pack selection is queued
   * here and drained in order, so "install these three" costs one connection
   * and one tap instead of three of each. */
  private val installQueue = ArrayDeque<PendingInstall>()
  private val installQueueLock = Any()
  /** Wakes the drain loop. Conflated: a signal that is already pending is the
   *  same news as a second one. */
  private val installWake = Channel<Unit>(Channel.CONFLATED)
  /** The batch being fetched and installed, so "Cancel install" stops all of
   *  it rather than the pack that happens to be in flight. */
  @Volatile
  private var installBatch: Job? = null
  /** Free space the device last reported. A queued pack is checked against
   *  this without publishing a library read that would replace the install
   *  line the user is watching. */
  @Volatile
  private var installableBytes: Long? = null

  /** One pack waiting its turn: a URL to fetch (optionally pinned to the
   *  catalogue's size and digest) or a file already on this phone. */
  private class PendingInstall(
    val name: String,
    val url: String? = null,
    val file: File? = null,
    val bytes: Long? = null,
    val sha256: String? = null,
  ) {
    /** "Pack 2 of 3 · " when this pack is one of several; empty for a single
     *  install, so the common case keeps its plain line. */
    var label: String = ""
  }

  private var scanResult: ScanResult? = null
  @Volatile
  private var gatt: BluetoothGatt? = null
  private var wifiCommand: BluetoothGattCharacteristic? = null
  private var status: BluetoothGattCharacteristic? = null
  private var control: BluetoothGattCharacteristic? = null
  private var data: BluetoothGattCharacteristic? = null
  private var response: BluetoothGattCharacteristic? = null
  private var negotiatedMtu = 23
  private var writeContinuation: CancellableContinuation<Unit>? = null
  private var readContinuation: CancellableContinuation<ByteArray>? = null
  /** True from the first pack of an install to the last, including the held
   *  SoftAP hop. The captive-portal monitor suppresses its auto-open while
   *  this is true so it does not yank the browser open mid-install. */
  private val _transferActive = MutableStateFlow(false)
  val transferActive: StateFlow<Boolean> = _transferActive
  private var scanTimeoutRunnable: Runnable? = null
  /** Which scan a pending timeout belongs to, so a late timeout from an older
   *  scan cannot stop the one running now. */
  private var scanGeneration = 0
  /** True while a scan the user asked for is running. Only that kind reports
   *  "Searching…" and its own timeout: a background scan must not overwrite a
   *  state the user is looking at. */
  @Volatile
  private var scanAnnounced = false
  /** True once a link has been established. While it is true the app restores
   *  a dropped link on its own instead of waiting to be asked. */
  @Volatile
  private var linkWanted = false
  /** True once a link has ever been established, so a reconnect can say
   *  "Reconnecting" where a cold start says nothing. */
  @Volatile
  private var everLinked = false
  private var supervisor: Job? = null
  /** Cuts the supervisor's sleep short when the user asks for a link. */
  private val linkNudge = Channel<Unit>(Channel.CONFLATED)

  fun permissions(): Array<String> = if (Build.VERSION.SDK_INT >= 31) {
    arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
  } else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

  init {
    /* The link and the install queue both outlive any one screen: the app
     * reconnects and finishes an accepted batch while the user reads. */
    ensureSupervisor()
    scope.launch(Dispatchers.IO) { drainInstalls() }
  }

  fun hasPermissions(): Boolean = permissions().all {
    ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
  }

  /**
   * Ask for the link and say so: "Searching for PocketWiki…" until the device
   * appears. The supervisor does the scanning; this only states the intent and
   * the words.
   */
  fun scan() {
    linkWanted = true
    scanAnnounced = true
    _state.value = BleState.Scanning
    _libraryState.value = DeviceNetworkState.Requesting
    ensureSupervisor()
    linkNudge.trySend(Unit)
  }

  /**
   * The one loop that keeps the link in the state the user asked for.
   *
   * A background presence scan and a reconnect ladder used to scan against the
   * same [ScanCallback]; whichever finished first stopped the other's scan, so
   * a reconnect could spend its whole window waiting on a scan that had already
   * been cancelled, and a timeout that returned early left a scan running for
   * the next `startScan` to collide with. One loop cannot lose its own scan.
   *
   * It scans only while there is no link. A link that has existed and dropped
   * reads as "Reconnecting" for the first few tries; after that the state stays
   * on the connect failure — which is what the user can act on — while the scan
   * keeps running quietly, so the link still returns on its own.
   */
  private fun ensureSupervisor() {
    if (supervisor?.isActive == true) return
    supervisor = scope.launch(Dispatchers.IO) {
      var attempt = 0
      while (isActive) {
        if (gatt != null) {
          attempt = 0
        } else if (hasPermissions()) {
          if (linkWanted && everLinked && attempt < RECONNECT_DELAYS_MS.size) {
            _state.value = BleState.Reconnecting
            /* An install writes this line with its own progress; link
             * bookkeeping must not erase what the user is watching. */
            if (_libraryState.value !is DeviceNetworkState.Busy) {
              _libraryState.value = DeviceNetworkState.Idle
            }
          }
          handler.post { scanInternal() }
          if (awaitLink(RECONNECT_WINDOW_MS)) {
            attempt = 0
            continue
          }
          attempt++
        }
        /* Linked, or waiting to try again: idle, or back off along the
         * ladder. The last gap repeats, so the link still returns on its own
         * for a device that takes longer than the ladder to come back. */
        val pause = if (attempt == 0) {
          LINK_POLL_MS
        } else {
          RECONNECT_DELAYS_MS[minOf(attempt - 1, RECONNECT_DELAYS_MS.lastIndex)]
        }
        withTimeoutOrNull(pause) { linkNudge.receive() }
      }
    }
  }

  /** Wait for a GATT link to exist, or give up after [timeoutMs]. */
  private suspend fun awaitLink(timeoutMs: Long): Boolean =
    withTimeoutOrNull(timeoutMs) {
      while (gatt == null) delay(200)
      true
    } ?: false

  @SuppressLint("MissingPermission")
  private fun scanInternal() {
    if (!hasPermissions()) {
      if (scanAnnounced) {
        scanAnnounced = false
        _state.value = BleState.Error("Nearby-device permission is required")
      }
      return
    }
    val scanner = adapter?.bluetoothLeScanner
    if (scanner == null) {
      if (scanAnnounced) {
        scanAnnounced = false
        _state.value = BleState.Error("Bluetooth is unavailable or turned off")
      }
      return
    }
    scanResult = null
    /* A scan may still be running from the previous pass. Starting a second one
     * on the same callback fails with SCAN_FAILED_ALREADY_STARTED, so every
     * pass begins by ending the last one. */
    stopScan()
    val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
    scanner.startScan(
      listOf(filter),
      ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
      scanCallback,
    )
    val generation = ++scanGeneration
    val runnable = Runnable { scanTimedOut(generation) }
    scanTimeoutRunnable = runnable
    handler.postDelayed(runnable, SCAN_TIMEOUT_MS)
  }

  /** Bound every scan, announced or not: an unstopped scan outlives the state
   *  it belonged to and makes the next one fail to start. */
  private fun scanTimedOut(generation: Int) {
    if (generation != scanGeneration) return
    stopScan()
    if (!scanAnnounced) return
    scanAnnounced = false
    _state.value = BleState.Error("No PocketWiki found. Keep it powered on and nearby, then try again.")
    _libraryState.value = DeviceNetworkState.Error("No PocketWiki found over Bluetooth")
  }

  @SuppressLint("MissingPermission")
  fun connect() {
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
    when (_state.value) {
      is BleState.Found -> connect()
      BleState.Connecting, BleState.Connected, BleState.Sending, BleState.Sent -> { /* already progressing */ }
      else -> scan()
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

  /** [announce] is false while a batch of packs is installing: the read still
   *  refreshes the free-space figure the next pack is checked against, but it
   *  must not replace the install line the user is watching with a library
   *  panel the next pack is about to replace again. */
  private suspend fun refreshNow(announce: Boolean = true) {
    try {
      if (announce) _libraryState.value = DeviceNetworkState.Busy("Reading library over Bluetooth…")
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
      installableBytes = installable
      if (announce) _libraryState.value = DeviceNetworkState.Ready(packs, used, total, installable)
    } catch (error: Exception) {
      _libraryState.value = DeviceNetworkState.Error(error.message ?: "PocketWiki did not respond over Bluetooth")
    }
  }

  fun upload(file: File, name: String) {
    enqueue(listOf(PendingInstall(name = name, file = file)))
  }

  /** Install every pack in [packs], in order, over the one link. */
  fun install(packs: List<CatalogPack>) {
    enqueue(packs.map { PendingInstall(it.id, url = it.url, bytes = it.bytes, sha256 = it.sha256) })
  }

  /** Stop the batch and drop everything still waiting. */
  fun cancelInstalls() {
    synchronized(installQueueLock) { installQueue.clear() }
    installBatch?.cancel()
  }

  private fun enqueue(items: List<PendingInstall>) {
    if (items.isEmpty()) return
    synchronized(installQueueLock) {
      val first = installQueue.size + 1
      val total = installQueue.size + items.size
      items.forEachIndexed { index, item ->
        item.label = if (total > 1) "Pack ${first + index} of $total · " else ""
        installQueue.addLast(item)
      }
    }
    installWake.trySend(Unit)
  }

  /**
   * Fetch everything waiting, then install it — in that order, as one batch.
   *
   * The order is not cosmetic. Installing a pack moves the phone onto the
   * PocketWiki access point, which has no route to the internet, so a download
   * that started after the first install could never finish. Fetching the whole
   * selection first also means the access point is opened once for the batch,
   * and the system asks the user to approve it once.
   *
   * Each batch runs in its own job so "Cancel install" stops the whole of it,
   * including the packs it has not reached yet.
   */
  private suspend fun CoroutineScope.drainInstalls() {
    while (isActive) {
      val batch = takeQueued()
      if (batch == null) {
        installWake.receive()
        continue
      }
      /* The batch, not the pack, owns the phone's Wi-Fi: the captive-portal
       * monitor stays quiet from the first pack until the last one is in, even
       * between packs and even while the access point is held open. */
      _transferActive.value = true
      val job = launch { runBatch(batch) }
      installBatch = job
      job.join()
      installBatch = null
      /* The batch is over: let the access point go, and with it the silence
       * the monitor keeps while an install owns the phone's Wi-Fi. */
      wifiTransfer.releaseAp()
      _transferActive.value = false
      /* A failure keeps its error; anything else settles on a fresh read. */
      if (_libraryState.value !is DeviceNetworkState.Error) refreshNow()
    }
  }

  /** Every pack waiting its turn, taken as a unit so nothing is downloaded
   *  after the first install has moved the phone off the internet. */
  private fun takeQueued(): List<PendingInstall>? = synchronized(installQueueLock) {
    if (installQueue.isEmpty()) null else ArrayList(installQueue).also { installQueue.clear() }
  }

  /**
   * Fetch [batch], then install it. False when a pack could not be fetched, was
   * refused, or the transfer broke; cancellation is rethrown so the caller can
   * tell a stopped batch from a failed one.
   */
  private suspend fun runBatch(batch: List<PendingInstall>): Boolean {
    val fetched = ArrayList<Pair<PendingInstall, File>>(batch.size)
    try {
      for (item in batch) {
        fetched += item to (item.file ?: download(item))
      }
      if (gatt == null && !awaitLink(LINK_WAIT_MS)) {
        _libraryState.value = DeviceNetworkState.Error(
          "PocketWiki's Bluetooth link dropped; no packs were installed")
        return false
      }
      /* One connection for the lot: the packs go up back to back. */
      for ((item, file) in fetched) {
        if (!uploadNow(file, item.name, item.label)) return false
      }
      return true
    } catch (error: CancellationException) {
      throw error
    } catch (error: Exception) {
      _libraryState.value = DeviceNetworkState.Error(error.message ?: "Pack download failed")
      return false
    } finally {
      /* Packs fetched into the cache belong to this install alone. */
      fetched.forEach { (item, file) -> if (item.file == null) file.delete() }
    }
  }

  /** Fetch a catalogue pack into the cache, verifying it against the catalogue
   *  entry when one was given. */
  private suspend fun download(item: PendingInstall): File {
    val url = item.url ?: error("No pack address")
    item.bytes?.let { require(it <= MAX_PACK_BYTES) { "Pack is larger than 2 MB" } }
    val file = File(context.cacheDir, "download-${item.name.take(40)}.pwp")
    _libraryState.value =
      DeviceNetworkState.Busy("${item.label}Downloading ${item.name.replace('-', ' ')}…", 0f, cancellable = true)
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
        val length = item.bytes ?: connection.contentLengthLong.takeIf { it > 0 }
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
              "${item.label}Downloading ${item.name.replace('-', ' ')}…",
              (total.toFloat() / length).coerceIn(0f, 1f),
              cancellable = true,
            )
          }
        }
      }
    }
    item.bytes?.let { require(total == it) { "Pack size did not match the catalogue" } }
    item.sha256?.let {
      val actual = digest.digest().joinToString("") { byte -> "%02x".format(byte) }
      require(actual == it) { "Pack checksum did not match the catalogue" }
    }
    return file
  }

  private suspend fun uploadNow(file: File, requestedName: String, label: String = ""): Boolean {
    val safeName = safePackName(requestedName)
    try {
      require(file.length() in 1..MAX_PACK_BYTES) { "Pack must be 2 MB or smaller" }
      val available = installableBytes
      if (available != null) require(file.length() <= available) {
        "Not enough PocketWiki storage: ${formatBytes(file.length())} needed, ${formatBytes(available)} available"
      }
      /* Wi-Fi is the fast path: the device is already a station on the
       * phone's network, or the phone can hop onto the PocketWiki access
       * point for the transfer. BLE stays the fallback. */
      if (transferViaWifi(file, safeName, label)) return true
      _libraryState.value = DeviceNetworkState.Busy("${label}Preparing $safeName…", 0f, cancellable = true)
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
            "${label}Installing ${safeName.replace('-', ' ')} over Bluetooth…",
            (offset.toFloat() / file.length()).coerceIn(0f, 1f),
            cancellable = true,
          )
        }
      }
      val committed = command(byteArrayOf(OP_COMMIT))
      require(committed.size >= 6) { "PocketWiki could not confirm the installed pack" }
      return true
    } catch (error: CancellationException) {
      /* The BLE upload session (if any) must be aborted and the library
       * state restored before the cancelled job finishes. */
      withContext(NonCancellable) {
        runCatching { command(byteArrayOf(OP_ABORT)) }
        refreshNow(announce = false)
      }
      throw error
    } catch (error: Exception) {
      runCatching { command(byteArrayOf(OP_ABORT)) }
      _libraryState.value = DeviceNetworkState.Error(error.message ?: "Bluetooth pack install failed")
      return false
    }
  }

  /** Try the Wi-Fi transports for a ready local pack. Returns true when the
   *  upload succeeded. Falls through to BLE when the device is unreachable
   *  over Wi-Fi; a device rejection is surfaced. */
  private suspend fun transferViaWifi(file: File, safeName: String, label: String): Boolean {
    refreshWifiStatusNow()
    val status = _wifiStatus.value ?: return false
    val name = safeName.replace('-', ' ')
    val progress: (Float) -> Unit = { p ->
      _libraryState.value = DeviceNetworkState.Busy(
        "${label}Installing $name over Wi-Fi…", p, cancellable = true,
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
        "${label}Installing $name over Wi-Fi…", 0f, cancellable = true,
      )
      if (settle(wifiTransfer.uploadTo(status.ip, file, safeName, progress))) return true
    }

    /* SoftAP hop: reach the PocketWiki access point through Android's system
     * network request and open the HTTP connection explicitly on that
     * Network. This works even though the AP has no internet route. The
     * connection is held across the batch, so installing several packs asks
     * the user to approve the network once. */
    if (status.apSsid.isNotBlank() && status.apIp.isNotBlank()) {
      _libraryState.value = DeviceNetworkState.Busy(
        "${label}Connecting to PocketWiki Wi-Fi…", 0f, cancellable = true,
      )
      val onAp = phone.ssid?.equals(status.apSsid, ignoreCase = true) == true
      when (val route = wifiTransfer.route(status.apSsid, onAp)) {
        is WifiPackTransfer.ApRoute.Reachable -> {
          val settled = settle(
            wifiTransfer.uploadTo(status.apIp, file, safeName, progress, route.network),
          )
          if (settled) return true
        }
        WifiPackTransfer.ApRoute.Unreachable -> Unit
      }
    }
    return false
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

  /** Install a pack the user pasted a link for. */
  fun downloadThenUpload(url: String) {
    val name = url.substringAfterLast('/').substringBeforeLast('.').ifBlank { "library" }
    enqueue(listOf(PendingInstall(name = name, url = url)))
  }

  @SuppressLint("MissingPermission")
  fun close() {
    linkWanted = false
    _transferActive.value = false
    supervisor?.cancel()
    supervisor = null
    stopScan()
    installBatch?.cancel()
    synchronized(installQueueLock) { installQueue.clear() }
    wifiTransfer.releaseAp()
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
      scanAnnounced = false
      _state.value = BleState.Found(result.device.name ?: "PocketWiki Setup")
      if (_libraryState.value !is DeviceNetworkState.Busy) {
        _libraryState.value = DeviceNetworkState.Idle
      }
      // Connect on sight whenever the user already had this device linked, so a
      // link lost to Wi-Fi/Bluetooth radio sharing comes back by itself the
      // moment the device advertises again — including after the user has been
      // away and returned with Bluetooth or the device toggled off.
      if (linkWanted && gatt == null) connect()
    }

    override fun onScanFailed(errorCode: Int) {
      stopScan()
      /* Only a scan the user asked for reports a failure. A background pass
       * that could not start — most often SCAN_FAILED_ALREADY_STARTED from the
       * scan the supervisor is about to end — must not replace the state the
       * user is reading with an error of its own. */
      if (scanAnnounced) {
        scanAnnounced = false
        _state.value = BleState.Error("Bluetooth scanning failed. Turn Bluetooth off and on, then try again.")
        _libraryState.value = DeviceNetworkState.Error("PocketWiki could not be found over Bluetooth")
      }
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
        /* Release the client. A BluetoothGatt that is only dropped keeps its
         * client interface registered, and Android stops granting connections
         * to an app that has leaked enough of them — which is why repeated
         * automatic reconnects stop working when the first few fail. */
        this@BleProvisioner.gatt?.close()
        this@BleProvisioner.gatt = null
        _wifiStatus.value = null
        _state.value = BleState.Error("PocketWiki disconnected")
        _libraryState.value = DeviceNetworkState.Error("PocketWiki Bluetooth connection was lost")
        /* PocketWiki shares one radio between Wi-Fi and Bluetooth, so a pack
         * install or removal can cost the link even though nothing is wrong.
         * The supervisor puts it back without making the user notice. */
        linkNudge.trySend(Unit)
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
    everLinked = true
    _state.value = BleState.Connected
    if (control == null || data == null || response == null) {
      _libraryState.value = DeviceNetworkState.Error("Update PocketWiki firmware to enable BLE library management")
    } else refresh()
    refreshWifiStatus()
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
