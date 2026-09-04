#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# Exercises the compile contract of this scope without opening SDL or hitting the network.
cc -Wall -Wextra -Werror -fsyntax-only \
  src/ctxmenu.c src/catalog.c src/trakt.c \
  -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -Wno-deprecated-declarations -Wno-macro-redefined

# Intent regression: it has to be captured BEFORE the POST, and the same value
# has to reach the local mirror once the response confirms.
line_intent=$(rg -n 'intent = !ci->inList;' src/ctxmenu.c | cut -d: -f1)
line_post=$(rg -n 'trakt_watchlist_kind\(ci->imdb, ci->kind, intent\)' src/ctxmenu.c | cut -d: -f1)
line_mirror=$(rg -n 'cat_set_in_list\(current, intent\)' src/ctxmenu.c | cut -d: -f1)
[ "$line_intent" -lt "$line_post" ]
[ "$line_post" -lt "$line_mirror" ]

# A non-null response alone can never become success: the contract requires
# HTTP 2xx and uses the variant that returns the status.
rg -q 'net_post_st\(url, 20, header, body, &status\)' src/trakt.c
rg -q 'confirmed = status >= 200 && status < 300' src/trakt.c

# The long press still belongs to home.c and reaches the modal via KEYUP.
rg -q 'home.c:.*NV_HOLD_MS.*KEYUP' src/ctxmenu.h

echo 'ctxmenu contract: PASS'
