#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# Exercita o contrato de compilacao do escopo sem abrir SDL nem fazer rede.
cc -Wall -Wextra -Werror -fsyntax-only \
  src/ctxmenu.c src/catalog.c src/trakt.c \
  -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -Wno-deprecated-declarations -Wno-macro-redefined

# Regressao da intent: ela tem de ser capturada before do POST e o mesmo
# value tem de chegar ao espelho local quando a response confirmar.
linha_intencao=$(rg -n 'intent = !ci->naList;' src/ctxmenu.c | cut -d: -f1)
linha_post=$(rg -n 'trakt_watchlist_kind\(ci->imdb, ci->kind, intent\)' src/ctxmenu.c | cut -d: -f1)
linha_espelho=$(rg -n 'cat_set_na_list\(current, intent\)' src/ctxmenu.c | cut -d: -f1)
[ "$linha_intencao" -lt "$linha_post" ]
[ "$linha_post" -lt "$linha_espelho" ]

# Resposta nao nula sozinha nunca pode virar success: o contrato exige HTTP
# 2xx e usa a variante que devolve o status.
rg -q 'net_post_st\(url, 20, header, body, &status\)' src/trakt.c
rg -q 'confirmed = status >= 200 && status < 300' src/trakt.c

# A pressao longa continua pertencendo a home.c e chega ao modal via KEYUP.
rg -q 'home.c:.*NV_HOLD_MS.*KEYUP' src/ctxmenu.h

echo 'ctxmenu contract: PASS'
