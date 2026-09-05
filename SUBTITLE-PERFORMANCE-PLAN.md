# Plan — customisable subtitles and perceived performance

Date: 2026-09-02. The reference TCL (com.nuvio.tv, 1920x1080) was measured with
`adb screenrecord`. The LG TV was off the network during the investigation: what
comes from it is what was already noted in the code, with its method.

Convention: **[MEASURED]** = a number obtained with a stated method.
**[ASSUMED]** = inferred from reading code, with no number. **[NOT MEASURED]** =
needs the TV. Nothing here is an invented number.

---

## FRONT 1 — Subtitle customisation

### 1.1 The uMS DOES EXPOSE subtitle attributes (evidence)

Today the subtitle is not ours: `video.c` only turns it on and off and picks a
track (`setSubtitleEnable`, `selectTrack type:text`, `setSubtitleSource`). The
question was whether it can be STYLED without a rewrite. It can.

Source 1 — the SDK that builds the app (`tools/Dockerfile`, the openlgtv
buildroot sysroot). The strings in `usr/lib/libums_pipeline.so` register these
LS2 methods, with the JSON field name next to each:

| Method `luna://com.webos.media/…` | Field | Type |
|---|---|---|
| `setSubtitlePosition` | `position` | int, −3..4 |
| `setSubtitleSync` | `sync` | int (ms) |
| `setSubtitleFontSize` | `fontSize` | int, 0..4 |
| `setSubtitleColor` | `color` | int, 0..5 |
| `setSubtitleEncoding` | `encoding` | string |
| `setSubtitlePresentationMode` | `presentationMode` | string |
| `setSubtitleCharacterColor` | `charColor` | string |
| `setSubtitleCharacterOpacity` | `charOpacity` | int 0..255 |
| `setSubtitleCharacterFontSize` | `charFontSize` | string |
| `setSubtitleCharacterFont` | `charFont` | string |
| `setSubtitleBackgroundColor` | `bgColor` | string |
| `setSubtitleBackgroundOpacity` | `bgOpacity` | int 0..255 |
| `setSubtitleCharacterEdge` | `charEdgeType` | string |
| `setSubtitleWindowColor` | `windowColor` | string |
| `setSubtitleWindowOpacity` | `windowOpacity` | int 0..255 |

`libplayerAPIs.so` has the matching handlers, plus four this SDK's uMS does NOT
expose over LS2 (`setSubtitleCharacterEdgeColor`, `setSubtitleCustomStyleEnable`,
`setSubtitleCharacterBlinking`, `setSubtitleBackgroundBlinking`).

Two names I had guessed do NOT exist: `setSubtitleAttribute` and
`setSubtitleEdgeType` — the right one is `setSubtitleCharacterEdge`, field
`charEdgeType`.

Source 2 — the web app (`playerController.js:125-140,4358-4395`) already calls
`setSubtitleFontSize` on webOS, with the comment "five discrete subtitle sizes
(0=tiny, 4=largest)". It is the only styling call it uses.

Source 3 — a community gist (aabytt/bddbb1bcf031a050d89a89aeee3a6737), which
matches the SDK's names and supplies the values: `color` 0 yellow, 1 red,
2 white (default), 3 green, 4 blue, 5 grey; named colours in
{black,white,yellow,red,green,blue}; `position` −3 (lowest) to 4.

I did NOT find the vocabulary for `charEdgeType`, `charFont`, `charFontSize` or
`presentationMode`. The official webOS OSE documentation documents no subtitle
method at all. Kodi is no help: it renders subtitles itself.

CAVEAT: the `.so` files read are the SDK's, not the TV's. Before writing any
code, check on the device —
`grep -a -o 'setSubtitle[A-Za-z]*' /usr/lib/libums_pipeline.so | sort -u`.

### 1.2 Decision: use the pipeline (a), not our own renderer (b)

Our own renderer would give continuous control over everything, and it costs: an
SRT/VTT parser, synchronisation with `video_pos()`, and the text starts
competing with `TXT_PER_FRAME=2`. Worse: **it only covers EXTERNAL subtitles** —
the ones embedded in an MKV would need demuxing, which we do not do (`mkv.c`
reads the header only). And PGS/VobSub are IMAGES: there is no way to re-render
them on either path.

The pipeline covers embedded and external with the same code and zero per-frame
cost. Our own renderer stays as plan B, external only, and only if the test on
the TV shows firmware 4.10 ignoring `fontSize`/`charColor`.

### 1.3 How the reference presents it

The TCL APK: `SubtitleStyleSettings(preferredLanguage, textColor, outlineColor)`,
events `OnSetSubtitleSize/Bold/TextColor/OutlineEnabled/OutlineColor/VerticalOffset`,
UI in `SubtitleSelectionOverlay` with a "style rail" on the SAME selection sheet.
Web: Delay, Font Size %, Bold, Text Color, Text Opacity, Outline, Outline Color,
Bottom Offset, Reset.

I did not capture the TCL's style panel (it would mean opening a title and
spending a debrid link). The protocol for capturing it later is in 2.6.

### 1.4 Options to offer

| Option | Values | Call |
|---|---|---|
| Size | Very small / Small / **Default** / Large / Huge | `setSubtitleFontSize` 0..4 |
| Colour | **White** / Yellow / Green / Blue / Red / Black | `setSubtitleCharacterColor` |
| Background | **None** / Dark 50% / Dark 100% | `setSubtitleBackgroundColor "black"` + `setSubtitleBackgroundOpacity` 0/128/255 |
| Position | 8 levels | `setSubtitlePosition` −3..4 |
| Edge | None / Outline / Shadow — **depends on the test** | `setSubtitleCharacterEdge`; CEA-708 candidates: none, raised, depressed, uniform, dropShadow |
| Delay | −5s..+5s in 250ms steps | `setSubtitleSync` |

Do not offer: text opacity (the web marks it unavailable on native webOS), font
(no known vocabulary), window (redundant with background).

### 1.5 Items, in order

**F1.0 — PROVE IT ON THE TV before coding any UI.** With a film playing, over
telnet:
`luna-send -n 1 luna://com.webos.media/setSubtitleFontSize '{"mediaId":"<id>","fontSize":4}'`
and so on, including every `charEdgeType` candidate. Verification:
`returnValue:true` AND a photo of the TV — the subtitle is drawn by the
pipeline, so it does NOT come out in `glReadPixels`, exactly like the video.
Repeat with an external subtitle and with PGS. The result decides the final
list.

**F1.1 — API in video.c/h.** `VideoSubtitleStyle {size,color,background,position,border,delayMs}`
and `video_subtitle_style()`, emitted through `call()` (video.c:614). Reapply in
`video_choose_subtitle` (1129), `video_subtitle_external` (1148) and on
`loadCompleted` (558) — the pipeline is born again on each load. Empty stub on
the Mac (43-50).

**F1.2 — Persistence** in `player.c:277-304` (`art/player.txt`, `key value`
format): `sub_size`, `sub_color`, `sub_background`, `sub_position`,
`sub_border`, `sub_delay`. It is a DEVICE preference, like `aspect` — it does
not go into `settings.txt`, which mirrors the web's layout keys.

**F1.3 — UI: a third "Style" column in tracks.c.** Rows of `label ◂ value ▸`;
OK cycles the value and applies it immediately without closing; LEFT goes back
to Subtitles. `FX_WIDTH` 1180 → 1400 to fit 3 columns.
PREVIEW: `tracks_draw` darkens the screen to 72% and the pipeline's subtitle
sits underneath — invisible. With `column==2`, drop the veil to ~0.25 and anchor
the panel at the TOP, leaving the bottom third clear. That is the live preview
the web has.

**F1.4 — Image subtitles.** `mkv.c` already reads Tracks; add `CodecID`
(EBML 0x86) to tell `S_TEXT/UTF8`, `S_TEXT/ASS`, `S_HDMV/PGS` and `S_VOBSUB`
apart. Mark them on the sheet and warn that styling does not apply.

**F1.5 — Plan B**, only if F1.0 fails: `src/subtitle.c` with an SRT/VTT parser,
drawn over the hole. Does not cover embedded subtitles.

---

## FRONT 2 — Performance

### 2.0 TCL reference **[MEASURED]**

`adb screenrecord` + `ffmpeg tblend=difference,signalstats`. The DURATIONS are
reliable; the key→pixel latencies are CEILINGS, because `input keyevent` adds
50–300 ms of its own.

| Event | Measurement |
|---|---|
| `am start -W` cold | 1417 ms |
| Launch → profile screen | ~1.8 s |
| Profile → home with TEXT and layout (grey skeleton) | **1.7 s** |
| → posters filling in | 2.0 to 3.7 s |
| → hero art | 3.8 s |
| → quiet | 4.8 s |
| RIGHT within a row | 0.59 s, with the hero crossfade alongside |
| DOWN (change row) | 1.26 s, hero changes in ~1.1 s |
| OK → detail | darkens 0.48 s, cut to skeleton, fills +0.15 to +0.55 s → **1.0–1.1 s** |
| BACK | 0.35 s |

Qualitatively: the TCL SHOWS A SKELETON IMMEDIATELY and fills it in later — it
never sits still. The hero follows the focus with a crossfade inside 0.6 s. The
focused card stays pinned to the left and the row slides underneath it.

### 2.1 Startup — the real cause of the slowness

`discover.c:build` (773-905) runs **one thread, everything in series, and
publishes only at the end** (`cat_set_all`, 896). An exact count from the code,
with the owner's addons:

1. `trakt_resume` — 1 GET `/sync/playback` + 1 Cinemeta `/meta` GET PER ITEM
   (`trakt.c:51-88`), up to 8 → **up to 9 requests in series**, 20–25 s timeout each
2. `readManifest` × 3 addons → 3 requests
3. `readCatalog` per row (853-877) → **up to 16 requests in series**, 25 s each
4. `trakt_list` watchlist + collection → 4 requests

**≈32 HTTP requests in series before home shows ONE network row.** And there is
no on-disk cache of the assembled catalogue: only the images (`art/cache`). Every
open redoes all of it. Cinemeta and Trakt are no faster on the TCL — the
difference is that it shows what it already has.

**P1 — On-disk cache of the assembled catalogue.** `build` writes
`art/catalog-net.bin` at the end; `cat_load` reads it before the package's static
data. The second open starts with yesterday's rows in <100 ms. Verification:
requests before the first useful frame drop from ~32 to 0.

**P2 — Incremental publishing.** `cat_set_all` after "Continue watching" and
after each row is read (swapping the block is already safe, catalog.c:438-486).
CAREFUL: `home.c:syncRows` calls `focus_start` and ZEROES THE FOCUS on every
count change — preserve `focus.row/column` by comparing on `key`, otherwise the
focus jumps while loading. Verification: the focus does not move during loading.

**P3 — Parallelise.** The 16 `readCatalog` calls are independent; the 3-thread
pattern already exists in `searchThread` (discover.c:354-415). The 8 Trakt
`decorate` calls likewise. Watchlist/collection after the first publish.

**P4 — Timeouts.** 25 s per catalogue and 20 s per meta, in series, become
minutes with one addon down. Bring them to 8 s, as was already done for images.
Verification: point an addon at a dead host and measure ≤ 10 s.

### 2.1b What was left after P1–P5 **[MEASURED on the Mac]**

With P1–P5 in, the remaining cost was not parallelism. Measured with a rig of
three addons (Cinemeta with 8 catalogues, WatchHub with none, one unreachable),
cold `catalog-net.bin`, three runs each; ms from `mark_start`:

| mark | before P14–P16 | after |
|---|---|---|
| `manifests read` | 420 / 351 / 344 | 206 / 200 / 164 |
| `first network row on screen` | 717 / 646 / 969 | 495 / 513 / 440 |
| `network catalog published` | 1404 / 1074 / 1830 | 653 / 704 / 572 |

And with ONE addon pointed at a black hole (a non-routable address, so connect
hangs rather than failing fast like a bad name):

| mark | before | after |
|---|---|---|
| `manifests read` | 20358 | 3022 |
| `network catalog published` | 21034 | 3515 |

The final row order is byte-identical before and after — compare the
`[disc] row %d:` lines, which are now printed once as an ordered list after the
assembly rather than as rows arrive.

**P14 — One curl handle per thread.** Every request did
`curl_easy_init`/`curl_easy_cleanup`, so the open connection, the DNS cache and
the TLS session died with it — ~32 requests at startup plus hundreds of images,
nearly all against the same few hosts, each paying DNS + TCP + handshake again.
`net.c` now keeps one handle per thread in a `pthread_key_t`, cleared between
requests with `curl_easy_reset` (which keeps exactly those three things) and
closed by the key's destructor when the thread dies — which is what the detached
threads need. Without the `curl_easy_reset` symbol it falls back to a handle per
request and says so once. Also adds `CURLOPT_CONNECTTIMEOUT` at 3 s: a dead host
used to spend the whole transfer budget on a connect that would never complete.
Verification: `network catalog published` drops by roughly a third on the Mac
(above); expect more on the TV, where the handshake costs more. Force the
fallback by skipping the `dlsym` and confirm the app still loads — done, 8 rows,
841 ms.

**P15 — The transfer byte cap was a process global.** `net_cap` was consulted by
`receive`, which is the write callback for EVERY transfer on every thread. While
`mkv.c` read a 320 KB MKV header, that ceiling applied to whatever the four
artwork threads were downloading. Now the cap lives in the `Bucket`.
Verification: a harness with one thread pulling a 782 KB image in a loop and
another reading a 320 KB chunk of it — before, 12 of 12 image pulls ABORTED
(curl error 56) and returned NULL; after, 12 of 12 come back full size. Note the
`r == 23 && cap > 0` line, which turns a write-abort into success: with a global
cap that also let a foreign transfer's truncated body be reported as success and
written to `art/cache`, which has no expiry and no eviction.

**P16 — Manifests in parallel, underneath the Trakt calls.** They were read one
at a time on the assembly thread, and nothing else ran until the last answered —
while `trakt_resume` and `trakt_social` had already spent up to three 20–25 s
GETs ahead of them, with the network idle as far as the addons were concerned.
`manifestsStart()` now fires them at the top of `build()` and `manifestsJoin()`
collects them in ADDON ORDER (the default row order) after the Trakt work.
`readPrefs` deliberately did NOT move: the account can deliver preferences at any
moment, so it stays next to the ordering that consumes it. Verification:
`manifests read` roughly halves, and a hung addon costs 3 s instead of 20 s.

**P17 — Publish on arrival instead of in reading order.** The assembly waited for
bucket k to publish bucket k (`SDL_Delay(10)` spin), so one slow row held back
every row already finished behind it — 8 s of hidden home for one bad addon. A
condition variable now wakes the assembly on any ready bucket and it REBUILDS
from every ready bucket in preference order, skipping what has not landed. Row 5
can appear before row 3, and row 3 inserts above it when it arrives; `home.c`
already re-finds focus and scroll by row KEY on every publish, precisely because
"a new row can enter in the middle". Publishes are capped at one per frame:
`cat_set_all` keeps only ONE garbage block, so two back-to-back publishes could
free a block the drawing thread is still walking, and it re-reads `progress.txt`
every call. Verification: the focus does not move during loading (same check as
P2), and the final row order matches baseline exactly.

### 2.1c On the TV **[MEASURED]**

OLED55C32LA, webOS 23 (sdk 10.3.1), build `20260905-152123`, verified on disk
(md5 `ddedaf1e`) AND running (the process reported its own build id).

The account delivers 2 addons — AIOMetadata, which declares **151 catalogues**,
and AIOStreams, which declares none — and the home builds 24 rows
(`CAT_FILTER_MAX`). Note the shape of a real launch: the FIRST `build` runs at
359 ms with zero addons and finishes in 2 ms, and the real one is the REBUILD
`disc_rebuild` fires when the account list lands at 1308 ms. Times below are from
that rebuild, which is the one the owner actually sees:

| mark | ms | Δ from `build: start` |
|---|---|---|
| `build: start` | 1308 | — |
| `manifests read` | 1960 | 652 |
| `first network row on screen` | 2178 | 870 |
| `network catalog published` | 3480 | 2172 |

24 rows in 2.2 s, and `/tmp/nuvio-fps.txt` reports `FPS=60.0 worst=21.5ms
janks=0 textures=15 14.9MB evictions=0` — no regression against the baseline
recorded at `catalog.h`.

P17 is visible in the log: the `[disc] row ready:` lines arrive in NETWORK order
(Trending-Film first) and the `[disc] row %d:` list ends in PREFERENCE order
(Popular-Series at 0, Trending-Film at 2). Rows landed out of order and reflowed
into place, which is exactly the intent.

**No TV baseline was taken.** The before/after table in 2.1b is the Mac's; the
old build was never put back on the TV to measure it there. So the TV numbers
above stand on their own — they say the new code is fast, not by how much it
improved on that hardware.

**Still open here.** `first network row on screen` is still gated by the manifest
JOIN — catalogues from an addon whose manifest already landed could start before
the slowest manifest returns. With only 2 manifests and one of them carrying 151
catalogues, that gate costs 652 ms of the 870 here, so it is the next thing worth
doing. And `CAT_THREADS` stays at 3: raising it to 5 measured no better on the
Mac, but that rig had one catalogue host — on the TV nearly all 24 rows come from
a SINGLE host (aiometa.minitwit.app), so this is now measurable properly and was
not re-tested.

### 2.2 Artwork **[ASSUMED]**

`tex_cache.c`: **the download happens INSIDE the decode thread** (`ensureLocal`
291, called from `threadDecode` 377). With a cold cache each of the 2 threads
sits blocked on the network for up to 8 s instead of decoding.

**P5 — Separate download from decode.** Network-only threads feeding the decode
queue. The network is I/O, not CPU: 4 downloads can run without competing with
the drawing.

**P6 — Queue by visibility.** Today it is FIFO. Priority = distance from the row
to the focus.

**P7 — Skeleton on home.** Guarantee a grey block plus a label when `tex_get`
returns 0. That is what makes the TCL look ready at 1.7 s.

### 2.3 D-pad response

Structural latency = 1 frame of waiting + 1 of presentation ≈ 40 ms. That is not
"sluggish". What is:

**P8 — Text arriving by drip.** `TXT_PER_FRAME 2`. `detail.c` has 58 `txt_*`
call sites. If the detail rasterises 30 new lines, that is 15 frames =
**300 ms of text appearing bit by bit** after the OK. Pre-rasterise the fixed
lines on open with a larger budget for 1 frame, when t≈0 and nothing else is
expensive. Verification: a capture 100 ms after the OK already has all the text.

**P9 — Hero with compounded delay.** `NV_HERO_IDLE_MS 220` + 330 fade + decode.
On the TCL the crossfade finishes 0.6 s from the key press. Worse: `home.c:75-80`
documents "CLEAR → EMPTY → CUT" — there is an EMPTY frame between one piece of
art and the next; the TCL crossfades with no empty frame. Request the texture
when the item becomes a CANDIDATE, not when it becomes `heroCurrent`.

**P10 — `txt_evictions` is not in the report.** It exists in `text.c` but
`main.c:508-537` does not print it. Add it. It should be 0 with the screen idle.

### 2.4 Animations — dt and springs **[MEASURED by a full read]**

dt: EVERY screen receives dt from `app_update`, which comes from
`SDL_GetPerformanceCounter`. None computes its own dt. The remaining
`SDL_GetTicks` calls are timers (hold, hero rest, toast) — correct usage.
**Nothing to fix here.**

**P11 — A real spring inconsistency.** Home scrolls with `anim_spring2` (2nd
order, w=11.5, measured against the reference). Detail, search, library, settings
and streams scroll with the 1st-order `anim_spring` at k=8 — a different shape,
leaving at maximum speed. Migrate the five to `anim_spring2`, keeping a velocity
next to each `scroll*`.

Screen changes (330 ms) are comparable to the TCL (0.48 s opening, 0.35 s
closing). **The slowness is not in the transition — it is in the content that
arrives afterwards.**

### 2.5 Opening and playing

**P12 — Sources in series.** `addons_fetch` queries the addons one at a time
with a 25 s timeout; then `stream_first_good(8)` does up to 8 `net_url_final`
calls in series at 20 s each. Parallelise the addons and check the best 3
together.

**P13 — The detail layout jumps** when the data arrives (`detail.c:1013-1017`
recalculates per frame). Reserve the height of the sections that are known.

### 2.6 Measurement protocol on the LG

1. Marks `printf("[t] %u <event>", SDL_GetTicks())` at: entry to main, first
   SwapWindow, `[disc] row 0`, catalogue assembled, first row of textures
   complete, `detail_open`, first frame with no pending text, `loadCompleted`.
2. A fixed route, with a cold cache (delete `art/cache`) and a warm one.
3. A burst of captures: `touch nuvio-shot-req` every 100 ms for 1 s after the
   key. It is the only way to measure key→pixel on the LG.
4. Side by side with the TCL, the same sequence as the table in 2.0.

### 2.7 Assumptions checked

- "With video playing the UI draws nothing": confirmed (`player.c:811-812`).
- The 512-line cache and `txt_evictions`: both confirmed, but the counter is not
  reported (P10).
- No assumption about FPS / fill rate / 1062px was contradicted.
- `TOOLS.md` is out of date: it mentions `/tmp/nuvio-shot.png`, the code writes
  `.bmp` (`main.c:168`).

---

## Order by impact on perceived slowness

1. P10 telemetry — a prerequisite for measuring the rest
2. P1 catalogue cache + P2 incremental publishing
3. P3/P4 parallelise and shorten timeouts
4. P8 text drip when the detail opens
5. P9 hero with no empty frame and no 220+330 ms
6. P5/P6 download outside the decode threads + priority by visibility
7. P12 sources in parallel
8. P11 the same spring on every scroll
9. P7/P13 skeletons
10. Front 1: F1.0 (prove it on the TV) → F1.1 → F1.2 → F1.3 → F1.4
