#!/bin/sh
# Builds web/out/ontodb.{js,wasm}. Needs emsdk (set EMSDK to its directory;
# default ../../emsdk). `web/build.sh` then `node web/smoke.mjs` to check it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
EMSDK=${EMSDK:-$here/../../emsdk}
. "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
mkdir -p "$here/build" "$here/out"
cd "$here/build"
emcmake cmake "$here" -DCMAKE_BUILD_TYPE=Release >/dev/null
emmake make -j8 2>&1 | grep -E "error|Error|warning: unused|Linking|Built" || true
cp ontodb_wasm.js ontodb_wasm.wasm "$here/out/"
ls -l "$here/out"
