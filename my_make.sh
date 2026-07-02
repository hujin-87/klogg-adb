#!/usr/bin/env bash
#
# 一键编译 + 打包脚本
#   - Linux 编译 (cmake --build，-j24)
#   - 打包成 .deb (CPack DEB generator)
#   - 交叉编译 + 打包成 Windows .exe / .zip (docker/mxe-win)
#   - 生成的 .deb 和 win64 klogg-win64.zip 统一 copy 到 ./output/，并自动打开该目录
#
# 用法:
#   ./my_make.sh              # 全部: 编译 + deb + exe
#   ./my_make.sh build        # 只编译 Linux
#   ./my_make.sh deb          # 编译 Linux + 打 deb
#   ./my_make.sh exe          # 只交叉编译 Windows exe/zip
#   ./my_make.sh all          # 等同于无参数 (build + deb + exe)
#
# 环境变量:
#   JOBS=24                   # 并行度 (默认 24)
#   BUILD_TYPE=RelWithDebInfo # cmake 构建类型
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$REPO/build_root"
OUTPUT_DIR="$REPO/output"
JOBS="${JOBS:-24}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

TARGET="${1:-all}"

log() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }

usage() {
  cat <<'EOF'
一键编译 + 打包脚本 (klogg-adb)

用法:
  ./my_make.sh [目标]

目标:
  build        只编译 Linux
  deb          编译 Linux + 打 .deb
  exe          只交叉编译 Windows exe/zip
  all          编译 + deb + exe (默认, 无参数时等同 all)
  -h, --help   显示本帮助

产物:
  生成的 .deb 和 win64 klogg-win64.zip 统一复制到 ./output/, 结束后自动打开该目录

环境变量:
  JOBS=24                   并行度 (默认 24)
  BUILD_TYPE=RelWithDebInfo cmake 构建类型

示例:
  ./my_make.sh              # 全部
  JOBS=32 ./my_make.sh deb  # 32 线程编译并打 deb
EOF
}

# ---------------------------------------------------------------------------
# 1) Linux 配置 + 编译
# ---------------------------------------------------------------------------
do_build() {
  if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    log "[配置] cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    cmake -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -S "$REPO" -B "$BUILD_DIR"
  fi
  log "[编译] cmake --build -j$JOBS"
  cmake --build "$BUILD_DIR" -j"$JOBS"
  log "产物: $BUILD_DIR/output/klogg"
}

# ---------------------------------------------------------------------------
# 2) 打包 .deb  (CPack 已在 CMakeLists.txt 中配置好 DEB generator)
# ---------------------------------------------------------------------------
do_deb() {
  log "[打包] 生成 .deb"
  ( cd "$BUILD_DIR" && cpack -G DEB )
  # CPACK_OUTPUT_FILE_PREFIX = "packages"
  local deb
  deb="$(find "$BUILD_DIR/packages" -maxdepth 1 -name '*.deb' 2>/dev/null | sort | tail -n1 || true)"
  if [ -n "$deb" ]; then
    mkdir -p "$OUTPUT_DIR"
    cp -f "$deb" "$OUTPUT_DIR/"
    log "deb 产物: $OUTPUT_DIR/$(basename "$deb")"
  else
    log "警告: 未找到 .deb 产物，请检查 cpack 输出"
  fi
}

# ---------------------------------------------------------------------------
# 3) 交叉编译 Windows exe/zip (docker + MinGW)
# ---------------------------------------------------------------------------
do_exe() {
  log "[Windows] 通过 docker/mxe-win 交叉编译"
  "$REPO/docker/mxe-win/build-win.sh"
  local zip="$REPO/build_win/klogg-win64.zip"
  if [ -f "$zip" ]; then
    mkdir -p "$OUTPUT_DIR"
    cp -f "$zip" "$OUTPUT_DIR/"
    log "win64 产物: $OUTPUT_DIR/klogg-win64.zip"
  else
    log "警告: 未找到 $zip"
  fi
}

# ---------------------------------------------------------------------------
# 打开 output 目录
# ---------------------------------------------------------------------------
open_output() {
  [ -d "$OUTPUT_DIR" ] || return 0
  log "output 目录内容:"
  ls -lh "$OUTPUT_DIR"
  if command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$OUTPUT_DIR" >/dev/null 2>&1 || true
  fi
}

case "$TARGET" in
  -h|--help|help) usage; exit 0 ;;
  build) do_build ;;
  deb)   do_build; do_deb; open_output ;;
  exe)   do_exe; open_output ;;
  all)   do_build; do_deb; do_exe; open_output ;;
  *)
    echo "未知参数: $TARGET" >&2
    echo "用法: $0 [build|deb|exe|all]" >&2
    exit 1
    ;;
esac

log "全部完成 ✅"
