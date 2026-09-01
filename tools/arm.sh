#!/bin/bash
# Compila para ARM, empacota e instala na TV. Ciclo completo (~40s).
#
#   bash tools/arm.sh            # compila, empacota, instala e lanca
#   bash tools/arm.sh --build    # so compila e empacota
#
# A imagem vem de tools/Dockerfile:
#   docker build --platform linux/arm64 -t nuvio-webos-sdk tools/
#
# -ldl e obrigatorio: video.c abre libluna-service2 por dlopen. A linha de
# compilacao do runbook antigo nao tinha, e o link falhava em 'dlsym@@GLIBC_2.4'.
#
# NAO acrescente -lcurl. rede.c abre libcurl por dlopen em execucao, tentando
# so.5 e depois so.4. Linkar curl cria dependencia rigida na libcurl.so.4 do
# SDK, e a TV so tem /usr/lib/libcurl.so.5 — o binario nem inicia, com
# "libcurl.so.4 => not found" e nenhuma mensagem visivel na tela.
set -e
cd "$(dirname "$0")/.."

TV_IP="${NUVIO_TV_IP:-192.168.1.32}"
TV_PASS="${NUVIO_TV_PASS:-alpine}"
APP_ID="space.nuvio.native.legacy"
ARES="../NuvioWeb-0.3.38-beta/node_modules/.bin/ares-package"

echo "==> compilando para ARM"
docker run --rm --platform linux/arm64 -v "$PWD":/work nuvio-webos-sdk sh -c '
  SR=$NUVIO_SYSROOT
  arm-webos-linux-gnueabi-gcc src/*.c -o nuvio-proto.arm -O2 \
    -I$SR/usr/include -I$SR/usr/include/SDL2 \
    -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -lEGL -ldl -lpthread -lm'

cp nuvio-proto.arm deploy/app/nuvio-proto
rm -f ./*.ipk

echo "==> empacotando"
"$ARES" deploy/app -o .
IPK=$(ls -t ./*.ipk | head -1)
echo "    $IPK ($(du -h "$IPK" | cut -f1))"

[ "$1" = "--build" ] && exit 0

echo "==> enviando para $TV_IP"
sshpass -p "$TV_PASS" scp -o StrictHostKeyChecking=no -P 22 "$IPK" "root@$TV_IP:/tmp/"

# O telnet (porta 23, sem senha) precisa de sleep ENTRE os comandos: sem eles o
# shell nao chega a executar. A saida vem com NUL, dai o `tr -d`.
REMOTE_IPK="/tmp/$(basename "$IPK")"
echo "==> instalando e lancando"
( sleep 3
  printf 'luna-send -n 1 -f luna://com.webos.appInstallService/dev/install '"'"'{"id":"%s","ipkUrl":"%s","subscribe":false}'"'"'\n' "$APP_ID" "$REMOTE_IPK"
  sleep 14
  printf 'luna-send -n 1 -f luna://com.webos.applicationManager/launch '"'"'{"id":"%s"}'"'"'\n' "$APP_ID"
  sleep 4
  printf 'exit\n'
) | nc -w30 "$TV_IP" 23 | tr -d '\0' | tail -5
