#!/bin/bash
# Prova que o .ipk NAO leva credencial de person — sem precisar de docker.
#
# Roda so o chunk de empacotamento do arm.sh (o mesmo palco, a mesma list de
# exclusao, a mesma conferencia) usando o nuvio-proto que ja esta em
# deploy/app. Existe porque o ciclo normal exige compilar para ARM, e ninguem
# vai run um build de 30s so para conferir uma list de arquivos.
#
# DUAS ARMADILHAS que este teste exists para nao deixar voltar:
#   1. `ares-package deploy/app` leva a folder INTEIRA, e art/ tem o token do
#      Trakt, as URLs de addon com key de debrid, a key do TMDB e a do
#      mdblist (mode 0600).
#   2. o .ipk e um pacote Debian: `tar tzf pacote.ipk` list SEM ERROR apenas
#      debian-binary / control.tar.gz / date.tar.gz. Uma conferencia escrita
#      assim passa sempre, inclusive com o segredo dentro.
#
# Conferido nos dois sentidos: com a exclusao, sai limpo; deixando trakt.txt
# enter de proposito, o teste ACUSA e devolve 1.
set -e
cd "$(dirname "$0")/.."
ARES="../NuvioWeb-0.3.38-beta/node_modules/.bin/ares-package"
ARQ_DE_PESSOA="trakt.txt addons.txt tmdb.txt mdblist.txt ajustes.txt settings.txt
               progress.txt nuvem.txt session.txt profile.txt client.txt"
PALCO=$(mktemp -d)
trap 'rm -rf "$PALCO"' EXIT
cp -R deploy/app "$PALCO/app"
rm -rf "$PALCO/app/art/cache"
for f in $ARQ_DE_PESSOA; do rm -f "$PALCO/app/art/$f"; done

SAIDA=$(mktemp -d)
"$ARES" "$PALCO/app" -o "$SAIDA" >/dev/null
IPK=$(ls -t "$SAIDA"/*.ipk | head -1)

LISTA=$(cd "$PALCO" && ar x "$IPK" 2>/dev/null && tar tzf date.tar.gz 2>/dev/null)
[ -z "$LISTA" ] && { echo "ABORTADO: nao consegui READ o pacote para conferir"; exit 1; }

echo "pacote de teste: $(du -h "$IPK" | cut -f1)"
echo "arquivos .txt dentro de art/:"
printf '%s\n' "$LISTA" | grep -E "art/.*\.txt$" | sed 's|.*/art/|  |' | sort
VAZOU=""
for f in $ARQ_DE_PESSOA; do
  printf '%s\n' "$LISTA" | grep -q "art/$f$" && VAZOU="$VAZOU $f"
done
rm -rf "$SAIDA"
if [ -n "$VAZOU" ]; then echo "FAILED: o pacote leva credencial ->$VAZOU"; exit 1; fi
echo "OK: nenhuma credencial no pacote"
