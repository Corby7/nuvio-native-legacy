#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
cc tests/director_cache.c src/js.c -Isrc -o /tmp/nuvio-director-tests -O1 -g -pthread
/tmp/nuvio-director-tests
