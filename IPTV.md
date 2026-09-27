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

After that, Live TV opens on the guide:

```
 Live TV                                   host · 312 channels   [Reload] [Source]
 101 · BBC One                                              ┌──────────────────┐
 The Programme Title                                        │  preview: the    │
 20:00 – 21:00 · 25 min left · Drama                        │  channel last    │
 ▬▬▬▬▬▬▬▬▬▬▬▬───────                                        │  tuned           │
 Synopsis, three lines…                                     └──────────────────┘
 All channels   Sun 27 Sep │20:00        │20:30        │21:00        │21:30
 Favourites     101 [logo] BBC One   │ Programme          │ Next one  │ …
 Recent         102 [logo] …         │ …
 News …
```

### Remote

| Where | Key | Does |
|---|---|---|
| Guide | ↑ ↓ | channel; the focus keeps its **time**, so ↓ from a 21:00 film lands on what the next channel shows at 21:00 |
| Guide | → ← | next / previous programme; ← from what is on now goes to the groups |
| Guide | CH+ / CH− | page through channels |
| Guide | OK | watch full screen |
| Guide | hold OK | add to / remove from Favourites |
| Guide | 0–9 | type a channel number |
| Guide | Back | stop the preview; again: to the groups; again: leave |
| Groups | ↑ ↓ | choose a group (applies at once) · ← opens the side bar |
| Full screen | ↑ ↓, CH+ / CH− | zap through the group being browsed (debounced: holding CH+ opens one stream, not ten) |
| Full screen | ← or OK | the quick channel list |
| Full screen | → | the channel banner (now / next, progress, clock) |
| Full screen | 0–9 | type a channel number |
| Full screen | Back | back to the guide; the picture keeps playing in the preview |

The Magic Remote pointer works on the guide, the groups, the header buttons, the
setup form and the quick list.

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
| `src/iptvui.[ch]` | the screen: setup, guide, full-screen playback |
| `deploy/app/art/icons/menu_live*.png` | the side-bar glyph, drawn on Phosphor's 256 grid to match the others |

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
current time, drives the screen with key events, asserts the focus model, the
favourite and the history round-trips, and captures the guide, a group, full
screen, the quick list and the setup form. It runs on the Mac, and on Linux
under `xvfb-run` with a GLES context.

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
