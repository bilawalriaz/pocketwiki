package com.example.pocketwiki.device

data class PackInfo(val name: String, val articles: Int, val bytes: Long, val enabled: Boolean)

/** The largest pack the firmware accepts. The host and the device both enforce it. */
const val MAX_PACK_BYTES: Long = 2_097_152L

sealed interface DeviceNetworkState {
  data object Idle : DeviceNetworkState
  data object Requesting : DeviceNetworkState
  data class Busy(val message: String, val progress: Float? = null, val cancellable: Boolean = false) : DeviceNetworkState
  data class Ready(
    val packs: List<PackInfo>,
    val used: Long,
    val total: Long,
    val installable: Long,
  ) : DeviceNetworkState
  data class Error(val message: String) : DeviceNetworkState
}
