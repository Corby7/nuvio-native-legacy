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
| rows viewport | y=518.4, h=561.6 (52% of the height), `overflow-y: auto` |
| row title | y=**564.4**, h=33.6, font 26, weight 600, ls −0.52 |
| row 0 cards | y=**614** |
| card | 212×322, radius 24 |
| gap between cards | 60 (step 272) |
| step between rows | 416 |
| poster title | 16, weight 500 |
| poster subtitle | 13, weight 400, `rgba(255,255,255,0.7)` |

### The 46px the rows rest below the viewport — RE-MEASURED 2026-09-07

This table used to say "row title y=518, row 0 cards y=564", and both were one
step out: **518.4 is the top of `.home-modern-rows-viewport`**, not of the title,
and the 564 quoted for the cards is where the **title** actually sits.

`.home-modern-rows-scroll`, the flex column inside that viewport, opens with
`padding-top: var(--modern-rows-top-feather)` = **46px** (components.css:7346),
so at `scrollTop: 0` the first row's header lands at 564.4 and its cards at 614.
It is not a first-row quirk: with the focus two rows down (`scrollTop: 782`) the
focused row's header still measures 564.6. It is where a row comes to **rest**.

The padding exists because the viewport carries a mask fading its own top 46px
(components.css:7295) — "opaque by 46px, which is where rows come to rest", in
the sheet's own words.

Consequence at the other end of the screen, which is how it was caught: with the
shelf 46px too high the port showed **65px** of the second row's cards where the web
shows **20**. `NV_SHELF_PAD_TOP` in `layout.h` now carries it.

### The hero's text block — RE-MEASURED 2026-09-07

Measured on the elements of `.home-modern-hero-copy`, with a poster focused (no
"N MINUTES LEFT" line) at 1920×1080:

| element | y | h | notes |
|---|---|---|---|
| copy box | 48 | 422.4 | `flex-end`, `gap: 12px`, bottom **470.4** |
| `.home-hero-brand` | 75.7 | 200 | logo 440 wide |
| meta line | 287.7 | 27.5 | font **22**/500 |
| secondary | — | 27 | font **20**/600, `display:none` when empty |
| description | 331.2 | 139.2 | font 24/400, leading 34.8, width **760**, `margin-top: 4` |
| first row head | 565 | 33.6 | |

Four numbers this file and `layout.h` had wrong, all of them pushing the block
**up**:

1. **The base is 470.4, not 478.4.** It has nothing to do with
   `--modern-hero-copy-bottom-gap` (40). The copy sits in `.home-hero-card`, a box
   of 518.4 (48% of the height) with `padding: 48px 64px`; the base is 518.4 − 48.
2. **The column's gap is 12, not 16** — and the description adds `margin-top: 4px`
   of its own, so the one 16 in the block is the space above the synopsis.
3. **The synopsis is 760 wide, not 640.** The 640 was read off the `.legacy-webos`
   block, which in fact narrows it to 560 and needs webOS ≤ 6 — the C3 is webOS 23.
4. **The line count is not a constant.** The CSS clamp is 4;
   `applyModernHeroDescriptionBounds` (homeScreen.js:6531) then lowers it to however
   many WHOLE lines are left in the fixed 422.4 box once the logo, the meta line and
   the secondary have taken their share — `floor(available / lineHeight)`, capped at
   4. With the secondary line present that is 3; without it, 4.

The port had been passing a fixed 4 at 640 wide with 16px gaps, which on a
continue-watching hero lifted the logo to y=40 against the web's 71.5. It now ports
the rule and lands at 75/76 against the web's 75.7 in both shapes.

#### …and then deliberately diverges by 46

Those two anchors are INDEPENDENT in the web: the copy hangs 48 under the hero
card's edge (base 470.4) and the rows rest 46 under the viewport's (title 564.4),
which leaves **94px of nothing** between the synopsis and the row title. Ported
literally — and it was, and it was measured band by band on the running app to
confirm it — the block reads as floating away from the row beneath it. The owner's
verdict with the two side by side: *"it still looks way too high, should be
connecting more to the element under it."*

So the copy now hangs off the FIRST ROW'S TITLE rather than off the viewport:
`base = NV_SHELF_TOP + NV_SHELF_PAD_TOP - NV_HERO_COPY_GAP`. The 48 is still the
web's number; what changed is what it is measured from. Everything in the hero
therefore sits 46px lower than the web, and the whole composition moves as one.

This is the only deliberate divergence in the home's vertical layout. Do not
"correct" it back to the web's 94 without asking — it has been rejected twice.


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

### The hero's meta line — RE-MEASURED 2026-09-15, and the fix is the SEPARATORS

The line read as clutter ("TV Show • Reality • Romance • 2005 • 31min • IMDb 4.2") and
the cause was not the number of words. Measured on `.home-modern-hero-meta-line`:

| element | value |
|---|---|
| tokens | 22/500, **`rgba(255,255,255,.62)`** |
| the "•" | 22/500, **`rgba(255,255,255,.34)`** — a little over half the tokens |
| dot spacing | 12 each side (a dot at 282..293.2, the next token at 305.2) |
| groups | TWO, `gap: 14` — what the title IS, then its NUMBERS |
| the score | `rgb(179,179,179)`, BRIGHTER than the tokens around it |
| genres | **one**: "Movie • Action", never a list |

The port baked the bullets into ONE string drawn at a flat `rgb(179,179,179)`, so every
dot was exactly as loud as the words it separated and the eye had nothing to group on.
A single `TxtLine` can only carry one colour, so the line is now a list of tokens drawn
one at a time — and `bulletize`, which existed to build that joined string, has gone.

Two traps in porting it:

- **The group boundary keeps its dot.** The measured lead group ends at 370.7 and the
  trailing group opens at 384.7 — a gap of 14 — with its own dot sitting there and the
  usual 12 after it. Dropping the dot at the boundary, which the first attempt did,
  reads as a missing separator rather than as a group.
- **A token that does not fit is dropped WHOLE.** `txt_line_trim` on the joined string
  put an ellipsis in the middle of a genre.

**One deliberate divergence, at the owner's request.** The streaming service used to be
a LOGO drawn ahead of the line by `badges_draw`; it is now the line's first token, as
text — *"dont need logos to indicate what streaming service it is on, just text is
enough"*. NuvioWeb shows no provider on the hero at all, so this is not a measurement.
It appears wherever `providerName` is populated, which is the same condition the badge
had: `discover.c` fills it from TMDB `/watch/providers` (region BR, `flatrate` only) as
each title is fetched, so it is absent on a title whose providers have not been asked
for yet.


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
| row title | y=**564.4**, h 33.6, font 26/600, ls −0.52 | identical, at x=104 |
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

The web app **never lifts the focused item**. How much it GROWS it was wrong here
until 2026-09-07 — see the correction below.

```
.home-screen-shell .home-poster-card.focused   { transform: none }
.home-screen-shell .home-content-card.focused  { transform: scale(1.01) }
.series-primary-btn.focused, .series-circle-btn.focused,
.series-secondary-btn.focused, .series-season-btn.focused,
.series-episode-card.focused, .series-insight-tab.focused { transform: none }
.movie-cast-card.focused, .series-insight-tab.focused     { transform: scale(1.03) }
```

### The home card grows 1.05, not 1.02 and not 0 — RE-MEASURED 2026-09-07

Four rules compete for a focused poster card, and reading only the first three
gives the wrong answer twice over:

```
.home-screen-shell .home-content-card.focused                    -> 1.01  (0,3,0)
.home-screen-shell .home-poster-card.focused                     -> 1.05  (0,3,0)
.home-screen-shell.home-layout-modern .home-content-card.focused -> 1.02  (0,4,0)
[class*="-card"].focusable:not([class*="player-"]).focused       -> 1.05  (0,4,0)
```

The last one (components.css:21168) wins: an attribute selector weighs the same
as a class and `:not()` takes its argument's weight, so it **ties** the modern
rule at (0,4,0) and comes ~13 000 lines later in the same sheet. Source order
breaks the tie.

MEASURED rather than deduced, with focus on a poster in the running app:
`getComputedStyle().transform` is `matrix(1.05, 0, 0, 1.05, 0, 0)`, origin
`114.5px 0px` (top centre), and the border box is **240.4 × 364.3** against the
**229 × 347** of the card beside it. The continue-watching card reaches the same
1.05 by its own route (`.home-continue-card.focused`, with `!important`).

The `transform: none` in the block above was read off an older build of the web
app; this one scales. `NV_FOCUS_SCALE_POSTER` went 0.02 → 0.05.

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

### The actions row — RE-MEASURED 2026-09-15, and it replaces the device set

Measured on NuvioWeb **0.3.8** at 1920×1080 (`npm run serve`), film "The Whisper
Man", on `.series-detail-actions` and each of its buttons, **at rest and focused**.

This row had been ported from the native TCL app instead (adb screencap,
2026-09-01) and the two references disagree on nearly every line of it. The web
wins here, by the owner's instruction that the title screen looks like NuvioWeb.
`NV_DETWEB_*` in `detail.h` carries these; the `NV_DETW2_BTN_*`/`CIRC`/`FOCUS_*`
set is gone rather than kept beside them.

| element | web (now ported) | the device set it replaced |
|---|---|---|
| row | x=208, y=562.61, h=96, flex `gap: 24` | — |
| primary | 193.3×96 for "Play", radius 64, `padding: 0 36` | 321×94, padding 54 |
| primary font | **32/600** | 25/600 |
| primary icon | 36×36 at x=244, then **24** to the label | 28×30, gap 21 |
| circles | 96×96, radius 999, step 120 | 96, step 120 |
| circle glyph | **44×44** (0.458 of the circle) | 32 (0.333) |
| focus | `box-shadow 0 0 0 4px #fff`, **`transform: none`** | a scale of 1.114×1.147 |

**The correction that most changes the screen: the primary is NOT a white pill at
rest.** `.series-primary-btn` measures `rgb(34,34,34)` with white ink unfocused
and `rgb(245,245,245)` with `rgb(17,17,17)` focused — the same pair the circles
use. Ported white-in-both-states, the row read as one lit button beside three dark
ones with nothing saying which had the focus, which is exactly why the old port
needed a scale to show focus at all.

**Checked on the device after porting** (C3, capture of the series "Lanterns"):
pill at x=96..366, circles at 391..486 and 511..606 — gaps of 24 to the pixel —
the focused circle 96 tall inside a ring at 728..831, and 60.0 fps / 0 janks.

### The tooltips over the circular buttons

`.series-circle-btn::after` with `content: attr(aria-label)`
(components.css:17732). It exists because the circles carry no label of their own
and a native `title` only ever fires for a mouse — the sheet shows the aria-label
on `.focused` so a d-pad gets one too.

| property | value |
|---|---|
| text | 24/**700**, `rgba(255,255,255,.92)` |
| shadow | `text-shadow: 0 2px 8px rgba(0,0,0,.8)` — and **no background pill** |
| box | `padding: 7px 16px`, `line-height: 24` → 38 tall |
| position | box base at `calc(100% + 16px)`; centred on the button |
| motion | `opacity` 0→1 over 140ms while `translateY(4px)` → 0 |

The labels are NuvioWeb's own aria-labels: "Add to Library" / "Remove from
Library", or the **Watchlist** wording when the library is the Trakt one — which
on this port is the only thing it is — and "Mark Watched" / "Mark Unwatched". The
primary button has none, because its label is already inside the pill. The
`Sources` button has no counterpart on the web (there the stream chooser opens
from play) and its label is this port's own.

**What the port does differently, and why:** there is no blur pass for glyphs, so
the 8px shadow is three offset black copies of the line. It is not decoration —
the tooltip sits straight on the backdrop, and on a bright frame white-on-white
would swallow it.

**It cost 58px of the logo's gap.** The tooltip's box reaches 54 above the button,
and `NV_DETW_LOGO_GAP` was 40 — the CSS margin. The *rendered* gap in the web is
**97.59** (logo ends 465.02, row starts 562.61); the difference is
`.detail-trailer-hint`, a paragraph always in the flow that only carries text
while a trailer plays. That band is where the tooltip lives, so the port now
carries 98 and the logo sits where the web's does.


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

### The play glyph — why the button still read as a different app

The pill's geometry matched and the button still looked wrong, and the reason was the
**glyph**: `ic_detail_play.svg` is a triangle with all **three corners rounded** (its
path is three cubic curves), and the port drew `GFX_PLAY`, a hard-edged triangle from
the shader. At 36px on a 96 pill that is the whole character of the control.

It is now the web's own file, rasterised at 128 with `rsvg-convert` into
`deploy/app/art/icons/detail_play.png` and drawn through `gfx_icon`, so the colour
still inverts with the label. The `play.png` already in the package is the same shape
but was rasterised tighter — its ink fills 0.84 of the box against the SVG's 0.90 — so
it lands ~2px short and stays where it is, in the player.

### The season selector is a DROPDOWN — measured 2026-09-15

`.series-season-row` holds ONE `.library-picker` (the same control the library screen
uses) with `aria-haspopup="listbox"`. The row of one chip per season was the Apple TV
app's; besides looking wrong it ran off the right of the screen on a series with eight
seasons, with no way to reach the far end.

| element | value |
|---|---|
| anchor | x=208, **359.8×80**, radius 64, `#222`, 1px `rgba(255,255,255,.1)`, padding 0 36 |
| label | "Season 1" **30/600** white, then " · 8 Eps" **30/400** `rgb(179,179,179)` |
| chevron | 32×32 `rgb(179,179,179)`, 24 after the label |
| focused | background `rgb(48,48,48)` and an **INSET** ring: `inset 0 0 0 3px rgba(255,255,255,.96)` — not the outer ring the hero uses |
| menu | 8 below the anchor, same width, `#222`, radius 64, 1px `rgba(255,255,255,.08)`, `0 8px 32px rgba(0,0,0,.6)`, `max-height: 540` |
| option | **84** tall, radius 64, **28/500**, padding 20 32; focused `#f5f5f5` on `#111`, the rest transparent on white |

### The episode card — RE-MEASURED 2026-09-15, and the CONTENT changed

The port's card came from a device capture of a different build and carried a
three-line synopsis and a clock + duration **inside** the thumbnail. The web has
neither. Its card is a still with two things at the top and two at the bottom, and the
synopsis lives **under the row**, one episode at a time, at reading size.

| element | web (now ported) | what it replaced |
|---|---|---|
| card | 600 wide, thumbnail 600×**395** radius 24, step **648** | 640×414, step 672, radius 32 |
| copy | padding 24 32 | pad 32 |
| badge | "EPISODE 1", padding 10 20, radius **64 (a pill)**, 20/600, **letter-spacing 2**, `rgba(0,0,0,.42)` | 38 tall, radius 12, no tracking |
| status | a **50×50** circle top-RIGHT, **2px dashed** `rgba(179,179,179,.9)` | nothing until Trakt answered, then a filled disc |
| meta | 20/400 `rgb(179,179,179)`, gap 24: rating badge then the date, both left | clock + duration left, date pushed right |
| title | **32/800**, min-height 56 | 32, no min |
| synopsis | **not in the card** — `.series-episode-desc-row` under the row, 32/400, leading 44, 1179 wide | 3 clipped lines over the still |
| progress | 8 tall **below** the thumbnail, in the card's own 8px | inside the thumbnail |
| gradient | `rgba(0,0,0,0) 52%, .77 72%, .95 100%` — the top half is CLEAR | started veiling at .06 from the very top |
| focus | **`scale(1.05)`** + a 4px white ring on the thumbnail + `0 8px 30px rgba(0,0,0,.5)` | a ring, no scale |

### The episode's rating, and the season's episode count — 2026-09-15

**The rating does NOT come from Cinemeta, and assuming it did was wrong.**

`videos[].rating` exists and reads "0" for a great many series — every Fallout, Game of
Thrones and Stranger Things episode does, while Breaking Bad and Friends carry real
numbers. That is a trap: a port that reads it looks like it works and quietly shows
nothing, and it led to the claim that "Fallout has no episode ratings", which the owner
corrected. NuvioWeb shows 8.2 on Fallout S1E1.

The chain is `fetchSeriesRatingsBySeason` (metaDetailsScreen.js):

1. resolve the IMDb id to a TMDB id;
2. **`<IMDB_RATINGS_API_BASE_URL>/api/shows/<tmdbId>/season-ratings`** — tried FIRST;
3. TMDB `vote_average` per season, only if that returns nothing;
4. the addon's own `rating`, last, via `mergeSeasonRatings`.

The API returns a BARE ARRAY of seasons, each with an `episodes` array carrying
`episode_number` and `imdb_rating` (`vote_average` repeats it). Checked against Fallout
(tmdb 106379): S1E1 "The End" = 8.2 over 23197 votes.

`discover.c` now calls it and `cat_set_ep_score` fills each episode in place, so the
numbers land into a row already on screen instead of republishing the list and
restarting every thumbnail. The base URL rides the SAME `local.properties` key the web
reads, through `tools/env.sh` as `-DNV_IMDB_RATINGS`.

**It has to run AFTER `photosOfCast`.** That function is what resolves the TMDB id
(`d->tmdb`), and the ratings API is keyed by it. Called before — which is where it went
first — the id is still 0 and the fetch returns without a word.

**Count what LANDED, not what you parsed.** The first log line counted API entries and
read "16 filled" while nothing appeared on screen; `cat_set_ep_score` returns whether an
episode actually matched, and the line now reads "5 of 5 matched an episode on screen".

**The season's episode count was the TOTAL across seasons.** The picker read
"Season 1 · 16 Eps" on Fallout, whose seasons are 8 and 8, and season 2 got no count at
all. Cinemeta's `videos` comes whole — every season in one flat list — and the count
fell back to `cat_n_episodes`, which is that whole list. (Fallout's raw `videos` is 19:
3 specials in season 0, which the parser drops, then 8 and 8.)

**The row was showing every season's episodes too.** `.series-episode-track` on the web
holds the chosen season and nothing else; this port drew all 16 with season 2's appended
after season 1's, and choosing a season only moved the FOCUS to the first card of that
season. That is the Apple TV app's single-track model. `nEpsOfSeason` / `epOfSeason` now
map a season-relative index onto the flat list and every caller goes through them.

### Two things the port had to solve that the web does not

**The gradient banded, and more bands was the wrong answer.** `veilEpisode` built the
overlay as stacked rounded rectangles, leaning on N layers of alpha d compositing to
1-(1-d)^n. That held for the old ramp, which climbed 0.06 → 0.95 across the whole 395px
thumbnail with bands 28px apart. The measured gradient is FLAT until 52% and does all
its work in the last 190px, and there the same trick drew visible horizontal stripes
across the still — worst on a bright frame. Spreading the bands over the ramp instead
of the card narrowed the stripes without removing them; raising the count only trades
stripes for draw calls.

The fix is **GFX_EP_SCRIM**, a per-pixel mode modelled on the `GFX_CW_SCRIM` the
Continue Watching card already had — which is the comparison that made the seams
obvious in the first place. Two `clamp()`s, one per segment, summing to 0.95 at the
base, clipped by the thumbnail's own corner. One quad per card instead of twenty, and
60.0 fps / 0 janks.

**The focused picker's ring came out with bites off the top and bottom.** `GFX_RING`
strokes `abs(d) < thickness` around the SDF's zero, and that zero IS the quad's edge —
so half the stroke always falls outside the quad and is clipped. On a pill the outline
touches the quad at twelve and six o'clock, which is exactly where the clipping lands.
Insetting the quad by half the stroke, which is what the first attempt did, carries the
problem inward with it. `GFX_RING_CSS` escapes it with its own `length(p)`, but that is
a circle and this control is a pill.

**GFX_RING_INSET** draws the band at `-thickness < d < 0` — strictly inside the edge,
so there is nothing to clip. It is what `box-shadow: inset` means, and neither existing
ring mode could express it.

**The " · 16 Eps" tail lost its space.** The web has one string in one span; the port
splits it in two because it is two styles (600 white, 400 grey), and a leading space
given to the tail disappears — SDL_ttf trims the line it rasterises, and the label came
out "Season 1· 16 Eps". It is drawn as an explicit 8px gap instead.

### The document coordinates all moved

These ARE the scroll targets (`NV_DETP_TARGET_ROW`), so they had to be re-measured
rather than nudged. Taken by adding `scrollTop` back onto `getBoundingClientRect` on
"The Gentlemen":

| group | was | now |
|---|---|---|
| `.series-season-row` | 1080 | **1080** (the picker draws at 1128, not 1160) |
| `.series-episode-track` | 1194 | **1240** (cards at 1256, not 1286) |
| `.series-insight-tabs` | 1680 | **1839** (labels at 1887, not 1758) |
| tab content | 1749 | **1976** (cards at 1992, not 1817) |
| document end | 2473 | 2471 measured — left at 2473 |

The tab labels also measure **36/500**, not the 32 this file recorded. Not ported yet.

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
| `cardDepth*` | off | on | ✅ Home rows (GFX_CARD_DEPTH); episode, cast and trailer switches not wired yet |
| `focusedPosterBackdropExpand*` | — | on | **not ported** |

### The poster size DOES come from `posterCardWidthDp` (re-measured 2026-09-07)

**This section said the opposite until 2026-09-07, and the web app has since
changed.** What it described was real: the modern layout used to pin the card
with `min-width: 212px` / `height: 318px`, which beat the inline variable, and
the experiment of 2026-09-01 (raising the inline value and watching nothing
move) was correct against that stylesheet.

Today those rules read the variables instead (components.css:7494 and 7499):

```css
.home-screen-shell.home-layout-modern .home-poster-card:not(.is-landscape)
  { flex-basis: var(--home-modern-portrait-poster-width); min-width: same; max-width: same }
.home-screen-shell.home-layout-modern .home-poster-card:not(.is-landscape)
  .home-poster-frame { height: var(--home-modern-portrait-poster-height) }
```

and `buildModernHomeSizingStyle` (homeScreen.js:246) writes them in the shell's
**style attribute**, where they beat the modern block's own
`--home-poster-width: 212px` (6662). MEASURED live, signed in, at the default
`posterCardWidthDp: 126`:

| element | value |
|---|---|
| shell inline | `--home-poster-width:229px; --home-poster-height:343px; --home-landscape-poster-width:419px; --home-landscape-poster-height:237px; --home-poster-radius:24px` |
| `.home-poster-card` | **229 x 347** (the 343 frame + 2px of card border top and bottom) |
| `.home-poster-frame` | 225 x 343, `border: 2px`, radius 24 |
| the art inside both borders | **221 x 339** |
| step between cards | 253 (229 + a 24 gap) |

So the port's 212 x 322 was a screenshot of an older stylesheet. `layout.h`
carries 229 x 347 for the DEFAULT 126 dp; the preference itself is still not
wired to it here.

### Rows — re-measured 2026-09-07 (modern, signed in, 1920 wide)

The row is `.home-row-head` (the title, no padding of its own) followed by
`.home-track` (`padding: 16px 52px 16px 104px`), and the rows sit in
`.home-modern-rows-scroll`, a flex column with `gap: 32px`.

| element | value |
|---|---|
| `.home-row-title` | **28 / 600**, line-height 1.2 = 33.6, letter-spacing −0.56 |
| title top -> first card | 33.6 + **16** of track padding = 49.6 |
| last card -> next title | **16** of track padding + **32** of flex gap = 48 |
| row step, end to end | 444.6 (rows at y=740.2 and 1184.8) |
| landscape-poster variant | the scroll's gap drops to 24, so the same sum gives 40 |

The 26px and the 46 of header this file carried for the row title came from the
signed-out session of 2026-08-31 and no longer hold.

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
