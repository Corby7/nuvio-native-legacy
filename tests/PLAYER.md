# Verifying the native player

`bash tests/player.sh` checks source extraction and ordering, the absence of
fictitious sources, long URLs, episode selection and S/E persistence. It also
checks navigation with no skip buttons, films with no episode subtitle, menu
navigation/cancellation and the discarding of a stale episode name.
`bash tests/player.sh --visual` renders six SDL/OpenGL states and saves captures
to `/tmp/nuvio-player-*.bmp`. The captures use test data; they prove neither
video playback nor the availability of a track.

`bash tests/player.sh --live` queries the configured addons for Silo S2E4 and
the Trakt progress. It checks the Play button's target against `next_episode`.
It neither plays anything nor sends history marks. It does not print signed URLs.

The parser in isolation with sanitizers, without initialising SDL:

```sh
cc -fsanitize=address,undefined -g src/stream_parse.c src/js.c \
  tests/stream_parser.c -Isrc -I/opt/homebrew/include -o /tmp/nuvio-parser-tests
/tmp/nuvio-parser-tests
```

On 2026-09-02: 36 direct sources, seven advertised as DV, one MP4/DV; Trakt and
the Play button agreed on S2E4. The Mac and ARM builds both compiled.
ASan/UBSan passed on the isolated parser. The full SDL executable with ASan
aborted in the macOS 27 beta's SDL library initialiser before `main`; that does
not count as a memory test of the complete application.

Limits: checking real DV on the LG is still necessary. The Mac does not have the
webOS pipeline. The subtitle sheet keeps the options that are actually supported
by the backend, still without the language column and all the web app's controls.

Play uses an explicit selection, then a resume not yet finished, and then the
next episode Trakt reports. Local progress stores S/E in optional columns
compatible with older files. Completions above 90% use scrobble/stop; below that
they use pause. No test sends those marks.

## The subtitle size limit on the LG C9

A read-only inspection of the firmware on 2026-09-02: the five
`setSubtitleFontSize` indices map to 36, 46, 50, 56 and 70 in the renderer.
`setSubtitleCharacterFontSize` (`very_small` to `very_large`) uses the same
values: it adds no smaller sizes. A success response from the API does not prove
a visual change. Do not send negative indices or ones outside 0–4.
The two requested options below the minimum need another subtitle rendering
path; they remain outstanding, with no fictitious options in the menu.

The firmware also exposes `setSubtitleCharacterOpacity`; the sheet now offers
100%, 75%, 50% and 25%, persisted per device. The background's opacity gained
intermediate steps of 25% and 75%, plus an action to restore the default style.
The font/family was not exposed because the binary reveals only the field's name
(`charFont`), not the vocabulary it accepts; the window and presentation mode
were not presented as style because they add no proven visual control over the
video subtitle the app uses.

The episode name in Continue Watching comes from Trakt or from the episode list
already available; no query is made while the card is being drawn.
The new field changes `sizeof(CatItem)` and invalidates the old binary cache
through the existing size check; the first opening repopulates that cache.
