# Cross-compiling klogg for Windows (x86-64) on Linux

Produces a portable **Windows x64 build** of klogg using Fedora's MinGW-w64
toolchain (GCC 14) + prebuilt Qt5, driven from a Docker container. No Windows
machine or MSVC required.

## 1. Build the toolchain image

```
docker build -f docker/mxe-win/Dockerfile.fedora -t klogg-win:fedora docker/mxe-win
```

The image provides: `mingw64-gcc-c++` (GCC 14), `mingw64-qt5-qtbase/qttools`,
`mingw64-boost`, native `gcc-c++` (to build the host-side `maddy` doc tool),
plus cmake/ninja/ragel. It also symlinks the Qt Linguist host tools that the
MinGW Qt package doesn't ship as runnable binaries.

## 2. Start a build container (repo mounted)

```
docker run -d --name klogg-build -v "$PWD":/src klogg-win:fedora sleep infinity
docker exec klogg-build git config --global --add safe.directory '*'
```

## 3. Build a host-native `maddy` (documentation generator)

The cross-built `maddy.exe` can't run on the Linux build host, so build a native one:

```
docker exec klogg-build bash -lc '
g++ -O2 -std=c++17 -I/src/build_win/_deps/maddy-src/include \
    /src/build_win/_deps/maddy-src/main.cpp -o /usr/local/bin/maddy-host'
```

(Only possible after the first configure has fetched the maddy source via CPM.
On a clean tree, run the configure in step 4 once — it will fail at the doc
step — then build maddy-host, then re-run step 4.)

## 4. Configure + build

```
docker exec klogg-build bash -lc '
cd /src
mingw64-cmake -S /src -B /src/build_win -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKLOGG_USE_HYPERSCAN=OFF -DKLOGG_USE_VECTORSCAN=OFF \
  -DKLOGG_BUILD_TESTS=OFF -DKLOGG_USE_LTO=OFF -DKLOGG_USE_SENTRY=OFF \
  -DKLOGG_GENERIC_CPU=ON \
  -DWARNINGS_AS_ERRORS=OFF \
  -DCMAKE_C_FLAGS="-include intrin.h" \
  -DCMAKE_CXX_FLAGS="-Wa,-mbig-obj" \
  -DTBBMALLOC_PROXY_BUILD=OFF \
  -DKLOGG_HOST_MADDY=/usr/local/bin/maddy-host
ninja -C /src/build_win'
```

### Why each non-obvious flag is needed for MinGW

- `KLOGG_GENERIC_CPU=ON` — the default build uses `-march=native`, which bakes in
  the *build host's* CPU instructions and crashes on other machines / Wine.
  Generic gives a portable `-march=x86-64` baseline (+SSE4.2).
- `CMAKE_C_FLAGS=-include intrin.h` — under MinGW `<windows.h>` defines `_M_X64`,
  so liblzma (in kf5archive) takes the MSVC `_BitScanForward64` branch; force-
  including `intrin.h` provides that intrinsic.
- `CMAKE_CXX_FLAGS=-Wa,-mbig-obj` — the exprtk-heavy `booleanevaluator.cpp`
  exceeds the PE/COFF section limit ("too many sections"); MinGW's `/bigobj`
  equivalent.
- `TBBMALLOC_PROXY_BUILD=OFF` — TBB's malloc proxy fails to link under MinGW and
  klogg uses mimalloc anyway.
- `WARNINGS_AS_ERRORS=OFF` — assorted MinGW-only sign-conversion warnings.

### Source changes required (committed on this branch)

- `3rdparty/CMakeLists.txt` — build TBB **static** for MinGW (its DLL exports no
  symbols under MinGW, so a shared build leaves `tbb::detail::r1::*` unresolved).
- `src/app/CMakeLists.txt` — allow `-DKLOGG_HOST_MADDY=<path>` to use a host
  maddy instead of the non-runnable cross-built one.

## 5. Package (exe + Qt DLLs + plugins)

A recursive DLL collector assembles `build_win/klogg-win64/` with `klogg.exe`,
`klogg_portable.exe`, `klogg_grep.exe`, all transitive MinGW/Qt DLLs, and the
mandatory `platforms/qwindows.dll` (+ imageformats/styles). Result is zipped to
`build_win/klogg-win64.zip`.

## Output

- `build_win/klogg-win64/klogg.exe`          — GUI log viewer (portable)
- `build_win/klogg-win64/klogg_portable.exe` — portable variant
- `build_win/klogg-win64/klogg_grep.exe`     — CLI grep tool
- `build_win/klogg-win64.zip`                — the whole redistributable package

Verified with Wine: `klogg_grep.exe -e apple sample.log` matches correctly and
exits 0; all bundled DLLs and the Qt platform plugin load.
