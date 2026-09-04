#!/bin/bash
# Compila e roda no Mac, com a mesma folder de art do pacote da TV.
#
# FICA LOGADO ENTRE EXECUCOES. A session mora em $NUVIO_DATA, que here aponta
# para ~/.nuvio: logue UMA vez (escaneando o QR da screen) e todo arranque
# seguinte abre com a account dentro, porque o refresh token se renova sozinho.
# CONFERIDO: 1o arranque "[session] logado como ..."; reiniciando sem escanear
# nada, "[session] session restaurada ..." e "[addons] N vindos da account".
#
# Para testar como um user NOVO (a first execucao de quem instala),
# aponte a variavel para uma folder empty:
#     NUVIO_DATA=/tmp/nuvio-new bash tools/mac.sh
#
# "Sair da account", em Ajustes, apaga ~/.nuvio inteiro (session, ajustes e
# progress) — after disso e preciso escanear de new.
set -e
cd "$(dirname "$0")/.."
# Os -D do servidor saem do MESMO local.properties do app web (tools/env.sh).
# Sem eles o app compiles, instala e abre — e o unico sintoma e a screen de login
# dizendo "Este pacote foi montado sem servidor". Nenhum desses values enters em
# file de code versionado.
ENV_D=$(tools/env.sh)
# Fora da folder do pacote: o Mac nao deve gravar session dentro de deploy/.
export NUVIO_DATA="${NUVIO_DATA:-$HOME/.nuvio}"
eval cc src/*.c -o /tmp/nuvio-native-legacy-mac -O1 -g "$ENV_D" \
  -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -L/opt/homebrew/lib -lSDL2 -lSDL2_image -lSDL2_ttf \
  -framework OpenGL -Wno-deprecated-declarations
exec /tmp/nuvio-native-legacy-mac "$(pwd)/deploy/app/art" "$@"
