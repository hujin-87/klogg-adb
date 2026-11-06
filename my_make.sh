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
#   ./my_make.sh bin          # 只编译 klogg 可执行文件 (带 LTO, 和 deb 一致)
#   ./my_make.sh dev          # 开发用: 不开 LTO, 改一个文件后重编最快 (~10s)
#   ./my_make.sh build        # 只编译 Linux (全部 target)
#   ./my_make.sh deb          # 编译 Linux + 打 deb
#   ./my_make.sh exe          # 只交叉编译 Windows exe/zip
#   ./my_make.sh all          # 等同于无参数 (build + deb + exe)
#
# 环境变量:
#   JOBS=24                   # 并行度 (默认 24)
#   BUILD_TYPE=RelWithDebInfo # cmake 构建类型
#
# 编译速度相关 (首次配置时生效; 已有 build 目录不会自动切换):
#   - 有 ninja 就用 Ninja 生成器, 有 ccache 就启用 ccache
#   - 链接阶段 LTO 用 -flto=auto 并行 (CMakeLists.txt), 76s -> ~11s
#   - dev 目标用独立的 build_dev/ 目录且关闭 LTO, 链接只要几秒
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$REPO/build_root"
DEV_BUILD_DIR="$REPO/build_dev"
OUTPUT_DIR="$REPO/output"
JOBS="${JOBS:-24}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

TARGET="${1:-all}"

log() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }

# cmake 配置参数: 自动用上 ninja / ccache (都可选)
configure_args() {
  local -a args=( -DCMAKE_BUILD_TYPE="$BUILD_TYPE" )
  # 显式指定系统编译器, 避免 PATH 里的同名杂物 (如 ~/bin/cc) 被当成编译器
  if [ -x /usr/bin/cc ] && [ -x /usr/bin/c++ ]; then
    args+=( -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ )
  fi
  if command -v ninja >/dev/null 2>&1; then
    args+=( -G Ninja )
  fi
  if command -v ccache >/dev/null 2>&1; then
    args+=( -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache )
  fi
  printf '%s\n' "${args[@]}"
}

usage() {
  cat <<'EOF'
一键编译 + 打包脚本 (klogg-adb)

用法:
  ./my_make.sh [目标]

目标:
  bin          只编译 klogg 可执行文件 (带 LTO, 产物 build_root/output/klogg)
  dev          开发用快速编译, 关闭 LTO (产物 build_dev/output/klogg)
  build        只编译 Linux (全部 target)
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
  ./my_make.sh dev          # 改代码后快速验证
  JOBS=32 ./my_make.sh deb  # 32 线程编译并打 deb
EOF
}

# ---------------------------------------------------------------------------
# 1) Linux 配置 + 编译
# ---------------------------------------------------------------------------
do_build() {
  if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    log "[配置] cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    mapfile -t cfg < <(configure_args)
    cmake "${cfg[@]}" -S "$REPO" -B "$BUILD_DIR"
  fi
  log "[编译] cmake --build -j$JOBS"
  cmake --build "$BUILD_DIR" -j"$JOBS"
  log "产物: $BUILD_DIR/output/klogg"
}

# ---------------------------------------------------------------------------
# 1b) 只编译 klogg 可执行文件 (只 link 主程序)
# ---------------------------------------------------------------------------
do_bin() {
  if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    log "[配置] cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    mapfile -t cfg < <(configure_args)
    cmake "${cfg[@]}" -S "$REPO" -B "$BUILD_DIR"
  fi
  log "[编译] cmake --build --target klogg -j$JOBS"
  cmake --build "$BUILD_DIR" --target klogg -j"$JOBS"
  log "产物: $BUILD_DIR/output/klogg"
}

# ---------------------------------------------------------------------------
# 1c) 开发用快速编译: 独立目录, 关 LTO, 不编测试
# ---------------------------------------------------------------------------
do_dev() {
  if [ ! -f "$DEV_BUILD_DIR/CMakeCache.txt" ]; then
    log "[配置] cmake (dev, LTO off) -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    mapfile -t cfg < <(configure_args)
    cmake "${cfg[@]}" -DKLOGG_USE_LTO=OFF -DKLOGG_BUILD_TESTS=OFF \
      -S "$REPO" -B "$DEV_BUILD_DIR"
  fi
  log "[编译] cmake --build --target klogg -j$JOBS (dev)"
  cmake --build "$DEV_BUILD_DIR" --target klogg -j"$JOBS"
  log "产物: $DEV_BUILD_DIR/output/klogg"
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
  bin)   do_bin ;;
  dev)   do_dev ;;
  build) do_build ;;
  deb)   do_build; do_deb; open_output ;;
  exe)   do_exe; open_output ;;
  all)   do_build; do_deb; do_exe; open_output ;;
  *)
    echo "未知参数: $TARGET" >&2
    echo "用法: $0 [bin|dev|build|deb|exe|all]" >&2
    exit 1
    ;;
esac

log "全部完成 ✅"
