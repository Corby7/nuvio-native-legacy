# Post draft — NOT PUBLISHED

Written for you to review and post. I do not publish anything.

**Before posting, decide two things:**

1. **Attribution.** The text below says "native port of Nuvio". If Nuvio is not
   yours, make that explicit and credit whoever it belongs to. If it is, switch
   to first person.
2. **Where.** r/webos and r/LGOLED are the obvious candidates; each has its own
   rule about download links and self-promotion. Read the sidebar rules first —
   a post removed by rule does not come back.

Also: **video playback is not confirmed on the current target.** The root
question is settled — the development TV is now a C3 on webOS 23 running
Developer Mode only, so "needs root" is no longer true. What replaced it is a
narrower and still-open question: whether the app's LS2 role is granted under a
Developer Mode install, and whether the new exported-surface video plane
produces a picture. Until someone has watched something play on a C3, the post
must say so. Do not remove that line — it is what stops someone installing it,
getting a black screen for video, and you becoming the person who promised.

**This draft is stale in its numbers.** It still describes a 2019 C9 throughout.
Rewrite it against the C3 before posting.

## Where the links go, and why not in the body

An earlier post on r/Nuvio was caught by **"Reddit's filters"** — the SITE's
antispam, not a moderator (that message differs from "removed by moderators").
Likely cause: account age/karma, or too many links in the body.

So: **no links at all in the body.** These go in the FIRST COMMENT:

- Code: https://github.com/iqui27/nuvio-native-legacy
- Download (.ipk, 175 MB): https://github.com/iqui27/nuvio-native-legacy/releases/tag/v1.0.1

If it still gets caught, **modmail** asking for approval — the post sits in the
filtered queue, it does not vanish.

## The video

`~/Desktop/nuvio-demo-1080p60.mp4` — 2 min, 1920x1080, **7194 frames in
119.98 s = 59.96 fps measured**. Upload it as a native Reddit video; they
re-encode, so do not count on the 60fps surviving their player — which is why
the number is written into the post body.

And it shows **the interface only**: the Mac build has no video pipeline, and on
the TV no software capture can photograph the video (hardware plane). If anyone
asks for proof of playback, that needs an HDMI capture.

---

## Title (pick one)

- Native C/SDL2 port of a streaming app for webOS — 60fps on a 2019 C9
- I ported a webOS streaming app from JS to native C — 60fps on a 5-year-old OLED
- webOS native app in C: 22k lines, 60fps, QR login. Notes from the port.

---

## Body

Over the past weeks I ported a webOS streaming app from its JavaScript/Chromium
build to a native C app on SDL2 + GLES2. It runs on a 2019 LG C9 (webOS 4.10).

**Where it landed:** 60.0 fps, 0 janks on the home screen, worst frame 18-19 ms.
Video plays through the TV's own pipeline (LS2 → `com.webos.media` + libAcbAPI),
not through a browser.

The clip is a screen recording of the same code running on macOS, so it shows
the UI only — 7194 frames in 119.98 s, 59.96 fps measured. Reddit re-encodes
video, so the number is here rather than left for you to count.

**What it does now**

- Log in to your account with a QR code on the TV, session survives reboots
- Multiple profiles, with the PIN checked server-side
- Addons, layout settings, TMDB key and watch progress all come from the account
- Trakt linked from the TV itself, through Trakt's device-code flow
- Continue Watching, library, search, settings

**Things I learned the hard way, in case they save someone time**

- The TV's video lives on a *hardware plane behind* the GL surface, revealed
  through an alpha hole. `glReadPixels` and the TV's own capture service never
  see it — a screenshot during playback comes out fully black. That's the model,
  not a bug, and it costs an afternoon if you don't know.
- `StarfishMediaAPIs` calls `exit(0)` when the process doesn't match the
  `exeName` in its LS2 role. Not a crash — atexit runs and the journal stays
  silent. Talking to `com.webos.media` over LS2 directly is what actually works.
- When SAM launches a native app, stdout and stderr go to `/dev/null`. Every
  printf is discarded until you `freopen` a log file.
- The Back button never arrives as a key. It shows up as FOCUS_LOST →
  FOCUS_GAINED within a few ms, because the compositor swallows it. Real exit
  (Home) is FOCUS_LOST with no return, and that's what separates the two.
- Don't link libcurl. The SDK ships `.so.4`, the TV only has `.so.5`, and the
  binary won't even start. `dlopen` at runtime, trying both.
- A `.ipk` is a Debian package. `tar tzf pkg.ipk` lists exactly three names
  without error and never the app files — so a "did my secret leak?" check
  written that way passes every time, including when it did.

**Honest limits**

- The interface is measured at 60 fps / 0 janks on a C3 (webOS 23) and, before
  it, on a 2019 C9. Installing through Developer Mode is the path I use myself,
  so no root is needed.
- **Video playback is not confirmed on webOS 23.** The app needs LS2 access to
  `com.webos.media`, and I have not verified that a Developer Mode install
  grants it. If it doesn't, expect the UI to run and video to be a black screen.
  If anyone tries it I would genuinely like to know.
- The package is 172 MB, most of it prebaked artwork I still need to strip.
- Developer Mode sessions expire every 50 hours. LG's limit, not mine.

Happy to answer anything about the webOS side — the pipeline, the LS2 roles, or
the SDL/GLES setup on a TV this old.

---

## If they ask for proof or images

Attach real captures, not a mock-up. The ones that already exist from this
session: the login screen with the QR, the profile picker, the home with
Continue Watching, and Settings → Account.

**An obvious caution, worth repeating:** the home captures show YOUR library,
the names of YOUR profiles and the activity of YOUR friends on Trakt. Crop them
or switch profile before posting.
