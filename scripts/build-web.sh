#!/bin/bash
# Builds the browser version of DOSBox with the Wing Commander multiplayer
# layer and assembles a ready-to-serve page in web/dist:
#
#   scripts/build-web.sh            # everything: deps, configure, make, page
#   web/serve.py                    # then open http://localhost:8000/
#
# Needs: the Emscripten SDK (emcc on PATH, or EMSDK=/path/to/emsdk), cmake,
# curl, autotools, protoc, node/npm (for the lobbylink TypeScript client)
# and the lobbylink submodule (git submodule update --init).
#
# Environment:
#   WCDIR      directory with the game files (default: ./wc); only *.EXE,
#              *.CFG, *.BAT, *.COM, *.DAT, *.OVL and GAMEDAT/ are packaged
#   WCFILES    explicit list of files/dirs inside WCDIR to package instead
#   WEB_OUT    output directory (default: web/dist)
#   JOBS       parallel make jobs (default: nproc)
#   CONFIGURE_FLAGS  extra ./configure flags
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD_WEB_DIR:-$ROOT/build-web}
DEPS=$BUILD/deps
WCDIR=${WCDIR:-$ROOT/wc}
OUT=${WEB_OUT:-$ROOT/web/dist}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
# The generated protobuf code must match the runtime linked into the wasm,
# so protoc has to be this version too (built below if the system one differs).
PROTOBUF_VERSION=3.21.12

say() { echo "== $*"; }

# -- Emscripten ---------------------------------------------------------------
if ! command -v emcc >/dev/null 2>&1; then
    EMSDK=${EMSDK:-$HOME/emsdk}
    if [ -f "$EMSDK/emsdk_env.sh" ]; then
        # shellcheck disable=SC1091
        source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
    else
        echo "emcc not found: install the Emscripten SDK (https://emscripten.org/docs/getting_started/downloads.html)" >&2
        echo "and put it on PATH or set EMSDK=/path/to/emsdk" >&2
        exit 1
    fi
fi
say "using $(emcc --version | head -1)"

# -- protobuf for wasm (and a matching protoc) --------------------------------
mkdir -p "$DEPS"
tarball=protobuf-cpp-$PROTOBUF_VERSION.tar.gz
fetch_protobuf() {
    cd "$DEPS"
    [ -f "$tarball" ] || curl -sSL -o "$tarball" "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION#3.}/$tarball"
    [ -d "protobuf-$PROTOBUF_VERSION" ] || tar xzf "$tarball"
}
if [ ! -f "$DEPS/protobuf/lib/libprotobuf.a" ]; then
    say "building protobuf $PROTOBUF_VERSION for wasm"
    fetch_protobuf
    emcmake cmake -S "protobuf-$PROTOBUF_VERSION" -B protobuf-build -DCMAKE_BUILD_TYPE=Release \
        -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_PROTOC_BINARIES=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF \
        -Dprotobuf_WITH_ZLIB=OFF -DCMAKE_INSTALL_PREFIX="$DEPS/protobuf" > protobuf-cmake.log
    cmake --build protobuf-build -j"$JOBS" > protobuf-build.log
    cmake --install protobuf-build > protobuf-install.log
fi
PROTOC_FLAG=
if ! protoc --version 2>/dev/null | grep -q " $PROTOBUF_VERSION\$"; then
    if [ ! -x "$DEPS/protoc/bin/protoc" ]; then
        say "system protoc is not $PROTOBUF_VERSION; building a matching native protoc"
        fetch_protobuf
        cmake -S "protobuf-$PROTOBUF_VERSION" -B protoc-build -DCMAKE_BUILD_TYPE=Release \
            -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF -Dprotobuf_WITH_ZLIB=OFF \
            -DCMAKE_INSTALL_PREFIX="$DEPS/protoc" > protoc-cmake.log
        cmake --build protoc-build -j"$JOBS" --target protoc > protoc-build.log
        cmake --install protoc-build > protoc-install.log
    fi
    PROTOC_FLAG="--with-protoc=$DEPS/protoc/bin/protoc"
fi

# -- DOSBox -------------------------------------------------------------------
if [ ! -x "$ROOT/configure" ] || [ "$ROOT/configure.ac" -nt "$ROOT/configure" ]; then
    say "generating configure"
    (cd "$ROOT" && ./autogen.sh > "$BUILD/autogen.log" 2>&1)
fi
mkdir -p "$BUILD"
cd "$BUILD"
if [ ! -f config.status ] || [ "$ROOT/configure" -nt config.status ]; then
    say "configuring"
    # shellcheck disable=SC2086
    emconfigure "$ROOT/configure" --with-protobuf="$DEPS/protobuf" $PROTOC_FLAG \
        --enable-funarray=no --disable-dynamic-core --disable-fpu-x86 --disable-opengl \
        --disable-screenshots ${CONFIGURE_FLAGS:-} > configure.log
fi
say "building dosbox (make -j$JOBS)"
emmake make -j"$JOBS" > make.log 2>&1 || { tail -40 make.log; exit 1; }

# -- lobbylink browser client -------------------------------------------------
TS=$ROOT/lobbylink/clients/ts
if [ ! -f "$TS/src/index.ts" ]; then
    echo "lobbylink submodule missing: run git submodule update --init" >&2
    exit 1
fi
if [ ! -f "$TS/dist/index.js" ] || [ "$TS/src/index.ts" -nt "$TS/dist/index.js" ]; then
    say "building the lobbylink TypeScript client"
    (cd "$TS" && { [ -d node_modules ] || npm ci --silent; } && npx tsc -p .)
fi

# -- the page -----------------------------------------------------------------
say "assembling $OUT"
mkdir -p "$OUT"
cp "$BUILD/src/dosbox.js" "$BUILD/src/dosbox.wasm" "$OUT/"
cp "$TS/dist/index.js" "$OUT/p2p-client.js"
cp "$ROOT/web/index.html" "$ROOT/web/wc.js" "$OUT/"

if [ -d "$WCDIR" ]; then
    if [ -n "${WCFILES:-}" ]; then
        # shellcheck disable=SC2206
        files=($WCFILES)
    else
        files=()
        while IFS= read -r f; do files+=("${f#./}"); done < <(
            cd "$WCDIR" && find . -maxdepth 1 -type f \( -iname '*.exe' -o -iname '*.cfg' -o -iname '*.bat' \
                -o -iname '*.com' -o -iname '*.dat' -o -iname '*.ovl' \) | sort)
        [ -d "$WCDIR/GAMEDAT" ] && files+=(GAMEDAT)
    fi
    if [ ${#files[@]} -eq 0 ]; then
        echo "no game files found in $WCDIR" >&2
        exit 1
    fi
    say "packaging ${#files[@]} entries from $WCDIR"
    tar -C "$WCDIR" --format=ustar -czf "$OUT/wc.tar.gz" "${files[@]}"
else
    echo "no game directory at $WCDIR (set WCDIR); web/dist has no wc.tar.gz" >&2
fi
ls -la "$OUT"
say "done. Serve it with: $ROOT/web/serve.py"
