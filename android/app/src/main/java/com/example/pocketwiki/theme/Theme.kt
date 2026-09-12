package com.example.pocketwiki.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

/*
 * Every Material role the app can reach is filled here. Leaving a role on the
 * Material baseline would let a stray violet surface into a green product, so
 * the schemes are complete rather than partially overridden.
 */

private val LightScheme =
  lightColorScheme(
    primary = Forest,
    onPrimary = Color.White,
    primaryContainer = ForestSoft,
    onPrimaryContainer = ForestDeep,
    inversePrimary = NightForest,
    secondary = MossedSlate,
    onSecondary = Color.White,
    secondaryContainer = SurfaceVariant,
    onSecondaryContainer = Ink,
    tertiary = MossedSlate,
    onTertiary = Color.White,
    tertiaryContainer = ForestSoft,
    onTertiaryContainer = ForestDeep,
    error = DangerInk,
    onError = Color.White,
    errorContainer = DangerSurface,
    onErrorContainer = Color(0xFF601A12),
    background = Paper,
    onBackground = Ink,
    surface = Paper,
    onSurface = Ink,
    surfaceVariant = SurfaceVariant,
    onSurfaceVariant = InkMuted,
    surfaceTint = Forest,
    surfaceContainerLowest = Color.White,
    surfaceContainerLow = Color.White,
    surfaceContainer = SurfaceContainer,
    surfaceContainerHigh = SurfaceHigh,
    surfaceContainerHighest = SurfaceHighest,
    inverseSurface = Ink,
    inverseOnSurface = Paper,
    outline = FieldLine,
    outlineVariant = Line,
    scrim = Color.Black,
  )

private val DarkScheme =
  darkColorScheme(
    primary = NightForest,
    onPrimary = NightOnForest,
    primaryContainer = Color(0xFF0F3D2C),
    onPrimaryContainer = Color(0xFFA7EBCB),
    inversePrimary = Forest,
    secondary = Color(0xFFAFC9BB),
    onSecondary = Color(0xFF1B2E25),
    secondaryContainer = Color(0xFF2A3A32),
    onSecondaryContainer = Color(0xFFCBE3D8),
    tertiary = Color(0xFFAFC9BB),
    onTertiary = Color(0xFF1B2E25),
    tertiaryContainer = Color(0xFF2A3A32),
    onTertiaryContainer = Color(0xFFCBE3D8),
    error = NightDangerInk,
    onError = NightDangerSurface,
    errorContainer = Color(0xFF7F2A1C),
    onErrorContainer = Color(0xFFFFDAD2),
    background = NightPaper,
    onBackground = NightInk,
    surface = NightPaper,
    onSurface = NightInk,
    surfaceVariant = NightHigh,
    onSurfaceVariant = NightInkMuted,
    surfaceTint = NightForest,
    surfaceContainerLowest = NightPaper,
    surfaceContainerLow = NightLow,
    surfaceContainer = NightContainer,
    surfaceContainerHigh = NightHigh,
    surfaceContainerHighest = NightHighest,
    inverseSurface = NightInk,
    inverseOnSurface = NightPaper,
    outline = NightFieldLine,
    outlineVariant = NightLine,
    scrim = Color.Black,
  )

/**
 * PocketWiki keeps its own forest-green scheme in both appearances. Material You
 * would tint the app from the user's wallpaper, which breaks the brand's
 * single-green rule, so the app does not opt into dynamic colour.
 */
@Composable
fun PocketWikiTheme(
  darkTheme: Boolean = isSystemInDarkTheme(),
  content: @Composable () -> Unit,
) {
  MaterialTheme(
    colorScheme = if (darkTheme) DarkScheme else LightScheme,
    typography = PocketWikiTypography,
    shapes = PocketWikiShapes,
    content = content,
  )
}
