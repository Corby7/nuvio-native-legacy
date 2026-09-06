addons.txt holds the owner's addon URLs, and THEY EMBED API KEYS in the path
itself (AIOStreams, Debridio, Xperience). Treat it as a secret: do not commit
it, do not publish it, do not paste it into a transcript.

It was extracted from the web app's localStorage on the TV:
  /var/lib/wam/Default/Local Storage/file_space.nuvio.webos_0.localstorage
  (SQLite; keys installedAddonUrls / DisplayNames / EnabledStates,
   in the format {"profiles":{"1":[...]}})
TAB is the separator on purpose: an addon name can contain "|" ("AIOStreams | ElfHosted").
