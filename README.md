# nuvio-native-legacy

A streaming app for LG webOS 4.x written in C99 against SDL2 and GLES2, instead
of JavaScript in the TV's browser. Built and measured on a 2019 OLED65C9
(webOS 4.10): **60.0 fps, 0 janks** on the home screen, worst frame 18-19 ms.

Video plays through the TV's own pipeline — LS2 to `com.webos.media` plus
libAcbAPI — on a hardware plane behind the GL surface, not in a browser.

**[Download the .ipk](https://github.com/iqui27/nuvio-native-legacy/releases/latest)**
· [Install guide](INSTALL.md)

## What works

- Sign in on the TV with a QR code; the session survives reboots
- Multiple profiles, PIN checked server-side
- Addons, layout settings, TMDB key and watch progress come from the account
- Trakt and Simkl linked from the TV itself, through their device-code flows
- Continue Watching, library, search, collections, director pages, settings

## What is not verified

**Only tested on a rooted C9.** It should install through Developer Mode or the
Homebrew Channel with no root, but the app needs LS2 access to
`com.webos.media`, and whether a non-rooted install grants that has not been
measured. If it does not, expect the UI to run and video to be a black screen.
[INSTALL.md](INSTALL.md) explains the reasoning and what evidence there is.
Reports from non-rooted TVs are welcome.

The package is **175 MB**, most of it prebaked artwork that still needs
stripping — whoever installs it sees the packager's catalogue before signing in.

## Building

```bash
bash tools/mac.sh              # build and run on macOS (UI only, no video)
bash tools/arm.sh              # cross-compile in Docker, deploy over ssh
bash tools/arm.sh --ipk        # also produce the .ipk
```

Server URLs and client ids are **not in the source**. They travel from a
`local.properties` file to the compiler command line through `tools/env.sh`; the
build refuses to package a credential file and deletes the `.ipk` if one appears
(`tools/testa-ipk.sh` verifies it by extracting the `ar` archive — `tar tzf` on
an `.ipk` lists three names and passes even when a secret is inside).

## Notes for anyone porting to webOS

Written down because each one cost a day:

- The video plane sits *behind* the GL surface, revealed through an alpha hole.
  `glReadPixels` and the TV's capture service never see it — a screenshot during
  playback is fully black. That is the model, not a bug.
- `StarfishMediaAPIs` calls `exit(0)` when the process does not match `exeName`
  in its LS2 role. Not a crash, and the journal stays silent. Talking to
  `com.webos.media` over LS2 directly is what works.
- When SAM launches a native app, stdout and stderr go to `/dev/null`. Every
  printf is discarded until you `freopen` a log file.
- The Back button never arrives as a key: it appears as FOCUS_LOST →
  FOCUS_GAINED within a few ms. A real exit is FOCUS_LOST with no return.
- Do not link libcurl. The SDK ships `.so.4`, the TV has `.so.5`, and the binary
  will not start. `dlopen` at runtime, trying both.

## Documentation

Most design documents are in Portuguese, since they were written for the author:
[PORT-LEGACY.md](PORT-LEGACY.md), [PLANO-CONTA-SYNC.md](PLANO-CONTA-SYNC.md),
[MEDIDAS-WEB.md](MEDIDAS-WEB.md), [FERRAMENTAS.md](FERRAMENTAS.md).
[INSTALL.md](INSTALL.md) and this file are in English.
