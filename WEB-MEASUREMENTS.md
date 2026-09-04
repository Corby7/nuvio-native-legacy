# Web app measurements — the 1:1 port reference

Everything here was **measured in the running web app** at 1920×1080, with
`getBoundingClientRect` and `getComputedStyle`, not read from the stylesheet.

Why that matters: reading the CSS misleads. `.player-title` declares
`font-size: 28px` at `components.css:12582` and is **overridden** by
`#playerUiRoot` at 14632 to `min(2.92vw, 56px)` = 56px. Reading only the first
block puts you out by half.

How to reproduce:

```bash
cd ../NuvioWeb-0.3.38-beta && npm run serve     # port 4173
```

Open at 1920×1080 and measure from the console.

> ## ⚠️ Two sessions, two sets of numbers
>
> Everything this file contained until 2026-08-31 was measured **signed out**
> ("Continue without an account"). The owner then signed in (profile "Henrique")
> and several screens changed — some because there was more data, **others
> because of profile preferences**. Each section below says which session it
> came from.
>
> How to reproduce the signed-in session: `localStorage` is per ORIGIN, so any
> tab on `http://localhost:4173` inherits the session. **Do not clear
> localStorage on that address** — it breaks the pairing and forces the owner to
> redo the QR on their phone.


## Home — SIGNED-OUT session (what is implemented today)


| element | value |
|---|---|
| rail | 144 wide, full height |
| nav items | 96×104 at x=24; first at y=340; step 144 |
| content | starts at x=248 (rail 144 + 104) |
| hero art | x=555, y=0, 1421×670, `object-fit: cover` |
| hero logo | 440×160 at (248, 135) |
| title without a logo | 76px, weight 600, letter-spacing −2.28 |
| meta line | y=327, h=52, font 21, weight 500, `rgb(179,179,179)` |
| synopsis | y=411, width 640, h 89, font 22, weight 400, ls 0.5 |
| row title | y=518, h=31, font 26, weight 600, ls −0.52 |
| row 0 cards | y=564 |
| card | 212×322, radius 24 |
| gap between cards | 60 (step 272) |
| step between rows | 416 |
| poster title | 16, weight 500 |
| poster subtitle | 13, weight 400, `rgba(255,255,255,0.7)` |

### Hero gradients

On the pseudo-elements of `.home-modern-hero-media`:

- `::before` — horizontal, covering the **leftmost 639px** of 1421 (45% of the
  UV): `#0d0d0d` → 0.86 at 22% → 0.56 at 46% → 0.16 at 76% → 0
- `::after` — vertical, full height:
  0 until 82% → 0.25 at 89.2% → 0.65 at 95.5% → solid at the end

These are **piecewise linear** ramps. A single `smoothstep` does not pass through
the intermediate points (at 89.2% it gives 0.35 instead of 0.25), and it is the
middle of the ramp that you actually see. Implemented with `clamp` in `gfx.c`'s
`GFX_HERO` mode.

## Home — SIGNED-IN session (measured 2026-08-31, **divergent**)

With the owner's profile the home is **a different screen**, and the difference
is not "more data": it is profile preference. `localStorage.layoutPreferences`,
profile 1:

```json
{ "homeLayout": "modern", "continueWatchingCardStyle": "card",
  "heroSectionEnabled": true, "modernLandscapePostersEnabled": true,
  "modernHeroFullScreenBackdropEnabled": true, "posterLabelsEnabled": true }
```

It is `modernHeroFullScreenBackdropEnabled: true` that turns the hero from a band
into a full screen, and `continueWatchingCardStyle: "card"` that gives the
landscape cards.

| element | signed out (implemented) | signed in (owner) |
|---|---|---|
| rail | 144 wide | `.home-nav-list` with **width 0** — takes no flow |
| content column | x=248 | x=**104** |
| hero art | x=555, 1421×670 | **x=0, 1920×1062** (full-bleed) |
| hero text block | x=248, width 640 | x=104, width 640, y 40…500 |
| hero logo | 440×160 at (248,135) | 640×160 at (104,**65**) |
| meta line | y=327, font 21/500 | y=**257**, h 52, font 21/500 `rgb(179,179,179)` |
| secondary line | *does not exist* | y=**341**, h 38, font **18/600**, `rgba(255,255,255,.88)` — "2H LEFT • 6.3 • EN" |
| synopsis | y=411, 640, font 22 | y=411, 640×89, font 22/400 |
| row title | y=518, h 31, font 26/600, ls −0.52 | identical, at x=104 |
| row cards | y=564 | y=**563.6** (header top + 45.2) |
| poster | 212×322, radius 24, step 272 | **identical** |
| "Continue watching" card | — | **432×247**, radius 24, step **492** |
| step between rows | 416 | **415.2**; the "Continue watching" one uses **358.2** |

**RESOLVED IN THE SOURCE, it was not a question.**
`js/data/local/layoutPreferences.js` holds the factory defaults, and the owner's
profile differs only in these:

```
collapseSidebar: true                        <- the rail does not vanish, it is COLLAPSED
modernHeroFullScreenBackdropEnabled: true    <- swaps the band for full screen
continueWatchingCardStyle: "card"            <- the landscape cards
modernLandscapePostersEnabled: true
```

They are not two layouts: it is **one** screen driven by preferences. And the
inset rule becomes obvious seen this way — the content always has **104** of
inset, and the rail adds its own 144 when it is fixed (104 collapsed, 248 fixed).
The port now reads the preferences and exposes them in Settings.

## Focus — MEASURED, and it fixes a real defect

The web app **barely scales the focused item**, and **never lifts it**:

```
.home-screen-shell .home-poster-card.focused   { transform: none }
.home-screen-shell .home-content-card.focused  { transform: scale(1.01) }
.series-primary-btn.focused, .series-circle-btn.focused,
.series-secondary-btn.focused, .series-season-btn.focused,
.series-episode-card.focused, .series-insight-tab.focused { transform: none }
.movie-cast-card.focused, .series-insight-tab.focused     { transform: scale(1.03) }
```

Focus is marked by **border colour and box-shadow**, not by geometry:

| element | ring |
|---|---|
| home card | `box-shadow` 2px `#f5f5f5`, inset **and** outset, on `.home-continue-media` / `.home-poster-frame`; transition `border-color .14s, box-shadow .14s` |
| detail button | `box-shadow 0 0 0 4px #fff`; the focused circular one becomes `#f5f5f5` with a `#111` icon; the primary one **does not change colour** |

This explains the defect the owner reported (a focused card touching the row
title): the port used scale 1.09 plus an 8px lift, from **tvOS** Top Shelf
tables. A 322-tall poster growing 9% and rising 8 has its top at 541.5, and the
row title ends at 549 — 7.5px of overlap, by construction. Fixed: scale 0, lift
0, a 2px `#f5f5f5` ring (it had been blue `#339f5`, a colour that exists nowhere
in the interface).

## Motion — MEASURED in the stylesheet (this file had no such section)

No card flights. The transitions are opacity and small displacements:

| what | rule |
|---|---|
| home entry | `.home-route-content-enter` → `homeRouteEnter` **0.24s ease-out**, `opacity 0→1` + `translateY(2% → 0)` |
| search / library / settings / addons entry | `searchRouteEnter` **0.35s ease**, `opacity 0→1` only |
| screen change (`.screen`) | `transition: 0.14s` |
| **detail scrolled to the sections** | `.detail-scrolled .series-detail-backdrop { opacity: .15 }` with the vignette and base shadow at 0 — **0.8s cubic-bezier(.4, 0, .2, 1)** |
| detail content | `.series-detail-content { transition: opacity .4s cubic-bezier(.4,0,.2,1) }` |
| detail hero body | `max-height .6s`, `opacity .4s`, same curve |
| detail button/card focus | `transform/background/border/box-shadow .22s cubic-bezier(0.22, 1, 0.36, 1)` |
| home card focus | `transform .12s ease-out, border-color .12s`; frame `.14s` |
| episode | `.series-episode-card { transition: transform .18s }` |

This confirms the owner's description: **the background art stays** and it is the
components that change. On scroll the web does NOT blur the art — it **fades it
to 15%** over 800ms. The gaussian blur with darkening that the port was doing
belongs to the Apple TV app, costs two full-screen passes per frame and kills the
art's colour.

`anim_spring` reproduces this if the stiffness matches the measured time:
`exp(-k·t)`, 95% of the way in 800ms → **k ≈ 3.8** (`NV_SPRING_PAGE`). With the
screen-change stiffness (9.0) the art faded in ~330ms, less than half.

## Home's exit when the detail opens — **MEASURED, and the answer is "there is none"**

This used to hold a "NOT YET MEASURED" about the staggering of the exit. Measured
on 2026-09-01 with `MutationObserver` + `document.getAnimations()` +
`getTiming()`, sampled by `setTimeout` (the panel's rAF is throttled;
`setTimeout` is not, with the tab visible). Reproducible method: instrument,
click a `.home-poster-card[data-action=openDetail]`, sample at
0/20/40/…/1300ms.

**There is no exit animation.** What the instrumentation saw:

| t (ms) | what happened |
|---|---|
| 1 | `#home` gets `style="display:none"` **and** `#detail` gets `style="display:block"`, in the same mutation batch |
| 2…1300 | no animation and no transition on any node of home or detail |

The three open questions, answered:

1. **Together or staggered?** Neither — home is hidden by `display`, in a single
   frame. `display` is not animatable, so `.screen`'s `transition: 0.14s` never
   runs.
2. **Does the detail's content enter during the exit or after?** In the **same
   frame**. Both `display` swaps land in the same batch.
3. **`homeRouteEnter` 0.24s `translateY(2%)`?** **It does not run in this
   runtime.**

Why: the app loads with `<body class="performance-constrained legacy-webos
no-flex-gap no-aspect-ratio no-css-math no-backdrop-filter">`, and the sheet has

```css
/* components.css:18190 */
.performance-constrained * {
  animation-duration: 0.01ms !important;
  animation-iteration-count: 1 !important;
  transition-duration: 140ms !important;
  transition-delay: 0ms !important;
}
/* components.css:18245 */
.performance-constrained .home-route-content-enter,
.performance-constrained .search-route-enter,
.performance-constrained .library-route-enter,
.performance-constrained .settings-route-enter { animation: none !important }
```

Consequences that hold for the whole port, not just this transition:

- **There can be no staggering anywhere**: `transition-delay: 0ms !important`
  applies to `*`. Any cascade the port invents is an invention.
- `homeRouteEnter` (0.24s, `translateY(2%)`) and `searchRouteEnter` (0.35s) are
  in the sheet but **do not run**. The rows in the "Motion" table above that cite
  them describe the CSS, not the behaviour on this TV.
- Transitions without an `!important` of their own drop to **140ms**. Confirmed
  by measuring `getComputedStyle` on the open detail screen:
  `.series-primary-btn` computes `0.14s` (the sheet declares .22s) and
  `.detail-bottom-shadow` computes `0.14s`.
- The ones that survive (they have their own `!important`), measured on the same
  screen: `.series-detail-backdrop` **0.8s** `cubic-bezier(.4,0,.2,1)` — which
  confirms `NV_SPRING_PAGE` 3.8; `.detail-hero-body` **0.6s**;
  `.series-detail-content` **0.25s** (the table above said .4s — **correct it**).
- Home card focus in this runtime is **200ms
  `cubic-bezier(0.22, 0.61, 0.36, 1)`** (components.css:18258), not the .12s/.14s
  the sheet declares.

In other words: the owner's description ("the background art STAYS and the
components leave as if descending") matches what the sheet *intends*, not what
the TV *executes*. On the TV the art stays because the detail draws its own
backdrop, and the components do not descend: they vanish in the same frame.


## Player

| element | value |
|---|---|
| margins | x 64, y 48 |
| button | 96, gap 4; icon 48 |
| bar | height 6 (10 focused), radius 3 |
| track | `rgba(255,255,255,0.30)` (0.45 focused) |
| fill | `#f5f5f5` (`--secondary-color`); buffer at 0.35 |
| bar → top | margin-top 12 (from the meta) |
| button row | margin-top 16 |
| gradients | top 150 (0.7→0), bottom 200 (0→0.8) |
| title | 56, weight 700 |
| subtitle and time | 32, weight 400, `rgba(255,255,255,0.9)` |
| time label | **a single one**, `elapsed / total`, pushed right |

## Detail — ported hero (SIGNED-OUT session)

> **A note on method.** Everything below was measured **without signing in**
> ("Continue without an account"). Signed out, the web app hides part of the
> title screen: there is no episode list, no season tabs and no progress. The
> **hero geometry** measured here does not depend on that, but the signed-in
> series screen has extra sections that have **not been measured** — when they
> are, this file has to grow.

The screen is **full-bleed**: a 1920×1080 backdrop covering everything, a
vignette on top, no rail and **none of the rounded card** the port had. Measured
with the title "The Whisper Man" open.

| element | value |
|---|---|
| shell / backdrop / vignette | 1920×1080, x=0, y=0; background `#0d0d0d` |
| backdrop | `background-size: cover`, `position: 100% 0` |
| hero section | `padding: 0 96 32 72`, `justify-content: flex-end` |
| logo | 261×104 at (72, 445); fixed height 104, max-width 710 |
| action row | 1752×108 at (72, 589), padding 6, gap 24 |
| primary button | 298×96 at (78, 595), radius 64, font **25/600**, BLACK text on white, side padding 48, icon 36, gap 16 |
| circular buttons | 84×84 at x=439, 586, 734 (step 147), y=601, radius 999, `#222` |
| focus | a **ring**, `box-shadow 0 0 0 4px #fff`; `transform: none` — there is no scale. The focused circular one becomes `#f5f5f5` with a `#111` icon; the primary one **does not change colour** |
| "Director: …" | 1040×36 at (72, 727), font 25/400, `rgb(179,179,179)`, lh 36.25 |
| synopsis | 1040×117 at (72, 787), font **26**/400, white, lh 39, 3 lines |
| meta stack | 1752×120 at (72, 928), gap 16 |
| meta line 1 | y=928, box h=49, lh 35, font 25/400 `rgb(179,179,179)`: **genres on the left, YEAR pushed right** (ending at 1824), with a 1×14 dot at 24px of slack |
| meta line 2 | y=1003, box h=45, lh 31, font **23**/400 **WHITE**: runtime • country |

**Corrections to what this file said before:** the meta line is neither a single
line nor all 25/`rgb(179,179,179)` — there are **two**, and the second is 23px
and **white**. And the year sits on the **first** line, on the right, not the
last.

### Vignette

A `linear-gradient(90deg, …)` from `#0d0d0d` to transparent, with nine stops —
0%:1.00 · 7.8%:0.95 · 17.16%:0.84 · 28.08%:0.70 · 40.56%:0.52 · 51.48%:0.34 ·
60.84%:0.18 · 70.2%:0.07 · 78%:0. **Piecewise linear** ramps, like the home
hero's. Implemented in `gfx.c`'s `GFX_DETAIL` mode, which already draws the art
and the vignette **in a single pass** — two full-screen layers are expensive on
this GPU.

There are no pseudo-elements: `.detail-bottom-shadow` exists in the DOM but with
`opacity: 0`.

### What came out different, and why

- **Two circular buttons instead of three.** The web's third opens the trailer on
  YouTube, and this app has no trailer player. The x positions of the first two
  are the measured ones.
- **The runtime line does not carry the country.** The catalogue's `CatItem` has
  no such field.
- **Weight 600 becomes Medium.** The embedded Inter only has Regular, Medium and
  Bold.


## Detail — SIGNED-IN session (measured 2026-08-31, series "Silo" in progress)

The hero geometry holds, but **the content changes and shifts the stack**. And
there are two corrections to what this file said:

**1. The buttons are IN FLOW, with 63px between neighbours.** The positions
x=439/586/734 are not constants: they are the result of the arithmetic with the
"Play" label. Checked on two screens — Whisper Man (signed out) and Silo (signed
in) — the gap between neighbouring buttons is 63 on both.

**2. The gap between the primary button's icon and its label is 34, not the 16
the flex `gap` declares.** The icon starts at 126, the label at 196, the icon is
36 wide. Primary width = `48 + 36 + 34 + textW + 48` — which gives 298 for "Play"
(text 132) and 334 for "Resume S2E3" (text 168). It checks out.

What the signed-in session adds:

| element | value |
|---|---|
| primary label | "**Resume S2E3**" instead of "Play" when there is progress |
| secondary button | `.series-secondary-btn` **345×96** — "Play from the beginning", `#222`, white text, radius 64, weight 600 |
| resume line | `.detail-resume-indicator` 1720×**37** at (72,633), font 22.66/400 `rgba(255,255,255,.82)`: "Resume available · 45% · Episode S2E3 · 30m left" |
| logo | rises to (72,**359**) — the stack got taller |
| actions | (72,**503**) |
| "Writer: …" | (72,**688**) — on a series it is the writer, on a film the director |
| synopsis | (72,748), 1040×117, font 26/400 — **unchanged** |
| meta stack | (72,**889**), height **159** (was 120) |
| meta line 1 | y=889, box h=**74** (it grows because of the IMDb badge), text at y=901; year "2023-" on the right; **IMDb badge 109×60**, radius 999, logo 60×60 + score font 20.7 |
| meta line 2 | y=**989**, box h=**59**; **`.detail-meta-badge.strong` badge** "RETURNING SERIES" 249×45 radius 8; then runtime and country |
| content width | `.series-detail-content` = **1888** (not 1920): the usable right edge becomes 1792 |

### Sections below the fold (series) — **NOT PORTED**

Level 1 of the native port is still the Apple TV app's page (season pills 236×63,
episode 212 with text below, sections "Trailers", "Cast and crew", "You may also
like", "How to watch", "About"). **None of that exists on the web.** What does
exist, measured:

| element | value |
|---|---|
| `.series-season-row` | y=1080, height 114; buttons **269×80** at x=96, 417, 738 (step **321**), radius 40, font 32/500; the chosen one `#2d2d2d` with white text, the others `#222` with `rgb(179,179,179)` text |
| `.series-episode-track` | y=1194; cards **640×422** at x=96, step **726**, radius 32 |
| episode thumbnail | 640×**414** — and **the text sits INSIDE it**, overlaid at the base, not below. That is the structural difference from the port. |
| episode badge | 163×44, font 20/600, background `rgba(0,0,0,.42)`, radius 12 |
| episode title | font **32/800**, lh 44 |
| episode synopsis | font **28**/400 `rgba(255,255,255,.9)`, lh 36, 2–3 lines |
| episode meta | font 20/400 `rgb(179,179,179)`: clock icon + runtime + the date written out |
| episode progress | bar 576×**8**, radius 999; track `rgba(0,0,0,.45)`, fill `rgb(158,158,158)` |
| `.series-insight-tabs` | y=1680; "Creator and cast \| Ratings \| Trailer", font 32/500; the chosen one white, the others `#808080`; the "\|" divider font 32/700 `#808080` |
| cast | avatar **140×140** radius 999, name font **26/500** `rgb(179,179,179)`, card 220 wide, step **270**, from x=96 |


## Home catalogues — where the list comes from (MEASURED, **not ported**)

The port declares four rows in a static array in `src/home.c`
(`"Continue Watching"`, `"Popular - Movie"`, `"Popular - Series"`, `"Trending"`).
On the web none of that is fixed. The source of truth is
`localStorage.homeCatalogPrefs`, scoped per profile:

```json
{ "__profileScoped": true, "version": 1,
  "profiles": { "1": { "order": [...], "disabled": [...], "customTitles": {...} } } }
```

- **`order`** — the row order, by key. Two key shapes:
  `<addonId>_<type>_<catalogId>` (e.g.
  `app.xperience.<uuid>_movie_recs_movies_for_you`) and `collection_<uuid>` for
  the user's own collections. The owner's profile has **more than 30**.
- **`disabled`** — the ones the profile turned off (empty today). This answers
  "what happens to one the profile disabled": it leaves the home.
- **`customTitles`** — per-row renaming; with no entry, the title comes from the
  addon's manifest.
- **`installedAddonEnabledStates`** — 3 installed addons; a disabled addon takes
  its catalogues with it.

"Continue watching" is **not** in `order`: it is a synthetic row, always first
when there are items, assembled from `continueWatchingItems` /
`watchProgressItems`.

On the native side `addons.c` (which reads `addons.txt`, with a column saying
whether the addon supplies a catalogue) and `discover.c` (which assembles the
library over the network) already exist. What is missing is for the home to stop
hard-coding the list and start asking.

**OPEN QUESTION FOR THE OWNER:** should the native app read `homeCatalogPrefs`
from the web app (same device, another process — and the web app keeps the file
open, as already happened with progress), or receive the list over the network
alongside the library? I did not invent an order.


## Layout preferences — the source, and what the port does with them

`js/data/local/layoutPreferences.js`, `DEFAULTS` (factory defaults):

| key | default | owner's profile | port |
|---|---|---|---|
| `homeLayout` | `"modern"` | `"modern"` | only the modern one exists; not exposed |
| `collapseSidebar` | `false` | **`true`** | ✅ "Sidebar" |
| `heroSectionEnabled` | `true` | `true` | ✅ "Show hero" |
| `modernHeroFullScreenBackdropEnabled` | `false` | **`true`** | ✅ "Full-screen backdrop" |
| `continueWatchingCardStyle` | `"card"` | `"card"` | ✅ "Continue watching style" (card/wide/poster) |
| `posterLabelsEnabled` | `true` | `true` | ✅ "Poster labels" |
| `modernLandscapePostersEnabled` | `false` | **`true`** | ✅ exposed; the landscape drawing is not |
| `posterCardWidthDp` / `posterCardCornerRadiusDp` | 126 / 12 | 120 / 12 | see below |
| `cardDepth*`, `focusedPosterBackdropExpand*` | — | on | **not ported** |

### The poster size does NOT come from `posterCardWidthDp`

Worth correcting a reasonable but wrong assumption. `buildModernHomeSizingStyle`
(homeScreen.js:521) computes, with `dpToPx = 2`:

```
portraitWidth  = round(dp * 0.84 * 1.08 * 2)   // 120 -> 218
portraitHeight = round(dp * 1.5 * 0.84 * 1.08 * 2)  // 120 -> 327
radius         = round(radiusDp * 2)           // 12  -> 24
```

And indeed `--home-poster-width: 218px` / `--home-poster-height: 327px`. **But
the measured card is 212×322.** The reason is in the modern layout's CSS, which
ignores the variable:

```css
.home-screen-shell.home-layout-modern .home-poster-card:not(.is-landscape)
  { min-width: 212px; max-width: 212px; flex-basis: 212px }
.home-screen-shell.home-layout-modern .home-poster-card:not(.is-landscape)
  .home-poster-frame { height: 318px }
```

212 wide, a 318 frame plus 2px of border top and bottom = **322**. The
`--home-poster-*` variables belong to the **classic** layout. In other words:
212×322 is a constant of the modern layout and does **not** change with
`posterCardWidthDp` — only the radius (24) comes from the preference. `layout.h`
may keep the numbers, as long as it says so.

**CONFIRMED BY EXPERIMENT (2026-09-01), no longer by reading.** With home open, I
changed the inline variable on the shell:

```
--home-modern-portrait-poster-width: 218px -> 300px   the card stayed 212
--home-modern-portrait-poster-height: 327px -> 400px  the frame stayed 318
```

So `posterCardWidthDp` **sizes nothing** in the modern layout. All that comes out
of the preference is the radius.

### Three more traps, found in the source

**1. `posterLabelsEnabled` has no effect in the modern layout — by the
stylesheet's decision.**

```css
/* components.css:7334 */
.home-screen-shell.home-layout-modern .home-poster-copy { display: none }
```

And that is why the web's Settings screen **hides the option** when the layout is
modern: `settingsScreen.js:4050` wraps the row in `!isModernLayout`. The port
draws the label when the preference is on (that was the owner's explicit
request); to be pixel-for-pixel with the web today, just turn it off.

**2. `modernLandscapePostersEnabled` is BROKEN on the web, and the break is a
wrong key.** The owner's profile has the preference at `true`, the shell receives
`.home-modern-landscape-posters` — and even so **every catalogue poster measures
212×322, portrait**. The only `.is-landscape` elements on screen are collection
cards (`is-collection-landscape`), whose shape comes from the collection's
`tileShape`.

The cause is in the reconciler:

```js
// homeScreen.js:11922, reconcileHomeCatalogRows()
showPosterLabels: this.layoutPrefs?.showPosterLabels !== false,
showCatalogTypeSuffix: this.layoutPrefs?.showCatalogTypeSuffix !== false,
preferLandscapePosters: Boolean(this.layoutPrefs?.preferLandscapePosters),
```

`layoutPrefs` **does not have** `preferLandscapePosters` (the key is
`modernLandscapePostersEnabled`), nor `showPosterLabels`/`showCatalogTypeSuffix`
(they are `posterLabelsEnabled`/`catalogTypeSuffixEnabled`). The full render in
`renderModernHomeLayout` passes the right keys, but the reconciler runs on every
row that arrives from the network and rewrites everything with `false`. The
landscape card appears for an instant on the first render and dies on the first
reconcile.

The port implements the intended effect (the owner asked to "implement the effect
of both"). **If the goal is to match today's screen, the fix belongs on the web
side**: swap the three keys at `homeScreen.js:11920-11922`.

**3. `focusedPosterBackdropExpandEnabled` is disabled in the code, on purpose.**
`homeScreen.js:6812`:

```js
const HOME_POSTER_EXPAND_DISABLED = true;
const shouldExpand = HOME_POSTER_EXPAND_DISABLED ? false : ...;
```

with a comment from the owner explaining the decision ("the hero already shows
the focused item's art, title and synopsis"). So the focused poster does **NOT
grow to 563.92 after 3s** in the app he uses. The port keeps the preference and
the delay, but does not draw the growth — drawing it would diverge from the real
screen.

Other widths from the same sheet, not yet ported:

- `.home-poster-card.is-landscape` — **318** wide, frame 178.875 (16:9). That is
  `modernLandscapePostersEnabled`.
- `.home-poster-card.is-expanded` — **563.92** wide, frame 318. That is
  `focusedPosterBackdropExpandEnabled`: the focused poster **grows on its own
  after 3s** (`focusedPosterBackdropExpandDelaySeconds`) and shows the backdrop
  with a gradient. It is visible behaviour and the port does not have it.

### Full-screen hero — the ramps

MEASURED on the pseudo-elements of `.home-modern-hero-media` with the preference
on (1920×1062 at 0,0, image `cover` with `object-position: 100% 0`):

- `::before` — horizontal, covering the **leftmost 1248px of 1920** (65%):
  `#0d0d0d` → 0.90 at 22% → 0.80 at 46% → 0.42 at 76% → 0
- `::after` — vertical, full height:
  0 until **64%** → 0.35 at 74.8% → 0.75 at 85.6% → solid at the end

The percentage stops are **the same** as the band hero's; what changes is the
coverage (65% of the width against 45%) and the depth. That makes sense: with the
art filling the screen, the text needs more dark ground beneath it. Implemented
in `GFX_HERO_FULL`, alongside `GFX_HERO`.


## Search — MEASURED (SIGNED-IN session, 2026-09-01). It had never been compared.

Background `#0d0d0d`. Rail collapsed (x=-48), content at 104, like the home.

| element | value |
|---|---|
| `.search-header` | y=22, h=110, padding `0 104` |
| `.search-discover-btn` | 110×110 at (104,22), `#222`, 1px `#333` border, radius 22, icon 54 |
| `.search-voice-btn` | 110×110 at (262,22) — step **158** (gap 48) |
| `.search-input-field` | 1396×110 at (420,22) → right edge 1816; `#222`, radius 22, font **34/500**, padding `0 32`; placeholder "Search films and series" |
| focused field | `#f5f5f5` border + `box-shadow 0 0 0 2px rgba(245,245,245,.22)` |
| `.search-empty-state` | y=148, h=400, centred; icon 136×136 at y=220.5 |
| empty-state title | 56/600 white, lh 58.24, y=378.5 |
| empty-state support text | 24/400 `rgb(179,179,179)`, lh 28.8, y=446.7 |

The results are **not a grid**: they are horizontal rows, one per catalogue.

| element | value |
|---|---|
| `.search-results-title` | 48/600 white, lh 51.84, padding `0 104` |
| `.search-results-subtitle` | 20/400 `rgb(179,179,179)`, margin-top 4 → +55.8 from the title |
| `.search-results-track` | +88.3 from the title; cards +4 from the track (→ +92.3) |
| `.search-result-card` | 248 wide; x = 104, 384, 664 → step **280** |
| `.search-result-poster-wrap` | 248×**372**, radius 22, `#222`, 2px border |
| `.search-result-name` | 28/500 white, lh 33.6, margin-top 8 |
| `.search-result-date` | 20/400 `rgb(179,179,179)`, margin-top 4 |
| step between rows | **562.4** (546.4 tall when there is a date; 523.9 without) |
| `.search-seeall-card` | the same 248×440.1 box, at the end of the track |

**The web has no on-screen keyboard**: the `<input>` is served by the TV's system
IME. The port is pure SDL and has no IME — the grid keyboard stays, and it is the
only deliberate divergence on this screen.


## Library — MEASURED (SIGNED-IN session, 2026-09-01). It had never been compared.

`.library-main` has `padding: 48px 96px 64px` → content at x=**96**, y=48, width
**1728**. (Note it is 96, not the 104 of home and search.)

| element | value |
|---|---|
| `.library-page-title` | "Library" 56/600, letter-spacing **1px**, at (96,48), h=56 |
| `.library-page-source` | "NUVIO" badge 28/500 `rgb(128,128,128)`, ls **4px**, padding-top 10, right-aligned (ending at 1824) |
| `.library-view-mode-row` | y=136, h=56, gap 16 |
| `.library-view-mode-button` | 150×56, radius 999, padding `14 24`, font 21/400; x = 96, 278 → step **182** |
| — the chosen one | `#303030`, 2px `#fff` border |
| — the others | `#222`, 2px `#333` border |
| `.library-picker-row` | y=212, h=110 |
| `.library-picker-anchor` | **840×110** at x=96 and x=**984** (step 888), radius 36, padding `18 28` |
| — focused | `#303030`, 1px `#fff` border |
| — unfocused | `#222`, 1px `rgba(255,255,255,.1)` border |
| `.library-picker-title` | 19/500 `rgb(128,128,128)`, ls 0.45, lh 24 |
| `.library-picker-value` | 30/500 white, ls 0.3, lh 40, margin-top 4 |
| `.library-picker-icon` | 40×40 (svg 32) pinned right |
| `.library-empty-state` | y=354, padding-top 38, gap 18 |
| — title | 46/500 white, lh 49.68 |
| — support text | 28/400 `rgb(179,179,179)`, lh 35 |

The grid (`.library-grid`), read from the sheet and checked against the usable
width:

```css
grid-template-columns: repeat(auto-fill, minmax(var(--library-poster-width, 252px), 1fr));
gap: 32px 24px;
```

1728 usable with a 252 minimum and a 24 gutter → **6 columns of 268**. A 2:3
poster → 268×**402**, radius 24, with `border: 4px solid transparent` (the focus
border is **on the inside**). Title 32/500, lh 1.18 (37.8), **16** from the
poster. Card height 455.8; row step **487.8**.

Focus: `transform: scale(1.02)` with `transform-origin: center top`, the border
goes to `--focus-color`, and `box-shadow: none` — the sheet comments: *"Android
TV uses the inside focus border, not an outer halo"*. It is the **only** focus
scale left on any web screen; the others (9%, 14%) came from tvOS Top Shelf
tables and not from this interface.

The port's three tabs ("My List", "Purchased", "Genres") do not exist on the web.
What exists are **two** dimensions: the mode (Saved/Cloud) and the two pickers
(Type, Sort).
