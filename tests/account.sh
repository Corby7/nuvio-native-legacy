#!/bin/bash
# Testes da account. PRECISAM DE REDE: falam com o servidor de verdade.
#
#   bash tests/account.sh
#
# O de ajustes nao precisa de rede. O de logout abre uma session ANONIMA e a grava como se fosse de user. Nao e
# gambiarra de teste: o user anonimo tem account real no servidor (o proprio
# app web faz isso before de pedir o code de login), com addons semeados. E o
# unico jeito de exercitar o ciclo inteiro sem alguem autorizando no celular.
set -e
cd "$(dirname "$0")/.."
ENV_D=$(tools/env.sh)
FONTES=$(ls src/*.c | grep -v 'src/main.c' | tr '\n' ' ')
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

echo "==> logout apaga o user previous"
eval cc tests/account_logout.c $FONTES -o "$TMP/logout" "$ENV_D" -Isrc \
  -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -L/opt/homebrew/lib -lSDL2 -lSDL2_image -lSDL2_ttf \
  -framework OpenGL -Wno-deprecated-declarations -Wno-macro-redefined
NUVIO_DATA="$TMP/data" "$TMP/logout"

echo
echo "==> ajustes da account chegam com o sentido certo"
eval cc tests/account_settings.c $FONTES -o "$TMP/ajustes" "$ENV_D" -Isrc \
  -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -L/opt/homebrew/lib -lSDL2 -lSDL2_image -lSDL2_ttf \
  -framework OpenGL -Wno-deprecated-declarations -Wno-macro-redefined
"$TMP/ajustes"

echo
echo "==> QR legivel por leitor de verdade"
cc tools/qr_dump.c src/qr.c -o "$TMP/qr" -Isrc
PY=${NUVIO_PY:-/tmp/qrvenv/bin/python}
if [ -x "$PY" ]; then "$PY" tools/qr_check.py "$TMP/qr"
else echo "   PULADO: sem venv com opencv (see o cabecalho de tools/qr_check.py)"; fi
