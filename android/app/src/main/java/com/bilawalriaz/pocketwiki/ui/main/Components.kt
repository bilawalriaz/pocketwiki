package com.bilawalriaz.pocketwiki.ui.main

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.bilawalriaz.pocketwiki.device.CatalogPack
import com.bilawalriaz.pocketwiki.device.MAX_PACK_BYTES
import com.bilawalriaz.pocketwiki.ui.icons.PocketIcons

/*
 * The shared furniture every PocketWiki screen is assembled from: the square P,
 * one quiet card, one heading rhythm, one row, one meter. Screens compose these
 * instead of inventing their own padding and surfaces.
 */

/** The sole PocketWiki graphic signature: a forest square holding one heavy `P`. */
@Composable
fun BrandMark(size: Dp = 40.dp) {
  Box(
    Modifier
      .size(size)
      .background(MaterialTheme.colorScheme.primary, RoundedCornerShape(size * 0.3f))
      .clearAndSetSemantics {},
    contentAlignment = Alignment.Center,
  ) {
    Text(
      "P",
      color = MaterialTheme.colorScheme.onPrimary,
      fontFamily = FontFamily.Monospace,
      fontWeight = FontWeight.Bold,
      fontSize = (size.value * 0.46f).sp,
    )
  }
}

/** The one container: tonal surface, hairline rule, no shadow. */
@Composable
fun QuietCard(
  modifier: Modifier = Modifier,
  content: @Composable ColumnScope.() -> Unit,
) {
  Surface(
    modifier = modifier.fillMaxWidth(),
    shape = MaterialTheme.shapes.large,
    color = MaterialTheme.colorScheme.surfaceContainerLow,
    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
  ) {
    Column(Modifier.fillMaxWidth(), content = content)
  }
}

/** A section heading with an optional support line and trailing action. */
@Composable
fun SectionHeading(
  title: String,
  supporting: String? = null,
  action: @Composable (() -> Unit)? = null,
) {
  Row(
    Modifier.fillMaxWidth().padding(top = 12.dp, bottom = 8.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    Column(Modifier.weight(1f)) {
      Text(title, style = MaterialTheme.typography.titleMedium)
      if (supporting != null) {
        Text(supporting, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
      }
    }
    action?.invoke()
  }
}

/** One labelled fact inside a card: icon, label, value, optional trailing content. */
@Composable
fun InfoRow(
  icon: ImageVector,
  label: String,
  value: String,
  modifier: Modifier = Modifier,
  iconTint: Color = MaterialTheme.colorScheme.onSurfaceVariant,
  trailing: @Composable (() -> Unit)? = null,
) {
  Row(
    modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
    verticalAlignment = Alignment.CenterVertically,
  ) {
    Icon(icon, null, Modifier.size(20.dp), tint = iconTint)
    Spacer(Modifier.width(14.dp))
    Column(Modifier.weight(1f)) {
      Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
      Text(value, style = MaterialTheme.typography.bodyLarge)
    }
    trailing?.invoke()
  }
}

@Composable
fun CardDivider() {
  HorizontalDivider(Modifier.padding(horizontal = 16.dp), color = MaterialTheme.colorScheme.outlineVariant)
}

/** A titled notice inside a card: state first, explanation under it, one way out. */
@Composable
fun NoticeCard(
  modifier: Modifier = Modifier,
  icon: ImageVector,
  title: String,
  body: String? = null,
  iconTint: Color = MaterialTheme.colorScheme.onSurfaceVariant,
  action: @Composable (() -> Unit)? = null,
) {
  QuietCard(modifier) {
    Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.Top) {
      Icon(icon, null, Modifier.size(20.dp), tint = iconTint)
      Spacer(Modifier.width(14.dp))
      Column(Modifier.weight(1f)) {
        Text(title, style = MaterialTheme.typography.bodyLarge)
        if (body != null) {
          Spacer(Modifier.height(3.dp))
          Text(body, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
      }
    }
    if (action != null) {
      CardDivider()
      Row(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) { action() }
    }
  }
}

/** The state vocabulary shared by every screen. */
enum class StatusTone { Good, Working, Attention }

@Composable
fun StatusTone.color(): Color = when (this) {
  StatusTone.Good -> MaterialTheme.colorScheme.primary
  StatusTone.Working -> MaterialTheme.colorScheme.onSurface
  StatusTone.Attention -> MaterialTheme.colorScheme.error
}

/** The same tone, eased, so a state change reads as motion rather than a cut. */
@Composable
fun StatusTone.animatedColor(): Color =
  animateColorAsState(color(), fastEffectsMotion<Color>(), label = "statusTone").value

/** A leading status dot plus its label — state, never decoration. */
@Composable
fun StatusLine(text: String, tone: StatusTone, modifier: Modifier = Modifier) {
  Row(modifier, verticalAlignment = Alignment.CenterVertically) {
    Box(Modifier.size(8.dp).background(tone.animatedColor(), CircleShape))
    Spacer(Modifier.width(8.dp))
    Text(text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
  }
}

/** A plain determinate bar. Material's gap and stop indicator are for unknown progress. */
@Composable
fun MeterBar(progress: Float, modifier: Modifier = Modifier, description: String? = null) {
  LinearProgressIndicator(
    progress = { progress.coerceIn(0f, 1f) },
    modifier = modifier
      .fillMaxWidth()
      .height(8.dp)
      .then(if (description == null) Modifier else Modifier.semantics { contentDescription = description }),
    color = MaterialTheme.colorScheme.primary,
    trackColor = MaterialTheme.colorScheme.surfaceVariant,
    strokeCap = StrokeCap.Round,
    gapSize = 0.dp,
    drawStopIndicator = {},
  )
}

/** Flash use, stated as a quantity and a shape. */
@Composable
fun StorageMeter(used: Long, total: Long, installable: Long) {
  val fraction = if (total > 0) used.toFloat() / total else 0f
  val shown = animateFloatAsState(fraction, effectsMotion(), label = "storageFraction")
  Column(Modifier.fillMaxWidth().padding(16.dp)) {
    Row(verticalAlignment = Alignment.CenterVertically) {
      Icon(PocketIcons.Storage, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
      Spacer(Modifier.width(14.dp))
      Text("Flash storage", Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
      Text(
        "${formatBytes(used)} of ${formatBytes(total)}",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    }
    Spacer(Modifier.height(14.dp))
    MeterBar(shown.value, description = "Flash storage ${formatBytes(used)} of ${formatBytes(total)} used")
    Spacer(Modifier.height(10.dp))
    Text(
      "${formatBytes((total - used).coerceAtLeast(0))} free · up to ${formatBytes(minOf(installable, MAX_PACK_BYTES))} per pack",
      style = MaterialTheme.typography.bodySmall,
      color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
  }
}

/** A working row inside a card: spinner, one line of plain language, optional progress. */
@Composable
fun BusyRow(message: String, progress: Float? = null) {
  val shown = animateFloatAsState(progress ?: 0f, effectsMotion(), label = "busyProgress")
  Column(Modifier.fillMaxWidth().padding(16.dp)) {
    Row(verticalAlignment = Alignment.CenterVertically) {
      CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.5.dp)
      Spacer(Modifier.width(14.dp))
      Text(message, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
    }
    if (progress != null) {
      Spacer(Modifier.height(14.dp))
      MeterBar(shown.value)
      Spacer(Modifier.height(8.dp))
      Text(
        "${(shown.value.coerceIn(0f, 1f) * 100).toInt()}%",
        style = MaterialTheme.typography.labelMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
      )
    }
  }
}

/** Teach the interface rather than announce emptiness. */
@Composable
fun EmptyState(
  icon: ImageVector,
  title: String,
  body: String,
  action: @Composable (() -> Unit)? = null,
) {
  Column(
    Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 26.dp),
    horizontalAlignment = Alignment.CenterHorizontally,
  ) {
    Icon(icon, null, Modifier.size(26.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
    Spacer(Modifier.height(14.dp))
    Text(title, style = MaterialTheme.typography.titleMedium, textAlign = TextAlign.Center)
    Spacer(Modifier.height(6.dp))
    Text(
      body,
      style = MaterialTheme.typography.bodyMedium,
      color = MaterialTheme.colorScheme.onSurfaceVariant,
      textAlign = TextAlign.Center,
    )
    if (action != null) {
      Spacer(Modifier.height(18.dp))
      action()
    }
  }
}

/** Bytes the way a person reads them. */
fun formatBytes(bytes: Long): String = when {
  bytes >= 1_048_576 -> "%.1f MB".format(bytes / 1_048_576.0)
  bytes >= 1024 -> "%.0f KB".format(bytes / 1024.0)
  else -> "$bytes B"
}

/** Pack ids become titles only when the catalogue has no better name. */
fun displayPackName(name: String, catalog: List<CatalogPack>): String {
  if (name == STARTER_PACK) return "Biology, Health & Medicine"
  return catalog.firstOrNull { it.id == name }?.name
    ?: name.replace('-', ' ').replace('_', ' ')
      .split(' ')
      .filter { it.isNotEmpty() }
      .joinToString(" ") { word -> word.replaceFirstChar { it.uppercase() } }
}

const val STARTER_PACK = "biology-health"
