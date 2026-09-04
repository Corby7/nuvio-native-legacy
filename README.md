# nuvio-native-legacy

A streaming app for LG webOS written in C99 against SDL2 and GLES2, instead of
JavaScript in the TV's browser. The current target is an **OLED55C32LA (a C3) on
webOS 23**, where the interface runs at **60.0 fps, 0 janks**, worst frame
19-20 ms, on a Mali-G52 with GLES 3.2.

It began on a rooted 2019 OLED65C9 (webOS 4.10), which measured the same 60.0
fps / 0 janks at worst frame 18-19 ms. Comments and docs that name the C9 are
findings from that machine; they have not been re-measured on the C3 unless they
say so.

Video plays through the TV's own pipeline — LS2 to `com.webos.media` — on a
hardware plane behind the GL surface, not in a browser. The plane is attached by
exporting the app's own Wayland surface to the compositor; up to webOS 4 that
job was done by `libAcbAPI`, which does not exist from webOS 5 onwards.

**[Download the .ipk](https://github.com/iqui27/nuvio-native-legacy/releases/latest)**
· [Install guide](INSTALL.md)

## What works

- Sign in on the TV with a QR code; the session survives reboots
- Multiple profiles, PIN checked server-side
- Addons, layout settings, TMDB key and watch progress come from the account
- Trakt and Simkl linked from the TV itself, through their device-code flows
- Continue Watching, library, search, collections, director pages, settings

## What is not verified

**Root is no longer needed to install, and no longer available to test with.**
The C3 runs Developer Mode only: ssh is `prisoner@<ip>:9922`, port 22 is closed,
and the app directory is not writable by that user. `tools/arm.sh` installs the
`.ipk` with `ares-install` and then verifies the result, because that path goes
through the installer that has been caught reporting success without replacing
the binary.

**Measured on the C3**, with a 1080p H.264/AAC file served over HTTP from a
laptop on the same network:

- The LS2 role **is** granted under a Developer Mode install — `com.webos.media`
  answers, `LSRegister` succeeds, `load` returns a mediaId.
- The compositor assigns a window id (`_Window_Id_13`) and accepts the exported
  window's source/destination regions.
- The pipeline allocates hardware decoders (`VDEC`, `ADEC`), reports the stream
  back correctly (1920x1080, 30 fps, avc1/aac), reaches `loadCompleted` in
  ~320-380 ms and advances `currentTime` steadily with no errors.
- The serving host's access log shows the TV fetching the file — the independent
  check, since the plane cannot be screenshotted.
- Stop-and-play-again works, reusing the same exported window: the old "second
  playback is black with audio" bug cannot recur, because there is no longer a
  per-session bind to go stale.
- The interface holds 60.0 fps / 0 janks *while* decoding.

### Codec and HDR support, measured on the C3

Each row played from a laptop over HTTP; the verdict is what the pipeline itself
reported back in `videoInfo`/`sourceInfo`, with no errors and `currentTime`
advancing.

| Source | Result |
|---|---|
| H.264 8-bit + AAC, MP4 | plays |
| HEVC 8-bit + AAC, MP4 (`profile main`) | plays |
| **HEVC 10-bit HDR10, MKV** | **plays, `hdrType: HDR10`** |
| H.264 + **AC3** 5.1, MKV | plays |
| H.264 + **E-AC3** 5.1, MKV | plays |
| H.264 + **DTS** 5.1, MKV | plays |
| H.264 3840x2160, MP4 | plays |

HDR is real, not just decoded: the mastering-display metadata arrives intact
(`maxContentLightLevel: 1000`, `maxPicAverageLightLevel: 400`, BT.2020 primaries,
`transferCharacteristics: 16` = PQ) and the pipeline classifies the stream as
HDR10 on its own. Nothing in this app asserts an HDR type any more — that was
the ACB's job and it is gone. The badge shown to the user comes from what the
pipeline reports, never from what the addon claimed.

Source cropping works: a non-identity `source` region is accepted and applied
(`[plane] source 480,270 960x540 -> destination 0,0 1920x1080`), which is what
the zoom modes are built on.

### Known limitation: no request headers

**A source that needs an HTTP header will not play.** The `load` payload carries
a URI and nothing else, and the pipeline fetches it with its own GStreamer HTTP
client:

```
GET /auth/test.mp4 auth=None ua='GStreamer souphttpsrc (compatible; LG NetCast.TV-2013)'
-> 401
[video] ev {"error":{"errorCode":40401,"errorText":"server error:40401"}}
[video] ev {"error":{"errorCode":206,"errorText":"Media Authorized Error"}}
```

That is measured, not theoretical. It affects debrid or proxied sources that
authenticate with a header rather than a pre-signed URL. The web app hits the
same wall — `<video>` cannot attach headers either — and works around it with a
local proxy that injects them (`js/platform/webos/webosPlaybackProxy.js`); the
same approach is the fix here.

Useful while debugging: `errorCode 404xx` encodes the HTTP status the pipeline
got, so `40401` is a 401 and `40403` is a 403/404. That is the difference
between "the server refused us" and "the pipeline cannot play this".

**Still unverified:** that a person sitting in front of the TV sees the picture,
and that the cropped rectangle lands where it should. Nothing in this project
can answer either from a terminal — the plane is invisible to `glReadPixels` and
to the TV's capture service — so every claim above is a state-and-log claim.
Dolby Vision is also untested; the `DolbyHdrInfo` opt-in remains off by default.

The package the current tree builds is **~24 MB**, nearly all of it prebaked
artwork — whoever installs it sees the packager's catalogue before signing in.
(Older notes in this repo say 175 MB; that was a larger `art/` and it no longer
matches what `tools/arm.sh` produces. The published v1.0.1 release has not been
re-measured.)

## Building

```bash
ssh-add ~/.ssh/lgc3_webos      # once per session: the devmode key has a passphrase
bash tools/mac.sh              # build and run on macOS (UI only, no video)
bash tools/arm.sh              # cross-compile, install via Developer Mode, verify
bash tools/arm.sh --build      # build only, no package and no TV
bash tools/arm.sh --ipk        # same as the default, but keeps the .ipk
```

The Mac build has **no video at all** — there is no compositor and no plane, so
`plane.c` and the device half of `video.c` are stubs there. That also means the
Mac never compiles the half that talks to the TV: a mistake in it survives every
local build and only appears on the device.

Server URLs and client ids are **not in the source**. They travel from a
`local.properties` file to the compiler command line through `tools/env.sh`; the
build refuses to package a credential file and deletes the `.ipk` if one appears
(`tools/test-ipk.sh` verifies it by extracting the `ar` archive — `tar tzf` on
an `.ipk` lists three names and passes even when a secret is inside).

## Notes for anyone porting to webOS

Written down because each one cost a day:

- The video plane sits *behind* the GL surface, revealed through an alpha hole.
  `glReadPixels` and the TV's capture service never see it — a screenshot during
  playback is fully black. That is the model, not a bug.
- `StarfishMediaAPIs` calls `exit(0)` when the process does not match `exeName`
  in its LS2 role. Not a crash, and the journal stays silent. Talking to
  `com.webos.media` over LS2 directly is what works. (On webOS 23 the library is
  not on the TV at all, so the question does not arise.)
- **`libAcbAPI` is gone from webOS 5 onwards.** It was only ever a proxy: the
  app's LS2 role cannot send to `com.webos.service.tv.display`, and libAcbAPI
  could. From webOS 5 the app exports its own surface instead
  (`wl_webos_foreign.export_element`), receives a `window_id_assigned` event,
  and passes that id in the `com.webos.media` `load` payload — no display
  service in the middle. Requiring libAcbAPI at startup is worse than useless:
  its `dlopen` failing took down the LS2 half, which works fine without it.
- **`NDL_DirectMedia` is not a URL player**, despite being the obvious-looking
  successor. `NDL_DirectMediaLoad` takes codec parameters and you feed it
  elementary-stream buffers with `NDL_DirectVideoPlay`. It suits a live game
  stream, not a file: using it here would mean writing an HTTP demuxer, and its
  v2 audio enum covers only PCM/MP3/Opus.
- Neither region argument of `wl_webos_exported.set_exported_window` accepts
  NULL. libwayland catches that client-side, logs `null value passed for arg`
  and **drops the request** — so "no crop" has to be spelled as a region
  covering the whole decoded frame, not as nothing.
- When SAM launches a native app, stdout and stderr go to `/dev/null`. Every
  printf is discarded until you `freopen` a log file.
- The Back button never arrives as a key: it appears as FOCUS_LOST →
  FOCUS_GAINED within a few ms. A real exit is FOCUS_LOST with no return.
- Do not link libcurl. The SDK ships `.so.4`, the TV has `.so.5`, and the binary
  will not start. `dlopen` at runtime, trying both.

## Documentation

- [PORT-LEGACY.md](PORT-LEGACY.md) — what this build is, and what it is not
- [ACCOUNT-SYNC-PLAN.md](ACCOUNT-SYNC-PLAN.md) — the account, the sync contract
  and every RPC, with what was measured against the real server
- [WEB-MEASUREMENTS.md](WEB-MEASUREMENTS.md) — every dimension measured in the
  running web app, the reference for the 1:1 port
- [SUBTITLE-PERFORMANCE-PLAN.md](SUBTITLE-PERFORMANCE-PLAN.md) — subtitle styling
  through the TV pipeline, and where the perceived slowness actually is
- [TOOLS.md](TOOLS.md) — building, logging, screen capture and key injection
- [INSTALL.md](INSTALL.md) — installing on another TV
