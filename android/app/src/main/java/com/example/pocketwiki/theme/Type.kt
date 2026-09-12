package com.example.pocketwiki.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.LineHeightStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/*
 * Material 3's type roles, tuned to the PocketWiki voice: one platform sans,
 * weight and size carrying hierarchy rather than a second display face. Only
 * the roles the app actually uses are pinned; the rest inherit the Material
 * scale so nothing is half-specified.
 */

private val Trim = LineHeightStyle(alignment = LineHeightStyle.Alignment.Center, trim = LineHeightStyle.Trim.None)

private fun role(size: Int, line: Int, weight: FontWeight, tracking: Double = 0.0) =
  TextStyle(
    fontFamily = FontFamily.Default,
    fontWeight = weight,
    fontSize = size.sp,
    lineHeight = line.sp,
    letterSpacing = tracking.sp,
    lineHeightStyle = Trim,
  )

val PocketWikiTypography =
  Typography(
    headlineLarge = role(32, 38, FontWeight.SemiBold, -0.25),
    headlineMedium = role(28, 34, FontWeight.SemiBold, -0.2),
    headlineSmall = role(24, 30, FontWeight.SemiBold, -0.15),
    titleLarge = role(21, 27, FontWeight.SemiBold, -0.1),
    titleMedium = role(16, 22, FontWeight.SemiBold),
    titleSmall = role(14, 20, FontWeight.SemiBold),
    bodyLarge = role(16, 24, FontWeight.Normal),
    bodyMedium = role(14, 20, FontWeight.Normal),
    bodySmall = role(13, 18, FontWeight.Normal),
    labelLarge = role(14, 20, FontWeight.Medium),
    labelMedium = role(12, 16, FontWeight.Medium, 0.1),
    labelSmall = role(11, 16, FontWeight.Medium, 0.4),
  )

/** Softly squared corners: 12 dp for small controls, 16–20 dp for containers. */
val PocketWikiShapes =
  Shapes(
    extraSmall = RoundedCornerShape(8.dp),
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(14.dp),
    large = RoundedCornerShape(18.dp),
    extraLarge = RoundedCornerShape(24.dp),
  )
