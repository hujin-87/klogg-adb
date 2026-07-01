#!/usr/bin/env bash
#
# Build a portable Windows x64 package of klogg on Linux via Docker + MinGW.
# Run from the repo root:  ./docker/mxe-win/build-win.sh
#
# Output: build_win/klogg-win64/  and  build_win/klogg-win64.zip
#
set -euo pipefail

IMAGE=klogg-win:fedora
CONTAINER=klogg-build
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DOCKERDIR="$REPO/docker/mxe-win"

echo "==> [1/5] Ensure toolchain image ($IMAGE)"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -f "$DOCKERDIR/Dockerfile.fedora" -t "$IMAGE" "$DOCKERDIR"
fi

echo "==> [2/5] Ensure build container ($CONTAINER)"
if ! docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER"; then
  docker run -d --name "$CONTAINER" -v "$REPO":/src "$IMAGE" sleep infinity
fi
docker start "$CONTAINER" >/dev/null 2>&1 || true
docker exec "$CONTAINER" git config --global --add safe.directory '*'

CONFIGURE='mingw64-cmake -S /src -B /src/build_win -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKLOGG_USE_HYPERSCAN=OFF -DKLOGG_USE_VECTORSCAN=OFF \
  -DKLOGG_BUILD_TESTS=OFF -DKLOGG_USE_LTO=OFF -DKLOGG_USE_SENTRY=OFF \
  -DKLOGG_GENERIC_CPU=ON \
  -DWARNINGS_AS_ERRORS=OFF \
  -DCMAKE_C_FLAGS="-include intrin.h" \
  -DCMAKE_CXX_FLAGS="-Wa,-mbig-obj" \
  -DTBBMALLOC_PROXY_BUILD=OFF \
  -DKLOGG_HOST_MADDY=/usr/local/bin/maddy-host'

echo "==> [3/5] Ensure host-native maddy (documentation tool)"
docker exec "$CONTAINER" bash -lc "
  set -e
  # First configure fetches CPM sources (incl. maddy) if not present.
  if [ ! -f /src/build_win/_deps/maddy-src/main.cpp ]; then
    $CONFIGURE >/tmp/cfg0.log 2>&1 || true
  fi
  if [ ! -x /usr/local/bin/maddy-host ]; then
    g++ -O2 -std=c++17 -I/src/build_win/_deps/maddy-src/include \
        /src/build_win/_deps/maddy-src/main.cpp -o /usr/local/bin/maddy-host
  fi
"

echo "==> [4/5] Configure + build"
docker exec "$CONTAINER" bash -lc "cd /src && $CONFIGURE && ninja -C /src/build_win"

echo "==> [5/5] Package exe + Qt DLLs + plugins"
docker exec "$CONTAINER" bash -lc '
set -e
MINGW=/usr/x86_64-w64-mingw32/sys-root/mingw
BIN=$MINGW/bin
DIST=/src/build_win/klogg-win64
OBJDUMP=x86_64-w64-mingw32-objdump

rm -rf "$DIST" && mkdir -p "$DIST" "$DIST/platforms" "$DIST/styles" "$DIST/imageformats" "$DIST/iconengines"
for e in klogg.exe klogg_portable.exe klogg_grep.exe; do
  cp "/src/build_win/output/$e" "$DIST/" && x86_64-w64-mingw32-strip "$DIST/$e"
done

queue="$(ls "$DIST"/*.exe)"
for p in platforms/qwindows.dll platforms/qminimal.dll \
         styles/qwindowsvistastyle.dll \
         imageformats/qico.dll imageformats/qjpeg.dll imageformats/qgif.dll imageformats/qsvg.dll \
         iconengines/qsvgicon.dll; do
  [ -f "$MINGW/lib/qt5/plugins/$p" ] && { cp "$MINGW/lib/qt5/plugins/$p" "$DIST/$p"; queue="$queue $DIST/$p"; }
done

declare -A seen
resolve() {
  for dll in $("$OBJDUMP" -p "$1" 2>/dev/null | awk "/DLL Name:/ {print \$3}"); do
    key=$(echo "$dll" | tr "A-Z" "a-z")
    [ -n "${seen[$key]:-}" ] && continue
    if [ -f "$BIN/$dll" ]; then seen[$key]=1; cp -n "$BIN/$dll" "$DIST/"; resolve "$BIN/$dll"; fi
  done
}
for f in $queue; do resolve "$f"; done

cd /src/build_win && rm -f klogg-win64.zip && (command -v zip >/dev/null || dnf install -y zip >/dev/null 2>&1) && zip -rq klogg-win64.zip klogg-win64
chown -R '"$(id -u)":"$(id -g)"' "$DIST" /src/build_win/klogg-win64.zip
du -sh "$DIST"; ls -la /src/build_win/klogg-win64.zip
'
echo "==> Done: $REPO/build_win/klogg-win64/  and  build_win/klogg-win64.zip"
