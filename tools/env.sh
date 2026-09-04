#!/bin/bash
# Reads local.properties (the SAME file as the web app) and prints the -D flags
# the build needs. No secret goes into a source file: the value travels from the
# property straight to the compiler's command line.
#
# The Supabase anon key is public per project — it is what the RLS expects to
# receive, and the web app already publishes it in its bundle. What must NOT go
# into the package is what lives in art/trakt.txt and art/addons.txt today: that
# is a PERSON's credential.
#
#   eval "cc src/*.c $(tools/env.sh) ..."
set -e

# The sibling web checkout has been called both NuvioWeb-0.3.38-beta and
# NuvioWeb. Try each in turn rather than hard-coding one: getting this wrong
# fails SILENTLY, and the only symptom is a login screen with no server.
if [ -z "${NUVIO_PROPERTIES:-}" ]; then
  PARENT=$(cd "$(dirname "$0")/../.." && pwd)
  for candidate in "$PARENT/NuvioWeb-0.3.38-beta" "$PARENT/NuvioWeb"; do
    if [ -f "$candidate/local.properties" ]; then
      NUVIO_PROPERTIES="$candidate/local.properties"
      break
    fi
  done
fi
PROP="${NUVIO_PROPERTIES:-}"

# The same setting has two spellings across the checkouts (NUVIO_SUPABASE_URL in
# the older one, SUPABASE_URL in the newer). Accept either: a missing value here
# produces a build that runs and cannot log in, which is the hardest kind of
# failure to attribute.
value() {
  [ -f "$PROP" ] || return 0
  for key in "$@"; do
    v=$(sed -n "s/^$key=//p" "$PROP" | head -1 | tr -d '\r')
    if [ -n "$v" ]; then printf '%s' "$v"; return 0; fi
  done
  return 0
}

URL=$(value NUVIO_SUPABASE_URL SUPABASE_URL)
KEY=$(value NUVIO_SUPABASE_ANON_KEY SUPABASE_ANON_KEY)
TVB=$(value TV_LOGIN_WEB_BASE_URL TV_LOGIN_REDIRECT_BASE_URL)
TRK=$(value TRAKT_CLIENT_ID)
TRS=$(value TRAKT_CLIENT_SECRET)
SMK=$(value SIMKL_CLIENT_ID)
SMA=$(value SIMKL_APP_NAME)

if [ -z "$URL" ] || [ -z "$KEY" ]; then
  # Failing silently would produce an .ipk that opens, shows the login screen
  # and never leaves it. The warning goes to stderr so it does not pollute the
  # -D flags on stdout.
  echo "env.sh: no SUPABASE URL/ANON_KEY in ${PROP:-<no local.properties found>} -- the app will build WITHOUT login" >&2
fi

# --env-file: writes the variables to a file for `docker run --env-file`.
# It exists because the ARM build runs INSIDE a container: passing the -D flags
# on the command line would need quotes inside quotes inside `sh -c`, and the
# failure there is silent — the compiler receives an empty macro and the app
# ships WITHOUT LOGIN, which is exactly what happened on the first deploy to the
# TV.
if [ "$1" = "--env-file" ]; then
  [ -n "$2" ] || { echo "env.sh --env-file needs a path" >&2; exit 2; }
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
