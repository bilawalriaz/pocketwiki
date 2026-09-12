package com.example.pocketwiki.device

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

data class CatalogPack(
  val id: String,
  val name: String,
  val version: Int,
  val description: String,
  val articles: Int,
  val bytes: Long,
  val sha256: String,
  val url: String,
)

data class PackCatalogState(
  val packs: List<CatalogPack> = emptyList(),
  val loading: Boolean = false,
  val message: String? = null,
  val syncedAt: String? = null,
)

class PackCatalog(private val context: Context, private val scope: CoroutineScope) {
  companion object {
    private const val CATALOG_URL = "https://packs.educated.space/index.json"
    private const val MAX_CATALOG_BYTES = 131_072
  }

  private val cache = File(context.filesDir, "pack_catalog.json")
  private val initial = loadInitial()
  private val _state = MutableStateFlow(PackCatalogState(initial, loading = true))
  val state: StateFlow<PackCatalogState> = _state

  init { refresh() }

  fun refresh() {
    _state.value = _state.value.copy(loading = true, message = null)
    scope.launch(Dispatchers.IO) {
      try {
        // The catalogue is a short-lived pointer. Add a unique query so a
        // phone, captive portal, or intermediary cannot keep returning an
        // older cached index after a release.
        val syncUrl = "$CATALOG_URL?sync=${System.currentTimeMillis()}"
        val connection = URL(syncUrl).openConnection() as HttpURLConnection
        try {
          connection.connectTimeout = 10_000
          connection.readTimeout = 15_000
          connection.useCaches = false
          connection.setRequestProperty("Accept", "application/json")
          connection.setRequestProperty("Cache-Control", "no-cache, no-store")
          connection.setRequestProperty("Pragma", "no-cache")
          connection.setRequestProperty("User-Agent", "PocketWiki-Android/1")
          if (connection.responseCode !in 200..299) error("Catalogue returned ${connection.responseCode}")
          val bytes = connection.inputStream.use { input ->
            val output = ArrayList<Byte>()
            val buffer = ByteArray(4096)
            while (true) {
              val count = input.read(buffer)
              if (count < 0) break
              if (output.size + count > MAX_CATALOG_BYTES) error("Catalogue is too large")
              for (index in 0 until count) output.add(buffer[index])
            }
            output.toByteArray()
          }
          val text = bytes.toString(Charsets.UTF_8)
          val packs = parse(text)
          cache.writeText(text, Charsets.UTF_8)
          val stamp = java.time.LocalTime.now().format(java.time.format.DateTimeFormatter.ofPattern("HH:mm"))
          _state.value = PackCatalogState(packs, syncedAt = stamp)
        } finally {
          connection.disconnect()
        }
      } catch (error: Exception) {
        val detail = error.message?.takeIf { it.isNotBlank() }
          ?: error::class.simpleName
          ?: "unknown network error"
        _state.value = PackCatalogState(
          packs = _state.value.packs,
          message = if (_state.value.packs.isEmpty()) "Catalogue sync failed: $detail"
                    else "Catalogue sync failed: $detail. Showing the saved catalogue.",
        )
      }
    }
  }

  private fun loadInitial(): List<CatalogPack> {
    val cached = runCatching { if (cache.isFile) parse(cache.readText()) else emptyList() }.getOrDefault(emptyList())
    if (cached.isNotEmpty()) return cached
    return runCatching {
      context.assets.open("pack_catalog.json").bufferedReader().use { parse(it.readText()) }
    }.getOrDefault(emptyList())
  }

  private fun parse(text: String): List<CatalogPack> {
    val source = JSONObject(text)
    require(source.optInt("schema") == 1) { "Unsupported catalogue" }
    val array = source.getJSONArray("packs")
    // The catalogue is bounded by MAX_CATALOG_BYTES above, not by a pack count:
    // a fixed ceiling silently blanked the whole list the moment the published
    // catalogue grew past it.
    require(array.length() >= 1) { "Empty catalogue" }
    val ids = HashSet<String>(array.length())
    return buildList {
      for (index in 0 until array.length()) {
        val item = array.getJSONObject(index)
        val id = item.getString("id")
        val url = item.getString("url")
        val sha = item.getString("sha256").lowercase()
        require(id.matches(Regex("[a-z0-9-]{1,40}"))) { "Invalid pack id" }
        require(ids.add(id)) { "Duplicate pack id" }
        require(URL(url).protocol == "https" && URL(url).host == "packs.educated.space") { "Invalid pack URL" }
        require(sha.matches(Regex("[a-f0-9]{64}"))) { "Invalid pack checksum" }
        require(item.getString("name").isNotBlank()) { "Invalid pack name" }
        require(item.getInt("version") > 0) { "Invalid pack version" }
        require(item.getInt("articles") > 0) { "Invalid article count" }
        require(item.getLong("bytes") in 1L..2_097_152L) { "Invalid pack size" }
        add(CatalogPack(
          id = id,
          name = item.getString("name"),
          version = item.getInt("version"),
          description = item.getString("description"),
          articles = item.getInt("articles"),
          bytes = item.getLong("bytes"),
          sha256 = sha,
          url = url,
        ))
      }
    }
  }
}
