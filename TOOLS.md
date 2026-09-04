# Development tools — native app

Things that did not exist and without which it was impossible to work alone on
this TV. Every one of them came out of a concrete obstacle, not a preference.

---

## 0. Build and run

```bash
bash tools/mac.sh          # build and run on the Mac, ALREADY SIGNED IN
bash tools/arm.sh          # build for ARM, install on the TV and launch
bash tools/arm.sh --build  # build only
bash tools/arm.sh --ipk    # also produce the .ipk, with no credential inside
bash tools/test-ipk.sh     # prove the .ipk carries no credential (no docker)
bash tests/account.sh      # account tests: sign-out, settings and QR
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
fix had not worked. The title carries the first 8 digits of the binary's md5
precisely so the launcher can say which build is there. To really swap it:

```bash
PID=$(sshpass -p alpine ssh root@192.168.1.32 \
      "ps aux | grep -a 'native.legacy/nuvio-proto' | grep -v grep | awk '{print \$2}' | head -1")
sshpass -p alpine ssh root@192.168.1.32 "kill $PID"
# and only then launch
```

And do not relaunch in a loop: **every start consumes backend quota**, and when
it runs out the login fails, the TV falls back to an anonymous session and syncs
the wrong account — which looks like a bug in the app.

---

## 1. Logging to a file

When SAM launches the app, `/proc/<pid>/fd/1` and `fd/2` point at `/dev/null`.
**Every `printf` was discarded** — FPS, shader errors, everything. That is why
`main.c` calls `freopen("/tmp/nuvio.log", ...)` right at the start.

```bash
(sleep 3; printf 'grep -a FPS /tmp/nuvio.log | tail -5\n'; sleep 3; printf 'exit\n') \
  | nc -w25 192.168.1.32 23 | tr -d '\0'
```

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
