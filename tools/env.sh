#!/bin/bash
# Le local.properties (o MESMO file do app web) e imprime os -D que a
# compilacao precisa. Nada de segredo enters em file de code: o value viaja
# da propriedade direto para a line de comando do compilador.
#
# A anon key do Supabase e publica por projeto — e o que o RLS waits receive, e
# o app web ja a publica no bundle. Quem NAO pode enter no pacote e o que hoje
# esta em art/trakt.txt e art/addons.txt: aquilo e credencial de PESSOA.
#
#   eval "cc src/*.c $(tools/env.sh) ..."
set -e

PROP="${NUVIO_PROPERTIES:-$(cd "$(dirname "$0")/../.." && pwd)/NuvioWeb-0.3.38-beta/local.properties}"

value() {
  [ -f "$PROP" ] || return 0
  sed -n "s/^$1=//p" "$PROP" | head -1 | tr -d '\r'
}

URL=$(value NUVIO_SUPABASE_URL)
KEY=$(value NUVIO_SUPABASE_ANON_KEY)
TVB=$(value TV_LOGIN_WEB_BASE_URL)
TRK=$(value TRAKT_CLIENT_ID)
TRS=$(value TRAKT_CLIENT_SECRET)
SMK=$(value SIMKL_CLIENT_ID)
SMA=$(value SIMKL_APP_NAME)

if [ -z "$URL" ] || [ -z "$KEY" ]; then
  # Falhar em silencio produziria um .ipk que abre, mostra a screen de login e
  # nunca sai dela. O warning vai para stderr para nao sujar os -D no stdout.
  echo "env.sh: sem NUVIO_SUPABASE_URL/ANON_KEY em $PROP -- o app vai compilar SEM login" >&2
fi

# --env-file: escreve as variaveis num file para o `docker run --env-file`.
# Existe porque a compilacao ARM roda DENTRO de um container: passar os -D na
# line de comando exigiria aspas dentro de aspas dentro de `sh -c`, e o error
# ali e mudo — o compilador recebe a macro empty e o app sai SEM LOGIN, que foi
# exatamente o que aconteceu no first deploy para a TV.
if [ "$1" = "--env-file" ]; then
  [ -n "$2" ] || { echo "env.sh --env-file precisa do path" >&2; exit 2; }
  {
    printf 'NV_SUPABASE_URL=%s\n' "$URL"
    printf 'NV_SUPABASE_ANON_KEY=%s\n' "$KEY"
    printf 'NV_TV_LOGIN_BASE=%s\n' "$TVB"
    printf 'NV_TRAKT_CLIENT_ID=%s\n' "$TRK"
    printf 'NV_TRAKT_CLIENT_SECRET=%s\n' "$TRS"
    printf 'NV_SIMKL_CLIENT_ID=%s\n' "$SMK"
    printf 'NV_SIMKL_APP=%s\n' "$SMA"
  } > "$2"
  chmod 600 "$2"
  exit 0
fi

printf -- '-DNV_SUPABASE_URL=\\"%s\\" -DNV_SUPABASE_ANON_KEY=\\"%s\\" -DNV_TV_LOGIN_BASE=\\"%s\\" -DNV_TRAKT_CLIENT_ID=\\"%s\\" -DNV_TRAKT_CLIENT_SECRET=\\"%s\\" -DNV_SIMKL_CLIENT_ID=\\"%s\\" -DNV_SIMKL_APP=\\"%s\\"' \
  "$URL" "$KEY" "$TVB" "$TRK" "$TRS" "$SMK" "$SMA"
