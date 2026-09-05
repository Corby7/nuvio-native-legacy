# Development tools — native app

Things that did not exist and without which it was impossible to work alone on
this TV. Every one of them came out of a concrete obstacle, not a preference.

---

## 0. Build and run

```bash
ssh-add ~/.ssh/lgc3_webos  # ONCE per session — see below
bash tools/mac.sh          # build and run on the Mac, ALREADY SIGNED IN
bash tools/arm.sh          # build, install via Developer Mode, launch, verify
bash tools/arm.sh --build  # build only, no package and no TV
bash tools/arm.sh --ipk    # same as the default, but keeps the .ipk
bash tools/test-ipk.sh     # prove the .ipk carries no credential (no docker)
bash tests/account.sh      # account tests: sign-out, settings and QR
```

### The TV is reached by ares device name, not by IP

The target is an **OLED55C32LA (C3), webOS 23**, registered with
`ares-setup-device` as `lgc3` → `prisoner@192.168.1.100:9922`. `arm.sh` reads
the host, port, user and key out of `~/.webos/tv/novacom-devices.json` under
that name, so the deploy and the verification can never end up pointing at
different machines. Override with `NUVIO_TV_DEVICE`.

The old default, `192.168.1.32` as root with the password `alpine`, is dead
twice over: the TV does not hold that lease any more, and this one has no root
at all — port 22 is closed and only Developer Mode ssh on 9922 answers.

**The devmode key has a passphrase**, so load it into the agent once per session:

```bash
ssh-add ~/.ssh/lgc3_webos      # passphrase is in the ares device config
```

`arm.sh` uses `BatchMode=yes` deliberately: without it, a missing key turns into
an interactive passphrase prompt in the middle of a deploy, and a prompt there
is a hang. With it, you get a clear failure telling you to run the line above.

Two more flags are needed for any manual ssh to this TV — it runs OpenSSH 6.1
and offers only `ssh-rsa`, which current clients refuse by default with
"no matching host key type found":

```bash
ssh -p 9922 -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedKeyTypes=+ssh-rsa \
    prisoner@192.168.1.100
```

### Server configuration arrives by `-D`, not by file

`tools/env.sh` reads the **same `local.properties` as the web app** and prints
the `-D` flags the build needs: `NV_SUPABASE_URL`, `NV_SUPABASE_ANON_KEY`,
`NV_TV_LOGIN_BASE` and `NV_TRAKT_CLIENT_ID`. None of those values goes into a
versioned source file — they travel from the property straight to the
compiler's command line.

**Without them the app compiles, installs and opens**, and the only symptom is
the login screen saying "This package was built without a server." That is what
happened on the first deploy: the `-D` flags had only been added to `mac.sh`,
and `arm.sh` compiles inside the container with a line of its own. Today the
variables enter the container through `docker run --env-file` (passing them on
the `sh -c` line would need quotes inside quotes, which is where this breaks
again) and `arm.sh` **aborts** if `api.nuvio.tv` is not in the generated binary.

### Staying signed in on the Mac, without scanning a QR every time

The session is written to `$NUVIO_DATA`, and `tools/mac.sh` already points that
variable at `~/.nuvio`. Sign in **once** (scanning the QR that appears on
screen) and from then on every `bash tools/mac.sh` opens with the account
already in — the refresh token renews itself when it expires.

CONFIRMED: first start `[session] signed in as …`; restarting without scanning
anything, `[session] session restored …` followed by
`[addons] N from the account`.

To run as a NEW user (to test the first run of whoever installs it), point the
variable at an empty folder:

```bash
NUVIO_DATA=/tmp/nuvio-new bash tools/mac.sh
```

"Sign out", in Settings, erases the whole of `~/.nuvio` — session, settings and
progress. After that you have to scan again.

### The `.ipk` must not carry `art/*.txt`

`art/` holds a PERSON's credentials: the Trakt token, the addon URLs with the
debrid key embedded, the TMDB and mdblist keys (the latter mode 0600) and the
`settings.txt` holding the layout of whoever built it. While the app depended on
those files there was no way around it; with the account it no longer depends on
them, and packaging now runs from a **clean copy**.

`tools/arm.sh --ipk` removes those files from the staging area and **checks the
finished package**, aborting and deleting the `.ipk` if one comes back.

> A MEASURED TRAP: the `.ipk` is a Debian package (`ar` holding `debian-binary`
> + `control.tar.gz` + `data.tar.gz`). `tar tzf package.ipk` lists those three
> names **with no error at all** — never the app's files. A check written that
> way always passes, secret included. You have to unpack the `ar` and list the
> `data.tar.gz`.

`tools/test-ipk.sh` proves this without docker and was verified both ways: with
the exclusion the package comes out clean; letting `trakt.txt` in on purpose,
the test catches it and returns 1.

### Launching does not restart

`luna://com.webos.applicationManager/launch` **does not restart** an app that is
already running: it only brings it to the front. Two consecutive deploys were
read from the log of an old process, and nearly led to the conclusion that the
fix had not worked. `arm.sh` closes the app before launching for that reason:

```bash
ares-launch -d lgc3 --close space.nuvio.native.legacy
ares-launch -d lgc3 space.nuvio.native.legacy
```

The title still carries the first 8 digits of the binary's md5 so the launcher
tile says which build is there. But the title only proves what is **on disk**,
and so does an md5 — neither proves what is **running**. That is why the binary
now carries a `-DNV_BUILD` stamp and prints it as its first log line:

```bash
ssh ... prisoner@192.168.1.100 "grep -m1 '^\[main\] build' /tmp/nuvio.log"
```

`arm.sh` checks that automatically and fails the deploy if it does not match the
build it just made. This matters more than it used to: the install now goes
through `appInstallService`, the same service that answers `"returnValue": true`
and `statusValue: 264` while leaving the old binary in place — three deploys
were lost to it, one of them running a stale build for 2h30 while the log said
success.

And do not relaunch in a loop: **every start consumes backend quota**, and when
it runs out the login fails, the TV falls back to an anonymous session and syncs
the wrong account — which looks like a bug in the app.

---

## 1. Logging to a file

When SAM launches the app, `/proc/<pid>/fd/1` and `fd/2` point at `/dev/null`.
**Every `printf` was discarded** — FPS, shader errors, everything. That is why
`main.c` calls `freopen("/tmp/nuvio.log", ...)` right at the start.

The old recipe here piped commands into the debug telnet console on port 23.
That was a rooted-TV feature and it is gone; read the log over the Developer
Mode ssh instead:

```bash
SSH="ssh -p 9922 -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedKeyTypes=+ssh-rsa \
     prisoner@192.168.1.100"
$SSH "grep -a FPS /tmp/nuvio.log | tail -5"
$SSH "grep -aE '^\[(video|plane|main)\]' /tmp/nuvio.log"   # the playback story
```

The second line is the one to read when video does not appear. `[plane]` covers
the exported surface and the window id, `[video]` the LS2 pipeline, and the
order in which they stop tells you which half failed.

### When the home shows the wrong thing

The home is assembled from three sources that arrive at different times, so
"which one failed" is the first question. Each answers for itself:

```bash
$SSH 'grep -aE "^\[(addons|disc|col|sync|home)\]" /tmp/nuvio.log'
```

| Line | Means |
|---|---|
| `[addons] N from the account` | the addon list arrived; `(list changed…)` triggers a rebuild |
| `[addons] '<name>' is id '<id>'` | a manifest was read; this is what collection sources resolve against |
| `[disc] manifest <host>: N catalogue(s) usable` | per addon — `NO "catalogs" array` means it is streams-only |
| `[disc] N catalogues declared by the addons` | 0 here and the home falls back to the packaged catalogue |
| `[disc] account row prefs: N ordered, N disabled, N renamed` | the owner's order arrived |
| `[col] account collections: N folder(s) added, N skipped` | `nothing usable` keeps whatever was already shown |
| `[home] N row(s):` + the numbered list | **the final order, catalogues and collections interleaved** |

The last one is the only place the finished order exists — `[disc] row` lines are
catalogues alone, and the interleave happens later, in `home.c`. It prints only
when the order actually changes, so a burst of them during startup is the
catalogue being published row by row, not a loop.

Two failures that used to be silent and now name themselves:
`[home] order names collection_<id>, which matches no collection` (the order
mentions a collection that is not loaded) and
`[seeall] '<title>': addon '<id>' has no address; not fetching` (a collection
source whose addon is not installed).

## 1b. Testing playback without anyone on the sofa

Two `/tmp` hooks drive the player directly, so a whole codec matrix runs in ONE
app session — which matters, because relaunching costs backend quota.

```bash
# play something
$SSH 'echo "http://<your-ip>:8099/open/file.mkv" > /tmp/nuvio-video'
$SSH 'echo "-" > /tmp/nuvio-video'        # stop

# place the plane: sx sy sw sh  dx dy dw dh
$SSH 'echo "480 270 960 540 0 0 1920 1080" > /tmp/nuvio-rect'   # 2x centre zoom
```

`/tmp/nuvio-rect` exists because the crop is the one part of playback the
pipeline log cannot answer for: the uMS does not know a crop happened — the
compositor does — and the plane cannot be photographed.

**Read the verdict from the pipeline, not from the screen.** `videoInfo` gives
the codec, dimensions, `hdrType` and the HDR SEI; `sourceInfo` gives the
container and the audio/video track list. Those two lines say what the TV
actually negotiated:

```bash
$SSH 'grep -a videoInfo /tmp/nuvio.log | tail -1'
$SSH 'grep -a sourceInfo /tmp/nuvio.log | tail -1'
```

Generate test material with the ffmpeg already in the sibling web checkout
(`NuvioWeb/node_modules/ffmpeg-static/ffmpeg`) — an HDR10 file needs
`-pix_fmt yuv420p10le`, `-color_primaries bt2020 -color_trc smpte2084
-colorspace bt2020nc` and an x265 `master-display=...:max-cll=...` string, or
the TV correctly reports it as SDR and the test proves nothing.

A file written into `/tmp` over ssh belongs to `prisoner` (uid 5514) while the
app runs as uid 6435, so the app cannot delete it and logs `cannot be consumed
(different owner); ignoring`. It still acts on it — writing again with a fresh
mtime re-triggers — but clean the files up afterwards.

## 2. Screen capture

The framebuffer cannot be read even as root (`/dev/fb0` →
"Operation not permitted") and `com.webos.service.capture` answers with an
error. The only way out is for the app to photograph itself: `glReadPixels` plus
a BMP written by hand.

- BMP and not PNG because the TV's SDL2_image **cannot write** (it produced a
  0-byte file).
- A hand-written BMP and not `SDL_SaveBMP` because that one converts pixel by
  pixel when the masks do not match, and on this CPU that takes seconds — the
  file was being read half-written.
- It writes to a temporary and renames, so a partial file never exists.

```bash
bash /tmp/shot.sh 6          # build + deploy + capture + download + convert
```

## 3. Key injection

Lets you open the detail screen and navigate with nobody on the sofa holding the
remote. Write keys (`up`, `down`, `left`, `right`, `ok`, `back`) into
`/tmp/nuvio-key`.

```bash
bash /tmp/grab.sh filename down down
```

### The sticky bit trap

`/tmp` is `drwxrwxrwt` and files created by the shell belong to **root**, while
the app runs as uid 5410. In other words: **the app cannot delete them.** Until
this was spotted, every request was reprocessed on every frame — a single `down`
key became hundreds and the focus ran to the end of the page.

That is why the files are **consumed by truncating** (`fopen("w")`), never by
removing, and a request only counts while it has content. Whoever writes one has
to `chmod 666` it so the app can truncate it.

## The Back key

The remote's Back **does not reach the app as a key**. Measured by logging every
SDL event: the arrows and Enter arrive; Back produces only `FOCUS_LOST` →
`FOCUS_GAINED` within a few milliseconds — the compositor swallows the key,
takes focus away to try to close the app, and gives it back. `main.c` treats
that pair within 600 ms as Back. Really leaving (the Home key) produces
`FOCUS_LOST` with no return, and that is what separates the two cases.

There is no public documentation on delivering Back to a native app on webOS. If
a better path turns up (luna-service2, `webos_shell_surface`), switch to it.
