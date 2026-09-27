# Live TV (IPTV)

**Live TV** is a destination on the side bar, between Library and Settings. It
plays the viewer's own IPTV service: an M3U playlist, or an Xtream Codes login,
with an XMLTV guide. It is independent of the Nuvio account and of the addons;
nothing about it is synced, and it belongs to the TV rather than to a profile:
every profile sees the same source, favourites and history.

## Using it

The first visit opens the setup form:

- **M3U playlist**: the playlist's address, and optionally a guide (XMLTV)
  address. Leave the guide empty when the playlist names its own
  (`#EXTM3U url-tvg="…"`).
- **Xtream Codes**: server, username and password. The playlist and guide are
  built from them (`get.php?…&type=m3u_plus&output=m3u8` and `xmltv.php`).

Text is typed on the TV's own keyboard, the same one Search uses. Where there
is no on-screen keyboard, the source can be written by hand into `iptv.txt` in
the app's data folder (see *Files* below).

After that, Live TV opens on the **channel list**; the **guide** is one press
away in the header. Both share the header and the category chips, so switching
changes only the body. The screens follow the Y1 (guide), Y2 (list) and Y3
(mechanics) mockups.

```
 Live TV                                      IPTV · 312 channels  [Guide] [Source]
 (All channels) (Favourites) (Recent) (News) (Sport) …
 101 [WN] World News    The Big Match ▬▬──   Next · Weather   ┌────────────────┐
 102 [N24] News 24      Market Watch ▬▬▬▬─   Next · Science   │ LIVE  preview  │
┃103 [BT] Business Today The Big Match ▬▬─   Next · Cooking ┃ │ of the TUNED   │
 104 …                                                        └────────────────┘
                                                               ON NOW 20:00 – 21:00
                                                               The Big Match
                                                               THEN 21:00 … · 21:30 …
                                                               [▶ Watch] [☆ Favourite]
```

The list answers "what can I watch right now", which is how most live-TV
sessions start. The guide answers "what's on at 21:30": channels down, two hours
across, a now line in the seek bar's colour with the minute on its cap, and the
aired part of every programme on air tinted.

**Watching is not pointing.** The preview always plays the **tuned** channel,
marked with the Sources panel's equaliser wherever it is listed. Moving the
focus changes text, never the stream; only OK tunes.

### Remote

| Where | Key | Does |
|---|---|---|
| Header | ← → · OK | Guide/Channels toggle, Source (setup; Reload lives there) |
| Chips | ← → | choose a category, applied at once · ← from the first opens the side bar |
| List | ↑ ↓, CH+ / CH− | channels; ↑ from the top row reaches the chips |
| List | → | the Watch / Favourite buttons beside the list |
| List | ← | the side bar |
| Guide | ↑ ↓ | channel; the focus keeps its **time**, so ↓ from a 21:00 film lands on what the next channel shows at 21:00 |
| Guide | → ← | next / previous programme, paging the timeline in 30-minute jumps; ← from what is on now lands on the channel's own cell, ← again opens the side bar |
| List, guide | OK | watch full screen (on the channel already playing: just full screen) |
| List, guide | hold OK | add to / remove from Favourites |
| List, guide | 0–9 | type a channel number |
| List, guide | Back | guide → list; list: stop the preview, then the chips, then leave |
| Full screen | ↑ ↓, CH+ / CH− | zap through the category being browsed (debounced: holding CH+ opens one stream, not ten) |
| Full screen | ← or OK | the quick channel list |
| Full screen | → | the channel banner (now / next, progress, clock) |
| Full screen | 0–9 | type a channel number |
| Full screen | Back | back to the screen; the picture keeps playing in the preview |

The Magic Remote pointer works on the header, the chips, the list, the guide's
channels and programmes, the buttons, the setup form and the quick list.

### Degrading well

An EPG is a bad-data problem, so every channel and programme has a floor:

- **Channel identity, three tiers.** A 54/58 px tile, always drawn and always
  filled: the logo on a `#1A1D21` plate with 9 px of air; else a monogram from the
  name ("News 24" → N24; provider noise like `|UK|`, `[FHD]`, `HD` skipped); else
  the bare number. A logo that 404s looks like a channel that never had one, and
  the texture cache remembers the failure.
- **Block widths.** At 200 px and over, a programme shows its title and both
  times; from 96 px, the title and the start; under 96 px, no text, just a dot.
  The focused programme's full title is always in the detail band.
- **No guide data.** One dashed block spanning the row — absence, not a very long
  programme. The dashes are one texture per row size, not a hundred rects.

## Files

All in the app's data folder (`data.h`; `$NUVIO_DATA` on the Mac):

| File | Holds |
|---|---|
| `iptv.txt` | the source: `kind=m3u\|xtream`, `url=`, `epg=`, `server=`, `user=`, `pass=` — one per line. **The password is stored in plain text**, like the account session beside it. |
| `iptv_playlist.m3u` | the last playlist that parsed, so the channels appear at once on the next visit (and when the provider is unreachable) |
| `iptv_favourites.txt`, `iptv_recent.txt` | one channel name per line. Keyed by name, not URL: providers rotate stream URLs (they carry the credentials) far more often than they rename channels |

The guide is not cached: stale programme data is worse than a "loading" line.

## How it is built

| File | Role |
|---|---|
| `src/iptv_parse.[ch]` | M3U and XMLTV parsing into one `IptvList`; gzip through the TV's own `libz` (dlopen, like libcurl). No SDL, no network — `tests/iptv_parse.sh` covers it under ASan/UBSan |
| `src/iptv.[ch]` | the source, the loader thread, the playlist cache, favourites and history |
| `src/iptvui.[ch]` | the screen: setup, channel list, guide, full-screen playback |
| `deploy/app/art/icons/menu_live*.png` | the side-bar glyph, drawn on Phosphor's 256 grid to match the others |
| `deploy/app/art/icons/live_*.png` | the header, button and favourite glyphs, from the mockups' SVGs |

Things worth knowing before changing them:

- **Two-stage load.** A playlist lands in a second; a guide can take half a
  minute. The channels are installed as soon as they parse, then replaced by
  the same channels with their programmes. Both come from the same playlist
  text, so channel **indices survive the swap** — the screen keeps indices
  across frames, never pointers.
- **The guide is windowed** to three hours back and thirty ahead, and only for
  channels in the playlist. A full provider XMLTV is a week of thousands of
  channels.
- **Channel matching**: `tvg-id` first; then the XMLTV `<channel>`'s display
  names against `tvg-name` / the channel name, for playlists without ids.
  Channels sharing a `tvg-id` (HD and SD copies) all get the guide.
- **Playback** calls `video.h` directly rather than going through the film
  player, which is bound to catalogue items, progress and Trakt. `video_set_mp4(1)`
  skips the MKV probe: it would open a second connection, and most IPTV accounts
  allow one.
- **Request headers** from `#EXTVLCOPT:http-user-agent` / `http-referrer` and
  `#EXTHTTP` go through the loopback relay (`proxy.h`) — **except for HLS**,
  whose segment URLs would resolve against the relay's address. An HLS channel
  that requires headers will not play yet.
- The router stops the stream whenever Live TV is not the current screen,
  whatever route left it (`iptvui_background`).

## Tests

```sh
bash tests/iptv_parse.sh   # parser: attributes, headers, groups, times, entities, gzip, 120k programmes
bash tests/iptv_ui.sh      # the screen off the TV, over file://; writes /tmp/nuvio-live-*.bmp
```

`iptv_ui.sh` generates a 48-channel playlist and a gzipped guide around the
current time, drives the screen with key events, and asserts the focus model
(the guide's time-keeping ↓, → and ←, ← onto the channel cell and then the side
bar), the favourite and the history round-trips. It captures the list, the list's
actions, the guide (now, later, on a channel cell), an empty category, a
category, full screen, the quick list and the setup form. It runs on the Mac,
and on Linux under `xvfb-run` with a GLES context. Off the TV there is no video
pipeline, so previews show their idle face and full screen its error banner.

## Performance

Measured off the TV on an x86 core, 2026-09-27. The TV's cores are several
times slower, so read these as proportions, not as the C3's numbers.

**Frames** (48 channels, main-thread CPU time; the GPU was a software
rasteriser, so GPU time is not meaningful here):

| | CPU / frame | draw calls | text settles in |
|---|---|---|---|
| List, steady | ~1.0 ms | ~140 rects, ~85 texture binds | — |
| List, first open | worst 2.6 ms | | 32 frames (~0.53 s) |
| List, holding DOWN through 20 rows | worst 2.7 ms | | 5 frames after stopping |
| Guide, steady | ~1.1 ms | ~165 rects, ~90 texture binds | — |
| Guide, first open | worst 2.9 ms | | 33 frames (~0.55 s) |
| Guide, six programmes to the right | worst 2.0 ms | | 12 frames (~0.2 s) |
| Guide, two pages down | worst 1.9 ms | | 26 frames (~0.43 s) |
| Guide, holding DOWN through 20 rows | worst 2.6 ms | | 16 frames after stopping |

The frame cost is low; what is visible is **text arriving**. `text.c`
rasterises at most two new lines per frame (`TXT_PER_FRAME`, set from a
measurement on the TV), and a fresh page carries 60-100 distinct strings —
titles, times, channel names. So after opening or paging, cells fill in over a
quarter to half a second. The budget is the app's, deliberately. Y3's block
tiers already help (narrow blocks draw no text, mid-width ones only the start
time); the next lever, if this reads as slow on the TV, is fewer strings still.

**Loading** (`iptv_parse.c`, -O2, run on the loader thread, never on a frame):

| | size | time |
|---|---|---|
| Playlist, 10 000 channels, 360 groups | 2.1 MB | 30 ms |
| Guide, 2 000 channels × 7 days, gzipped | 4.1 MB → 140 MB XML | inflate 173 ms, parse 484 ms |
| Kept after the -3 h..+30 h window | 77 493 programmes | — |
| Resident afterwards (channels + guide) | ~16 MB | — |

The cost to watch is **memory while the guide loads**: the whole inflated XML
is held at once, about 140 MB for that guide, and more when a panel serves it
uncompressed (the download buffer grows by reallocation). On a TV that can end
the app, so a streaming parse — inflate and parse a chunk at a time, a few MB
resident — is the first thing to do if large guides misbehave.

## Not verified yet

Everything above was exercised off the TV. **Nothing has been played on the C3
yet**, so these are open:

- Whether `com.webos.media` plays live HLS and MPEG-TS from typical providers
  with `type: "media"` as-is, or needs a different load payload for live
  (no duration, no seek). The film player's payload is reused unchanged.
- How the pipeline reports a dead stream (the screen watches
  `video_error_count`) and how long a stall takes to surface.
- Memory and parse time for a large real guide (tens of MB inflated) on the TV.
- The TV keyboard for long URLs (the phone keyboard via LG ThinQ helps).

## Roadmap

Roughly in order:

1. Verify playback on the C3 and adjust the load payload for live streams.
   Watch memory while a large guide loads (see Performance).
2. Search channels (and programmes) by name.
3. Reminders: OK on a future programme offers "Remind me", with a toast when it
   starts.
4. Catch-up / timeshift for channels with `catchup-days` (Xtream `timeshift.php`,
   `catchup-source` templates) — the guide already dims the past, ready for it.
5. Xtream VOD and series as their own rows (`get_vod_streams`,
   `get_series`) — or left to the addons, which already cover films and series.
6. More than one source, and hiding / reordering groups.
7. Headers for HLS channels (a relay that rewrites the playlist's segment URLs).
8. Parental lock on groups, using the profile PIN.
