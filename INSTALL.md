# Installing on another TV

Download `space.nuvio.native.legacy_1.0.1_arm.ipk` from the
[releases page](https://github.com/iqui27/nuvio-native-legacy/releases/latest),
or build it yourself:

```bash
bash tools/arm.sh --ipk
```

The package contains **no credentials**. `tools/test-ipk.sh` proves it, and
`arm.sh` aborts and deletes the package if one appears.

---

## Do you need root? No — and the development TV no longer has it either.

| | Developer Mode | Homebrew Channel | Root |
|---|---|---|---|
| Root required | no | no, to install the channel | yes |
| Lifetime | **50 h session**, renewable | permanent | permanent |
| LG developer account | required | only to install the channel | no |
| Install with | `ares-install` | from the TV itself | — |

Developer Mode is now the path the project itself uses, so it is the one that
gets exercised on every deploy rather than the one nobody tried.

### Developer Mode

```bash
ares-setup-device
ares-install space.nuvio.native.legacy_1.0.1_arm.ipk -d <name>
ares-launch space.nuvio.native.legacy -d <name>
```

`tools/arm.sh` now does exactly this, plus the build and the verification —
`ares-install -d <device>` against `prisoner@<ip>:9922`. It used to be the
opposite: it copied the binary in as root over port 22, which is no longer
possible and is no longer what the script does.

### Homebrew Channel

Install the `.ipk` through the channel. It puts apps in
`/media/developer/apps/usr/palm/applications/`, which is **exactly** the
directory this app runs from on the development TV — same directory, same
environment.

---

## The caveat that matters: LS2

The risk is not the installation. It is the **bus permission**.

This app talks directly to `luna://com.webos.media`. It registers on the bus as
`com.webos.media.client.nuvio`, because the app's LS2 role only allows names
matching `com.webos.media.client.*`.

That role does **not** reach `com.webos.service.tv.display` — `src/video.c`
records the hub refusing it with "Not permitted to send to
com.webos.service.tv.display". Up to webOS 4 the way around that was
`libAcbAPI`, which could reach it. **That library does not exist from webOS 5
onwards**, and on the C3 `find /` turns up nothing.

The video plane therefore no longer goes through any display service. The app
exports its own Wayland surface (`wl_webos_foreign.export_element`), the
compositor answers with a window id, and that id is handed to `com.webos.media`
in the `load` payload. Nothing has to be permitted, because nothing extra is
addressed.

The package **ships no LS2 role file**: only `appinfo.json`, with
`requiredPermissions: ["all"]`, so bus access depends on what the install
directory grants. That was the open question for a long time, and on this TV it
is now **settled: it is granted.**

Measured on an OLED55C32LA (webOS 23), installed with `ares-install` through
Developer Mode, no root anywhere:

```
[plane] foreign 'wl_webos_foreign' v1 -> 0x254e900
[plane] window id '_Window_Id_13' (type 0)
[video] ready (window id '_Window_Id_13')
[video] load: {"returnValue":true,"mediaId":"_0iQtqrBVnMnWDK"}
[video] ev {"resourceInfo": ... "VDEC" ... "ADEC" ...}
[video] load->loadCompleted 382ms
[plane] source 0,0 1920x1080 -> destination 0,0 1920x1080
```

`LSRegister` succeeds, `com.webos.media` answers, hardware decoders are
allocated and `currentTime` advances. A Developer Mode install is enough.

**If it is ever refused elsewhere**, the symptom is specific: the UI opens
normally and the last video line is `[video] LSRegister recusado`, with nothing
after it. That is distinct from the plane failing, which instead logs
`[plane] NO window id ...` or refuses the load with
`[video] no window id from the compositor`.

**How to check on your own TV:** install and read `/tmp/nuvio.log`.

---

## What you get on first launch

1. **QR login.** The account code is 32 hex digits, which is why it is a QR and
   not something you type. The session is stored and the refresh token renews
   itself: you sign in once.
2. **Profile selection**, if the account has more than one.
3. Addons, layout settings, TMDB key and watch progress come **from the
   account**.
4. **Trakt is linked on the TV itself**: Settings → Account → Trakt. The account
   does not store a Trakt credential (measured:
   `sync_pull_provider_credentials` returns tmdb, mdblist, animeskip, introdb and
   the debrid providers, never `trakt`). The Trakt code is 8 characters and easy
   to type on a phone.
5. **Simkl** can be linked too, but no screen in this app consumes Simkl yet —
   it exists so the credential reaches the account and from there the web app.

## What still bothers me

- **~24 MB** as the tree builds today, nearly all prebaked artwork — and it is
  the packager's: whoever installs it sees someone else's catalogue before
  signing in. Not a credential, but it does not belong there. (Older notes say
  175 MB, from a larger `art/`.)
- **50 h** sessions in Developer Mode. LG's limit, not the app's.
- The package uses the id `space.nuvio.native.legacy` so it can coexist with the
  web app (`space.nuvio.webos`) on the same TV. See PORT-LEGACY.md.
