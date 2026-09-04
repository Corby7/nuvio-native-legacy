#!/bin/bash
# Compila para ARM e instala na TV por COPIA DIRETA. Ciclo complete (~30s).
#
#   bash tools/arm.sh            # compiles, copy, checks e lanca
#   bash tools/arm.sh --build    # so compiles
#   bash tools/arm.sh --ipk      # tambem gera o .ipk (para distribuir)
#
# NAO usa o appInstallService, e a razao esta escrita no dot do envio: ele
# responde success e nao swap o binario.
#
# A imagem vem de tools/Dockerfile:
#   docker build --platform linux/arm64 -t nuvio-webos-sdk tools/
#
# -ldl e obrigatorio: video.c abre libluna-service2 por dlopen. A line de
# compilacao do runbook old nao tinha, e o link falhava em 'dlsym@@GLIBC_2.4'.
#
# NAO acrescente -lcurl. net.c abre libcurl por dlopen em execucao, tentando
# so.5 e after so.4. Linkar curl cria dependencia rigida na libcurl.so.4 do
# SDK, e a TV so tem /usr/lib/libcurl.so.5 — o binario nem inicia, com
# "libcurl.so.4 => not found" e nenhuma message visible na screen.
set -e
cd "$(dirname "$0")/.."

TV_IP="${NUVIO_TV_IP:-192.168.1.32}"
TV_PASS="${NUVIO_TV_PASS:-alpine}"
APP_ID="space.nuvio.native.legacy"
ARES="../NuvioWeb-0.3.38-beta/node_modules/.bin/ares-package"

echo "==> compilando para ARM"
# A configuracao do servidor enters por VARIAVEL DE AMBIENTE e os -D sao montados
# dentro do container. Passa-los na line de `sh -c` exigiria aspas dentro de
# aspas e o error seria MUDO: a macro chega empty, o binario compiles, instala,
# abre — e a screen de login diz "pacote montado sem servidor". Foi o que
# aconteceu no first deploy desta funcionalidade.
# Uma limpeza SO, com all dentro: `trap` nao acumula — um second `trap ... EXIT`
# substitui o first, e o file com a key anon ficaria para tras em
# /tmp toda vez que o mode --ipk fosse used.
LIXO=""
clear() { [ -n "$LIXO" ] && rm -rf $LIXO; }
trap clear EXIT
ENVF=$(mktemp); LIXO="$LIXO $ENVF"
tools/env.sh --env-file "$ENVF"
docker run --rm --platform linux/arm64 --env-file "$ENVF" \
  -v "$PWD":/work nuvio-webos-sdk sh -c '
  SR=$NUVIO_SYSROOT
  arm-webos-linux-gnueabi-gcc src/*.c -o nuvio-proto.arm -O2 \
    -DNV_SUPABASE_URL="\"$NV_SUPABASE_URL\"" \
    -DNV_SUPABASE_ANON_KEY="\"$NV_SUPABASE_ANON_KEY\"" \
    -DNV_TV_LOGIN_BASE="\"$NV_TV_LOGIN_BASE\"" \
    -DNV_TRAKT_CLIENT_ID="\"$NV_TRAKT_CLIENT_ID\"" \
    -DNV_TRAKT_CLIENT_SECRET="\"$NV_TRAKT_CLIENT_SECRET\"" \
    -DNV_SIMKL_CLIENT_ID="\"$NV_SIMKL_CLIENT_ID\"" \
    -DNV_SIMKL_APP="\"$NV_SIMKL_APP\"" \
    -I$SR/usr/include -I$SR/usr/include/SDL2 \
    -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -lEGL -ldl -lpthread -lm'

# CONFERE que a configuracao entrou MESMO no binario. Sem isto o unico sintoma
# e a screen de login dizendo que o pacote saiu sem servidor, ja na TV.
# A conferencia checa CADA key, lendo os VALORES DO ENV-FILE.
#
# Ela ja leu do ambiente do SHELL, e isso a tornava inutil sem parecer: as
# variaveis so existem DENTRO do container (o --env-file as injeta la), entao
# no host all davam vazias, cada uma caia no "warning: vazio" e a checagem era
# PULADA. Uma guarda que se auto-desliga e worst que guarda nenhuma, porque
# tranquiliza.
while IFS='=' read -r NOME VALOR; do
  [ -z "$NOME" ] && continue
  if [ -z "$VALOR" ]; then
    echo "    warning: $NOME vazio em local.properties"
    continue
  fi
  if ! strings nuvio-proto.arm 2>/dev/null | grep -qF "$VALOR"; then
    echo "    ABORTADO: $NOME nao entrou no binario ARM"
    exit 1
  fi
done < "$ENVF"

cp nuvio-proto.arm deploy/app/nuvio-proto
rm -f ./*.ipk

# O .ipk so interessa para DISTRIBUIR (instalar em outra TV, publicar). O ciclo
# de desenvolvimento nao passa por ele — see a score abaixo.
#
# EMPACOTA DE UMA COPIA LIMPA, nunca de deploy/app direto. Motivo concreto:
# `ares-package deploy/app` leva a folder INTEIRA, e art/ tem credencial de
# PESSOA — o token do Trakt, as URLs de addon com a key do debrid embutida, a
# key do TMDB e a do mdblist (esta ate com mode 0600, de tao secreta). Um
# .ipk gerado assim entrega all isso para quem instalar. Ate a version com
# login isso nao tinha como ser diferente, porque o app dependia dos arquivos;
# now ele nao depende mais, e resume embarcando-os seria so descuido.
#
# ajustes.txt settings.txt sai pelo mesmo motivo, com dano smaller: e a preferencia de LAYOUT
# de quem montou, e ela chegaria como se fosse a de quem instalou.
ARQ_DE_PESSOA="trakt.txt addons.txt tmdb.txt mdblist.txt ajustes.txt settings.txt
               progress.txt nuvem.txt session.txt profile.txt client.txt"

if [ "$1" = "--ipk" ]; then
  echo "==> empacotando (sem credenciais)"
  PALCO=$(mktemp -d); LIXO="$LIXO $PALCO"
  cp -R deploy/app "$PALCO/app"
  # cache/ e cache de EXECUCAO, nao art do pacote: sao megabytes de imagem
  # baixada que o app rebaixa sozinho.
  rm -rf "$PALCO/app/art/cache"
  for f in $ARQ_DE_PESSOA; do rm -f "$PALCO/app/art/$f"; done

  "$ARES" "$PALCO/app" -o .
  IPK=$(ls -t ./*.ipk | head -1)

  # CONFERE o pacote READY, e nao a folder de onde ele saiu. A list de
  # exclusao acima e uma intent; o teste abaixo e o fato.
  #
  # ARMADILHA MEDIDA: o .ipk e um pacote Debian (file `ar` com
  # debian-binary + control.tar.gz + date.tar.gz). `tar tzf pacote.ipk` LISTA,
  # sem error nenhum, apenas esses tres names — nunca os arquivos do app. Uma
  # conferencia escrita assim passa sempre, inclusive quando o segredo esta
  # dentro. Tem de desempacotar o `ar` e listar o date.tar.gz.
  LISTA=$(cd "$PALCO" && ar x "$OLDPWD/$IPK" 2>/dev/null && tar tzf date.tar.gz 2>/dev/null)
  if [ -z "$LISTA" ]; then
    echo "    ABORTADO: nao consegui READ o pacote para conferir; nao vou dizer que esta limpo"
    rm -f "$IPK"
    exit 1
  fi
  VAZOU=""
  for f in $ARQ_DE_PESSOA; do
    printf '%s\n' "$LISTA" | grep -q "art/$f$" && VAZOU="$VAZOU $f"
  done
  if [ -n "$VAZOU" ]; then
    echo "    ABORTADO: o pacote leva credencial ->$VAZOU"
    rm -f "$IPK"
    exit 1
  fi
  echo "    $IPK ($(du -h "$IPK" | cut -f1)) — sem art/{$(echo $ARQ_DE_PESSOA | tr ' ' ',')}"
fi

[ "$1" = "--build" ] && exit 0

# A TV ESTA ROOTEADA: copy direta, sem passar pelo instalador.
#
# O appInstallService responde `"returnValue": true` e `statusValue: 264`
# (instalado) e NAO SUBSTITUI O BINARIO — escreve art/, deixa nuvio-proto e
# appinfo.json intactos. Perdi tres deploys achando que tinha subido: o app na
# TV ficou 2h30 running uma version antiga enquanto o log dizia success.
#
# Com root nao ha motivo para o intermediario. scp + mv + chmod faz o mesmo em
# dois seconds, e o md5 no end PROVA que rose — a licao real e essa: deploy
# sem verificacao e torcida, nao entrega.
#
# ares-install tambem nao serve here: ele waits prisoner@<ip>:9922 do Developer
# Mode, e esta TV nao roda o Developer Mode — e root na 22 com senha alpine.
APPDIR=/media/developer/apps/usr/palm/applications/$APP_ID
SSH="sshpass -p $TV_PASS ssh -o StrictHostKeyChecking=no"
SCP="sshpass -p $TV_PASS scp -o StrictHostKeyChecking=no -q"

# O DIRETORIO PODE NAO EXISTIR: o owner pode ter desinstalado o app pela TV, e
# ai todo scp abaixo failure com "No such file or directory" — que foi exatamente
# o que aconteceu. Criar before torna o deploy capaz de REINSTALAR, nao so
# atualizar.
$SSH "root@$TV_IP" "mkdir -p $APPDIR"

echo "==> enviando binario para $TV_IP"
$SCP nuvio-proto.arm "root@$TV_IP:$APPDIR/nuvio-proto.new"
# Renomear em vez de sobrescrever: se o app estiver running, o executavel esta
# mapeado e a escrita direta failure com ETXTBSY. O rename swap o inode.
$SSH "root@$TV_IP" "cd $APPDIR && mv -f nuvio-proto.new nuvio-proto && chmod 755 nuvio-proto"

# A ARTE muda pouco, mas quando muda (icon new, source) tem de ir junto.
# CARIMBO DE BUILD NO TITULO.
#
# "ainda e build antiga" nao tem como ser respondido olhando a screen: o md5 do
# binario prova o que esta no DISCO, nao o que foi LANCADO, e a TV tem dois apps
# Nuvio (este e o web "Nuvio TV") — abrir o tile errado da exatamente o mesmo
# sintoma. Com os 8 primeiros digitos do md5 no title, o launcher responde
# sozinho which build esta ali.
echo "==> carimbando title com a build"
STAMP=$(md5 -q nuvio-proto.arm 2>/dev/null || md5sum nuvio-proto.arm | cut -d' ' -f1)
STAMP=${STAMP:0:8}
sed "s/(BUILD)/($STAMP)/" deploy/app/appinfo.json > /tmp/appinfo.stamped.json
cp /tmp/appinfo.stamped.json deploy/app/appinfo.json.stamped

$SCP /tmp/appinfo.stamped.json "root@$TV_IP:$APPDIR/appinfo.json"

echo "==> sincronizando art"
# --exclude art/cache: e CACHE DE EXECUCAO, nao art do pacote. Com ele o tar
# passava de 49 MB (27 MB so de posteres baixados no Mac) e a extracao no
# busybox da TV morria no meio — e o que vinha DEPOIS de "cache/" na order
# alfabetica, "marcas/" inclusive, sumia em silencio. Foi assim que o wordmark
# do Trakt "rose" tres vezes sem nunca chegar la.
#
# Sem o cache o tar cai para poucos MB. A TV reconstroi o dela sozinha, e nao
# herda mais o uid do Mac — a mesma armadilha que o chown abaixo remedia.
#
# 2>&1 e nao 2>/dev/null: error de tar escondido foi exatamente o que fez o
# deploy mentir. A licao ja estava escrita here para o binario (o md5 no end) e
# a art tinha ficado de fora dela.
# O filter do ruido de xattr fica no MAC: a sh da TV e busybox ash e nao tem
# PIPESTATUS — tentar usar la dava "bad substitution" e derrubava o deploy.
#
# E o status vem de um `set -o pipefail` local em torno do pipe, nao de indices
# de PIPESTATUS, que ficam ambiguos assim que se acrescenta um `|| true`.
if ! ( set -o pipefail
       tar czf - -C deploy/app --exclude 'art/cache' \
           --exclude 'appinfo.json.stamped' \
           art fonts icon.png \
         | $SSH "root@$TV_IP" "tar xzf - -C $APPDIR" ) 2>&1 \
     | grep -v 'unknown extended header keyword'; then
  :
fi
if ! $SSH "root@$TV_IP" "test -f $APPDIR/art/brands/trakt.png"; then
  echo "    FAILED: a art nao chegou na TV"; exit 1
fi
rm -f deploy/app/appinfo.json.stamped
# O `core` de um crash old fica no diretorio do app e pesa 118 MB numa
# particao de 4,2 G. Nao serve para nada after que o relatorio foi gerado.
$SSH "root@$TV_IP" "rm -f $APPDIR/core; rm -f /tmp/nuvio-shot-req /tmp/nuvio-shot.bmp" || true

# DONO DA PASTA DE CACHE. O tar e feito no Mac e extraido como ROOT na TV, entao
# art/cache herda o uid do Mac (13888160) com mode 755. O app roda como uid 5152
# e NAO CONSEGUE GRAVAR ali: toda imagem baixada era descartada em silencio, e os
# dois threads de decode ficavam rebaixando o que nunca poderia ser guardado.
#
# MEDIDO: 91 "decode failed" no log com ZERO error de rede. Depois do chown, 15 —
# e as texturas subiram de 87 para 99. Sem esta line o defeito volta a cada
# deploy, e o sintoma e "poster que nao aparece", que ja custou meia session.
$SSH "root@$TV_IP" "mkdir -p $APPDIR/art/cache && chown -R 5152:5000 $APPDIR/art && chmod -R u+rwX $APPDIR/art"

echo "==> conferindo"
LOCAL=$(md5 -q nuvio-proto.arm 2>/dev/null || md5sum nuvio-proto.arm | cut -d' ' -f1)
REMOTO=$($SSH "root@$TV_IP" "md5sum $APPDIR/nuvio-proto | cut -d' ' -f1" 2>/dev/null | tr -d '\r')
if [ "$LOCAL" != "$REMOTO" ]; then
  echo "    FAILED: local $LOCAL != TV $REMOTO"
  exit 1
fi
echo "    ok ($LOCAL)"

echo "==> lancando"
( sleep 2
  printf 'luna-send -n 1 -f luna://com.webos.applicationManager/launch '"'"'{"id":"%s"}'"'"'\n' "$APP_ID"
  sleep 3
  printf 'exit\n'
) | nc -w20 "$TV_IP" 23 | tr -d '\0' | grep returnValue
