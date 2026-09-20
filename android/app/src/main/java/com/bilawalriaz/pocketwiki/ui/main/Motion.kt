package com.bilawalriaz.pocketwiki.ui.main

import androidx.compose.animation.AnimatedContentTransitionScope
import androidx.compose.animation.ContentTransform
import androidx.compose.animation.SizeTransform
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.Easing
import androidx.compose.animation.core.FiniteAnimationSpec
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.runtime.Composable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.ui.unit.IntSize

/*
 * Motion, declared once.
 *
 * Two kinds of change live here, and they use different physics on purpose.
 *
 * Structure — arriving at a destination, moving through a setup step, opening a
 * panel — uses Material's timed transitions: a decelerating curve that is quick
 * off the mark and lands softly. That curve is what makes a screen change read
 * as designed rather than as a cut; a soft spring would keep drifting for half a
 * second and read as lag no matter how fast the panel runs.
 *
 * State — a colour, a progress value, a size that settles — uses spring specs,
 * so an interrupted value keeps its velocity instead of restarting.
 *
 * Everything animates through graphics-layer transforms and alpha, so a
 * transition costs a render pass and never a layout pass. Compose scales all of
 * it by the platform animator duration, so Android's "remove animations"
 * developer setting makes every one of them instant.
 */

/** Decelerates into place: most of the travel happens in the first third. */
private val Arrive: Easing = CubicBezierEasing(0.2f, 0f, 0f, 1f)

/** Leaves promptly, so the arriving content owns the screen. */
private val Leave: Easing = CubicBezierEasing(0.3f, 0f, 0.8f, 0.15f)

private const val LeaveMillis = 90
private const val ArriveMillis = 210
private const val TravelMillis = 300
private const val DisclosureMillis = 250

/** Colour, alpha, and progress values: fast and critically damped. */
@Composable
@ReadOnlyComposable
fun <T> effectsMotion(): FiniteAnimationSpec<T> =
  spring(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = 1600f)

/** A shorter variant for small, frequent changes such as a status colour. */
@Composable
@ReadOnlyComposable
fun <T> fastEffectsMotion(): FiniteAnimationSpec<T> =
  spring(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = 3800f)

/** Opening and closing a panel in place, where the height itself animates. */
fun disclosureTiming(): FiniteAnimationSpec<IntSize> = tween(DisclosureMillis, easing = Arrive)

/**
 * Material's fade-through: the outgoing destination fades out fast, then the
 * incoming one fades in from 92% scale. Used where two destinations are peers —
 * the bottom-navigation tabs, and a body that swaps between an empty state and
 * its content — so the change reads as one thing replacing another rather than
 * as travel.
 */
fun fadeThrough(): ContentTransform {
  val enter =
    fadeIn(animationSpec = tween(ArriveMillis, delayMillis = LeaveMillis, easing = LinearEasing)) +
      scaleIn(initialScale = 0.92f, animationSpec = tween(ArriveMillis, delayMillis = LeaveMillis, easing = Arrive))
  val exit = fadeOut(animationSpec = tween(LeaveMillis, easing = LinearEasing))
  // Nothing here changes size, so the transform is only there to stop the
  // outgoing screen from being clipped mid-fade.
  return ContentTransform(enter, exit, 0f, SizeTransform(clip = false))
}

/**
 * Material's shared-axis X: stepping forward or back through a sequence. The
 * outgoing step leaves in the direction of travel while the incoming one
 * arrives from the other side, so a wizard has a direction you can feel.
 */
fun AnimatedContentTransitionScope<*>.sharedAxisX(forward: Boolean): ContentTransform {
  val sign = if (forward) 1 else -1
  val enter =
    slideInHorizontally(animationSpec = tween(TravelMillis, easing = Arrive)) { width -> sign * width / 2 } +
      fadeIn(animationSpec = tween(TravelMillis, delayMillis = LeaveMillis, easing = LinearEasing))
  val exit =
    slideOutHorizontally(animationSpec = tween(TravelMillis, easing = Arrive)) { width -> -sign * width / 2 } +
      fadeOut(animationSpec = tween(LeaveMillis, easing = Leave))
  return ContentTransform(enter, exit, 0f, SizeTransform(clip = false))
}
