package com.example.pocketwiki.device

import android.annotation.SuppressLint
import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.suspendCancellableCoroutine
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import kotlin.coroutines.resume

/** Same safe-name rules as the BLE path; shared so every transport addresses
 *  the device's pack store identically. */
fun safePackName(requestedName: String): String =
  requestedName.lowercase().replace(Regex("[^a-z0-9_-]+"), "-")
    .trim('-').take(40).ifBlank { "library" }

/** Outcome of one HTTP upload attempt. `Unreachable` means the phone could
 *  not talk to the device over Wi-Fi at all — the caller should fall back to
 *  the next transport (BLE). `Rejected` is a device verdict on the pack
 *  itself (no space, invalid pack…) and must not be retried over another
 *  transport. */
sealed interface WifiUploadResult {
  data class Success(val articles: Int) : WifiUploadResult
  data class Rejected(val message: String) : WifiUploadResult
  data object Unreachable : WifiUploadResult
}

/** Streams `.pwp` packs to the firmware's `POST /api/packs/upload?name=…`
 *  over Wi-Fi. Two call sites:
 *
 *  - same-network upload: the device is a station on the phone's network, so
 *    `host` is the device's own IP and the phone never changes networks;
 *  - SoftAP hop: `joinApAndUpload` temporarily joins the device's access
 *    point (`WifiNetworkSpecifier`, user confirms once), uploads to the AP
 *    IP, then releases the network so the phone returns to its saved one.
 *
 *  Android restores the previous network when the specifier request is
 *  released; nothing is persisted on the phone. */
class WifiPackTransfer(context: Context) {
  private val appContext = context.applicationContext
  private val connectivity = appContext.getSystemService(ConnectivityManager::class.java)

  private val reportEvery = 64 * 1024L

  /** Upload over the current network to `host` (IP literal). */
  suspend fun uploadTo(host: String, file: File, name: String, onProgress: (Float) -> Unit): WifiUploadResult =
    withContext(Dispatchers.IO) { post(host, file, name, onProgress, null) }

  /** Temporarily join `ssid` (open network), upload to `host`, then release
   *  the request so Android returns the phone to its saved network. */
  suspend fun joinApAndUpload(ssid: String, host: String, file: File, name: String, onProgress: (Float) -> Unit): WifiUploadResult {
    if (Build.VERSION.SDK_INT < 29) return WifiUploadResult.Unreachable
    val callback = object : ConnectivityManager.NetworkCallback() {
      var settled = false
      @Volatile var continuation: kotlin.coroutines.Continuation<Network?>? = null
      override fun onAvailable(network: Network) {
        if (settled) return
        settled = true
        continuation?.resume(network)
      }

      override fun onUnavailable() {
        if (settled) return
        settled = true
        continuation?.resume(null)
      }
    }
    val specifier = WifiNetworkSpecifier.Builder().setSsid(ssid).build()
    val request = NetworkRequest.Builder()
      .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
      .setNetworkSpecifier(specifier)
      .build()
    val network = withTimeoutOrNull(25_000) {
      suspendCancellableCoroutine { continuation ->
        callback.continuation = continuation
        connectivity.requestNetwork(request, callback)
        continuation.invokeOnCancellation {
          runCatching { connectivity.unregisterNetworkCallback(callback) }
        }
      }
    }
    if (network == null) {
      runCatching { connectivity.unregisterNetworkCallback(callback) }
      return WifiUploadResult.Unreachable
    }
    try {
      /* Use Network.openConnection() so this request is routed through the
       * requested PocketWiki AP even when Android keeps the phone's normal
       * internet network as the process default. This is important for an
       * AP-only network, which intentionally has no validated internet route. */
      return withContext(Dispatchers.IO) { post(host, file, name, onProgress, network) }
    } finally {
      runCatching { connectivity.unregisterNetworkCallback(callback) }
    }
  }

  private fun post(
    host: String,
    file: File,
    name: String,
    onProgress: (Float) -> Unit,
    network: Network?,
  ): WifiUploadResult {
    var connection: HttpURLConnection? = null
    try {
      val url = URL("http://$host/api/packs/upload?name=${URLEncoder.encode(name, "UTF-8")}")
      connection = (network?.openConnection(url) ?: url.openConnection()) as HttpURLConnection
      connection.requestMethod = "POST"
      connection.doOutput = true
      connection.connectTimeout = 8_000
      connection.readTimeout = 30_000
      connection.setRequestProperty("User-Agent", "PocketWiki-Android/1")
      connection.setFixedLengthStreamingMode(file.length().toInt())
      var sent = 0L
      var lastReport = 0L
      connection.outputStream.use { output ->
        file.inputStream().use { input ->
          val chunk = ByteArray(8192)
          while (true) {
            val count = input.read(chunk)
            if (count < 0) break
            output.write(chunk, 0, count)
            sent += count
            if (sent - lastReport >= reportEvery || sent == file.length()) {
              lastReport = sent
              onProgress((sent.toFloat() / file.length()).coerceIn(0f, 1f))
            }
          }
        }
      }
      val code = connection.responseCode
      if (code in 200..299) {
        val body = connection.inputStream.bufferedReader().use { it.readText() }
        val articles = runCatching { JSONObject(body).optInt("articles") }.getOrDefault(0)
        return WifiUploadResult.Success(articles)
      }
      val detail = runCatching {
        connection.errorStream?.bufferedReader()?.use { it.readText() }?.let { JSONObject(it).optString("message") }
      }.getOrNull()?.takeIf { it.isNotBlank() }
      return WifiUploadResult.Rejected(detail ?: "PocketWiki rejected the pack (HTTP $code)")
    } catch (_: IOException) {
      return WifiUploadResult.Unreachable
    } catch (error: kotlin.coroutines.cancellation.CancellationException) {
      throw error
    } catch (_: Exception) {
      return WifiUploadResult.Unreachable
    } finally {
      connection?.disconnect()
    }
  }
}
