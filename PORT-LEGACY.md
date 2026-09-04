# Nuvio 1.0.1 — legacy native port

This build is separate from the `nuvio-native` prototype (the Apple TV layout).
The source of truth is the `NuvioWeb-0.3.38-beta` app on the `legacy-tv` branch,
tag `v1.0.1+legacy.1`.

## Visual contract

- a fixed 144px side rail, expanded only when the menu opens;
- a modern hero at the top, media on the right and text on the left;
- an independent row viewport, with `Continue Watching` at 419×236 and portrait
  cards at 212×318, 24px gap;
- focus by border/scale, and spatial D-pad navigation;
- search, library and settings respect the same usable area after the rail.

The native code keeps the prototype's asynchronous image cache, addon/Trakt
client, video pipeline and frame telemetry. No visual token or behaviour from
the Apple TV layout is treated as a requirement of this build.

## Identity and build

During validation the package uses the id `space.nuvio.native.legacy`, so it can
be installed alongside the prototype without overwriting it. The displayed
version is `1.0.1`; the id may go back to `space.nuvio.native` only once this
build officially replaces the prototype.

```bash
bash tools/mac.sh
```

## The account, and what the package may carry

This build stopped being a ONE-owner app. Addons, profile settings, the TMDB key
and watch progress come from the ACCOUNT of whoever signs in (see
ACCOUNT-SYNC-PLAN.md); login is by QR, and the session survives a restart.

The consequence for packaging is direct: **an `art/*.txt` holding a credential
must not go into the `.ipk`**. `tools/arm.sh --ipk` already packages from a
clean copy and CHECKS the finished package; `tools/test-ipk.sh` proves it
without docker.

One known gap: the account does NOT store a Trakt credential
(`sync_pull_provider_credentials` returns tmdb, mdblist, debrid:* and others,
but no `trakt`). Since `art/trakt.txt` no longer ships in the package, whoever
installs it has no Trakt until they sign in to the web app once.

The script compiles every SDL2/GLES2 module for validation on the Mac. The
`deploy/app/nuvio-proto` file that came with the copy is only a reference
artefact and must not be distributed: the webOS package needs to replace that
executable with an ARM binary compiled from this directory, keeping the
`space.nuvio.native.legacy` manifest.
