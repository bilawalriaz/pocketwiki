---
name: PocketWiki
description: A calm, setup-first companion and offline reading system for a pocket library.
colors:
  forest: "#176b4d"
  forest-deep: "#0f5139"
  forest-soft: "#e6f2ed"
  mossed-slate: "#466457"
  paper: "#f5f7f6"
  surface: "#ffffff"
  surface-low: "#fafcfb"
  surface-container: "#f1f5f3"
  surface-high: "#ebf1ee"
  surface-highest: "#e4ece8"
  surface-variant: "#dde6e1"
  ink: "#17201c"
  ink-muted: "#65716b"
  line: "#dbe2de"
  field-line: "#cbd5d0"
  code-surface: "#eef1ef"
  danger-ink: "#8a2c21"
  danger-surface: "#fff0ee"
  danger-line: "#efc6bf"
  dark-paper: "#000000"
  dark-surface: "#090909"
  dark-container: "#121a16"
  dark-high: "#1a231e"
  dark-highest: "#232d27"
  dark-line: "#2c3733"
  dark-field-line: "#3c4843"
  dark-ink: "#e2e9e5"
  dark-ink-muted: "#bac5bf"
  dark-forest: "#79d4ae"
  dark-on-forest: "#003824"
typography:
  headline:
    fontFamily: "system-ui, -apple-system, Segoe UI, Roboto, sans-serif"
    fontSize: "clamp(26px, 4vw, 34px)"
    fontWeight: 600
    lineHeight: 1.22
    letterSpacing: "-0.025em"
  title:
    fontFamily: "system-ui, -apple-system, Segoe UI, Roboto, sans-serif"
    fontSize: "22px"
    fontWeight: 600
    lineHeight: 1.3
  body:
    fontFamily: "system-ui, -apple-system, Segoe UI, Roboto, sans-serif"
    fontSize: "16px"
    fontWeight: 400
    lineHeight: 1.62
  label:
    fontFamily: "system-ui, -apple-system, Segoe UI, Roboto, sans-serif"
    fontSize: "14px"
    fontWeight: 600
    lineHeight: 1
  mark:
    fontFamily: "ui-monospace, SFMono-Regular, Menlo, monospace"
    fontSize: "17px"
    fontWeight: 750
    lineHeight: 1
rounded:
  compact: "8px"
  control: "10px"
  surface: "12px"
  android-action: "14dp"
  android-card: "16dp"
spacing:
  tight: "8px"
  compact: "14px"
  content: "20px"
  section: "28px"
components:
  button-primary:
    backgroundColor: "{colors.forest}"
    textColor: "{colors.surface}"
    rounded: "{rounded.control}"
    padding: "11px 16px"
    height: "48px"
  button-primary-hover:
    backgroundColor: "{colors.forest-deep}"
    textColor: "{colors.surface}"
    rounded: "{rounded.control}"
    padding: "11px 16px"
    height: "48px"
  android-primary-action:
    backgroundColor: "{colors.forest}"
    textColor: "{colors.surface}"
    rounded: "{rounded.android-action}"
    height: "56dp"
    width: "100%"
  input:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    padding: "10px 12px"
    height: "48px"
  quiet-card:
    backgroundColor: "{colors.surface-low}"
    textColor: "{colors.ink}"
    rounded: "{rounded.android-card}"
    padding: "20dp"
  brand-mark:
    backgroundColor: "{colors.forest}"
    textColor: "{colors.surface}"
    typography: "{typography.mark}"
    rounded: "{rounded.control}"
    size: "34px"
---

# Design System: PocketWiki

## Overview

**Creative North Star: "The Pocket Field Guide"**

PocketWiki feels like a dependable field guide translated into small digital tools and a direct project landing page: warm paper beneath dark ink, a single forest-green signal, and sturdy controls with gently rounded edges. The result is calm, practical, and recognisable without requiring decorative assets or a dense application shell.

The Android companion has two lives. First run is a three-step promise—Device, Wi-Fi, Library—on one pinned rail. After that it is a three-destination tool—Library, Packs, Device—where the device's own state leads every screen and the pack catalogue is one tap away. The embedded web surface reads and manages the same durable offline library. The landing page makes the promise persuasive inside its own presentation frame (see [Landing page](#landing-page)). Across all surfaces, technical machinery stays behind plain-language states, cards contain meaningful status or proof, and each viewport preserves one obvious next action.

**Key Characteristics:**

- Warm paper and ink form the dominant reading field.
- Forest green identifies the brand, the current state, links, focus, and primary action.
- A square `P` is the sole graphic brand signature.
- Controls are generous and rounded; content remains quiet and single-column.
- Android follows Material 3 roles and native behavior — top app bar, navigation bar on phones and a rail once there is width, 48 dp targets, edge-to-edge insets, predictive Back — while the embedded web UI stays lightweight and server-rendered.

## Colors

The palette is a restrained Primary + Neutral system: forest green supplies every affirmative signal, while warm green-cast neutrals keep reading softer than a stark white-and-black interface. Android fills every Material role it can reach from these tokens — a role left on the Material baseline would let a stray violet surface into a green product.

### Primary

- **Pocket Forest** (`forest`): Brand mark, full-width primary actions, links, active states, and progress.
- **Deep Forest** (`forest-deep`): Hover emphasis for web actions and navigation.
- **Mist Green** (`forest-soft`): Quiet hover fills and low-emphasis action backgrounds.
- **Night Mint** (`dark-forest`): The Android dark-theme primary. Android keeps this scheme in both appearances instead of opting into Material You, which would tint the app from the wallpaper and break the One Green Rule.

### Secondary

- **Mossed Slate** (`mossed-slate`): Android's supporting semantic color; it remains subordinate to Pocket Forest and does not create a second accent system.

### Neutral

- **Warm Paper** (`paper`): Default page and app background.
- **Clean Leaf** (`surface`): Header, fields, reading tables, and web containers that need separation from paper.
- **Quiet Leaf** (`surface-low`): Android cards — the one container a screen may use.
- **Rising Leaf** (`surface-container`, `surface-high`, `surface-highest`): The remaining Android surface steps, so pressed, selected, and inset states stay in the family.
- **Future Step** (`surface-variant`): Unfilled progress segments, meter tracks, and other low-emphasis state fills.
- **Deep Ink** (`ink`): Primary reading and interface text.
- **Weathered Ink** (`ink-muted`): Explanations, metadata, and secondary states.
- **Soft Rule** (`line`): Dividers and low-contrast container borders.
- **Field Rule** (`field-line`): Stronger control outlines.
- **Code Wash** (`code-surface`): Inline and block code backgrounds.
- **OLED Black / Night Surface / Night Leaf** (`dark-paper`, `dark-surface`, `dark-container`): Android's dark surface ramp, with true black as the app canvas and a green-cast step for every raised container.
- **Muted Brick** (`danger-ink`, `danger-surface`, `danger-line`): The single failure trio, used only for errors and destructive confirmation.

### Named Rules

**The One Green Rule.** Green carries brand, progress, interaction, and success; do not introduce another decorative accent.

**The Paper First Rule.** Most of every screen remains paper. White or low-tonal surfaces appear only where containment improves scanning.

## Typography

**Display Font:** Platform system sans serif
**Body Font:** Platform system sans serif
**Label/Mono Font:** Platform monospace, reserved for the `P` mark and code

**Character:** Typography is native, direct, and compact enough for embedded delivery. Weight and spacing create hierarchy; no downloaded font or ornamental display face is required by the shipped product surfaces. The landing page alone loads Manrope Variable from a CDN, with the platform sans serif as its fallback.

### Hierarchy

- **Headline** (semibold, responsive 26–34 px on web; Material `headlineLarge` at 32 sp on Android): Direct page and setup-step headings.
- **Title** (semibold, 22 px on web; Material title roles on Android): Section and card titles.
- **Body** (regular, 16 px; 1.62 web line height and 24 sp Android line height): Instructions, article prose, and form support.
- **Label** (semibold, 13–14 px on web; Material label roles on Android): Navigation, compact actions, metadata, and progress labels.
- **Mark** (heavy monospace, 17 px on web; Material title styling on Android): The single letter inside the square brand mark.

### Named Rules

**The Plain-Language Hierarchy Rule.** Headings name the user's immediate task; supporting text explains the next useful fact rather than the underlying radio or transfer protocol.

## Layout

Both surfaces use a single primary column and progressive disclosure rather than dashboard regions.

First run is a wizard: a compact brand header names the step, a pinned three-segment rail shows Device → Wi-Fi → Library, the content scrolls between them, and one full-width primary action stays fixed above the navigation bar. Neither the rail nor the action can scroll out of view.

The installed app is a three-destination tool on a scrolling list. A small top app bar names the destination and carries at most one action. Library leads with the device's own state and the dashboard hand-off, then storage and the installed packs. Packs is the catalogue followed by the two manual routes. Device is the only place radios, addresses and credentials are named. Horizontal content inset is 16 dp inside the app and 24 dp in first-run setup, so cards align with the app bar.

The embedded web reader uses a compact sticky header inside a 960 px maximum frame and an 800 px reading column. At 680 px and below, the header becomes two rows: brand and Manage occupy the first row, and search takes the full second row. Main and footer gutters contract from 20 px to 14 px. Forms and pack management remain stacked at every size.

The project landing page opens with a centred hero inside a 1040 px frame: a gradient-clipped headline, one supporting paragraph, two actions, the CSS device preview and a four-cell statistic strip. Ruled sections follow it in product order, and the catalogue is a full-bleed marquee that spans the viewport. A fixed glass island navigation sits above the page; at 1024 px and below its links collapse into a full-screen overlay, and all section grids stack below 768 px.

Spacing follows a small recurring rhythm: tight adjacency, compact control gaps, content padding, and larger section separation. Android also guarantees 48 dp touch targets, scrollable content, edge-to-edge system insets, font scaling, and predictive Back through native behavior.

### Named Rules

**The Three-Step Promise Rule.** First-run setup progresses from Device to Wi-Fi to Library; never present those stages as peer dashboard modules or radio/file controls.

**The Three-Destination Rule.** Once setup is done, the app has exactly three destinations — Library, Packs, Device — reachable from a navigation bar on a phone and a rail on anything wider. System Back returns to Library from anywhere else and never leaves the app from the last screen. A destination is a place, never an action.

**The One-Action Rule.** Each screen carries one primary action, and it belongs to the thing that screen is about: connect on Library, install on Packs, save Wi-Fi on Device. Secondary routes are text or outlined buttons below it.

**The Declared-Motion Rule.** Every animation comes from `Motion.kt`; no screen invents its own duration or curve. Structure uses Material's timed transitions, state uses springs, and nothing animates off a timer the user did not ask for.

**The One-Column Rule.** Reading, setup, and management stay linear. Additional capability appears below the current task instead of beside it.

## Elevation & Depth

Depth is quiet and mostly tonal. Android uses Material surface roles with 1 dp tonal elevation on the top bar and discovery card. Web headers use a hairline rule and near-imperceptible shadow; article lists may use one diffuse low shadow, while pack lists stay flat. No layered card stack or ornamental shadow vocabulary is present.

### Shadow Vocabulary

- **Ambient List:** A paired 1 px contact shadow and broad 24 px ambient shadow; used only on the web article-list container.
- **Header Hairline:** A single 1 px shadow beneath the sticky web header; secondary to its divider.

### Named Rules

**The Flat-by-Default Rule.** Use borders and tonal surfaces first. Elevation is reserved for a sticky bar or a container that genuinely separates a browsable list from the reading field.

## Shapes

The shape language is softly squared, not pill-shaped. Compact interactive elements use 8–10 px corners on the web; containers use 12 px. Android scales the same language: 10 dp for the small step in the shape ramp, 14 dp for primary actions, 18 dp for cards, and 24 dp for the largest containers. Circular geometry appears only in status dots, progress segments, and loading indicators. Thin neutral borders define web fields, reading tables, code, quotations, and lists.

**The Square P Rule.** The green brand mark remains a compact rounded square containing one `P`; do not replace it with a circular avatar, external icon, or illustration.

## Components

### Buttons

- **Shape:** Gently squared controls; 10–11 px on web and 14 dp for the Android primary action.
- **Primary:** Forest fill with light text. Web form actions are at least 48 px high; Android's single action spans the content width at 56 dp.
- **Hover / Focus:** Web hover deepens the forest fill. Keyboard focus uses a visible translucent green outline with offset; active search presses move by 1 px unless reduced motion is requested.
- **Secondary / Ghost:** Android uses Material outlined and text buttons for file, URL, pack, and recovery actions. Web inline actions use Mist Green; destructive removal uses the dedicated danger surface and ink.

### Cards / Containers

- **Corner Style:** 12 px web containers and 16 dp Android status or pack cards.
- **Background:** Clean Leaf on web and Quiet Leaf through Android's low surface role.
- **Shadow Strategy:** Flat for status and management; only browsable web article lists receive Ambient List elevation.
- **Border:** Web containers use Soft Rule; Android cards take a 1 dp `outlineVariant` hairline on the low surface role — one edge treatment per card, never a border under a shadow.
- **Internal Padding:** 10–14 px for web rows; 18–20 dp for Android card content.

### Inputs / Fields

- **Style:** Full-width, quiet light surface, Field Rule outline, softly squared corners, and native 16 px/sp input text.
- **Focus:** Border shifts to Pocket Forest and gains a translucent green outer ring on the web; Android uses Material outlined-field semantics.
- **Error / Disabled:** Errors use the dedicated muted-red trio rather than reusing green. Disabled actions lower emphasis and preserve their label; busy states use progress text or a native circular indicator.

### Navigation

The reader header pairs the square mark and wordmark with one `Manage` link and title search. The landing header is a floating glass island: a wordmark with no status dot, compact anchor navigation, a light/dark toggle and one primary action. Navigation is muted at rest and brightens on hover or when its section is in view.

Android carries a navigation bar on compact widths and a navigation rail from 640 dp up, both on the low surface role, with the same three destinations and the same icons. System Back returns to Library from Packs or Device, and leaves the app from Library. First-run setup has no navigation chrome at all: one pinned rail and one pinned action carry the whole flow.

### Device Showcase

The landing hero depicts the device with CSS geometry rather than decorative imagery: a softly squared light enclosure, a dark bezel around a lit screen showing the reader, a small status light, a floating card at its lower right, and faint circular guides behind it. The component is the original one, kept intact — markup, geometry and behaviour — with its colours resolving to the landing palette through tokens scoped to `.device-scene`, and its enclosure, bezel and status light keeping their own material colours. The screen keeps the light paper-and-ink palette in both site themes, because it is showing the page the device actually serves. Its app header is set in type alone: the coloured square `P` is a product-surface mark and does not appear inside the preview. Two text values inside the screen are darkened for legibility on the lit panel, and nothing else in the component changes.

### Search

The web search field and button form one joined 42 px-high control. The field owns the left corners, the primary action owns the right corners, and the shared seam reads as one task.

### Motion

Motion is declared once, in `ui/main/Motion.kt`, and every screen animates through it.

- **Structure uses timed curves.** A destination change, a setup step, and a disclosure all use Material's transitions: the outgoing side leaves in 90 ms on an accelerating curve, the incoming side arrives in 210–300 ms on a decelerating one. `fadeThrough()` switches peer destinations — the bottom-navigation tabs, and a body that swaps between its empty state and its content — by fading out fast and fading the newcomer in from 92% scale. `sharedAxisX()` moves a setup step in the direction of travel, so a wizard has a direction the user can feel.
- **State uses springs.** Colours, alphas, and progress values use critically damped springs, so an interrupted value keeps its velocity instead of restarting.
- **Nothing bounces.** Travel and height changes are critically damped; only settled values are allowed the smallest overshoot, and none is used today.
- **Transforms only.** Everything animates through graphics-layer translation, scale, and alpha, so a transition costs a render pass and never a layout pass. Lists animate their own reflow with `Modifier.animateItem()`.
- **The display rate is requested, not assumed.** A 120 Hz panel does not give an app 120 Hz frames on its own: without a request the platform treats the window as ordinary content and lifts the refresh rate only while a finger is moving, so a transition that plays after the touch ends arrives at 60 Hz. The window therefore asks the view hierarchy for the display's current mode rate (`View.setRequestedFrameRate` on Android 14+, `preferredRefreshRate` below it) and follows the user down to a slower mode if they choose one.
- **The system scale wins.** Compose scales every animation by the platform animator duration, so Android's "remove animations" setting makes all of them instant.
- **No entrance choreography.** Content arrives because the user asked for it, never on a timer.

Measured on a 120 Hz phone from `dumpsys gfxinfo`: release build, 2 766 frames across every destination and scroll — 50th, 90th, and 95th percentile frame work 5 ms against an 8.3 ms budget, no missed vsyncs, 0.04% janky frames, and an 8.30 ms presentation cadence during animation.

### Icons

One set, one weight: Material Symbols Rounded, fill 1, on the 24 dp grid. The app ships only the glyphs it draws, generated into `PocketIcons` by `tools/generate_android_icons.py`, so no icon pack sits in the APK and no two sets can drift apart. Icons never stand in for text that carries meaning, decorative ones stay undescribed, and every interactive one names its action.

### Progress Indicator

Three 4 dp segments with their labels anchor the Android setup sequence, pinned directly under the header so they never scroll away. Completed and current segments are green; future segments use the surface-variant neutral, while only the current label becomes green. Completed steps are tappable; future ones are not.

## Landing page

The page at `educated.space/pocketwiki` is a presentation surface, not a product surface. It runs its own dark-first token set so it can carry more contrast and motion than the embedded reader, but every fact it states comes from this repository.

### Landing tokens

- **Canvas:** `#000000` dark and `#f5f5f7` light, with `#1F1F1F` / `#ffffff` cards, `#313131` / `#e3e3e6` borders, `#f5f5f7` / `#1d1d1f` text, and `#a1a1a6` / `#58585d` secondary text with `#8e8e93` / `#6e6e73` for metadata. Every pairing clears 4.5:1 on its own surface, including the small meta line.
- **Signal:** one teal accent, `#2db89a` in the dark theme and `#12705c` in the light theme, with no second accent. The light value is deliberately deeper than the dark one so accent text and 12 px labels stay readable on paper.
- **Type:** Manrope Variable from the `@fontsource-variable/manrope` CDN, falling back to the platform sans serif; code uses the platform monospace.
- **Shape:** 8 px controls, 12–24 px containers, and a full radius only for chips and pills. The navigation island uses 14 px with 12 px link radii.
- **Motion:** 150–800 ms transitions on `cubic-bezier(0.32, 0.72, 0, 1)`, section reveals on scroll, and digit pop-in on the hero statistics.
- **Browser surfaces:** selection, caret, focus ring, scrollbars, underline offset and tabular numerals are themed from the landing tokens rather than left at browser defaults.
- **Controls:** `--control-line` (`#6e6e73` dark, `#8e8e93` light) outlines buttons, tabs and fields so a control boundary clears 3:1 against its own backdrop. Container hairlines stay on `--border`.

### Landing structure

- A fixed glass island navigation with anchor links, a light/dark toggle and one primary action. Rows are 40 px on a 14 px pill; below 1024 px the links collapse into a full-screen overlay. The wordmark carries no status dot and the island carries no coloured brand mark.
- A centred hero: gradient-clipped headline, one supporting paragraph, two actions, the CSS device preview, and a four-cell statistic strip whose figures are the catalogue totals and the baseline flash size.
- Ruled sections in product order: catalogue marquee, setup steps, reader screenshots, offline guarantees, board comparison with the field manifest, documentation viewer, FAQ, and one closing action.
- Anchor jumps clear the floating island, so no heading lands underneath it.
- The catalogue marquee pauses on hover or focus, and becomes scrollable rather than moving when reduced motion is requested.
- Browser storage holds only the chosen theme; the page loads no analytics, fonts beyond the typeface, or third-party scripts.

### Named Rules

**The One Frame Rule.** Gradient, glass and scroll motion belong to the landing frame alone. The embedded reader and the Android app keep the calm paper-and-ink system, and neither vocabulary leaks into the other.

**The Sourced Numbers Rule.** Every count, capacity and price statement on the landing page comes from `packs/catalog-source.json` in the `pocketwiki-content` checkout, the partition tables, or the host tooling. The page states no testimonials, benchmarks or commercial claims.

## Do's and Don'ts

### Do:

- **Do** keep one full-width primary action pinned in the Android setup flow, and one primary action per destination afterwards.
- **Do** let warm paper dominate and use cards only for meaningful state or grouped rows.
- **Do** preserve native Material semantics, the brand colour scheme in both appearances, dark theme, font scaling, touch targets, and system Back on Android.
- **Do** retain strong keyboard focus, responsive text, and reduced-motion behavior on the web.
- **Do** stack landing-page grids into a single reading column below 768 px and keep the catalogue reachable without animation.
- **Do** keep destructive pack removal visually subdued and unavailable for the starter library.

### Don't:

- **Don't** expose the setup stages as a permanent tab bar; the three-step promise belongs to first run, and the installed app's destinations are Library, Packs, and Device.
- **Don't** use green as decorative fill across large areas; its scarcity keeps actions and state legible.
- **Don't** introduce gradients, glass effects, ornamental illustration, or competing brand marks into the product surfaces.
- **Don't** place management controls into the article reading flow unless they directly support navigation or download.
- **Don't** expose BLE, AP/STA, storage, or transfer implementation detail unless it helps the user recover.
