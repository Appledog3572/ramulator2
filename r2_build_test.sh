#!/usr/bin/env bash
# r2_build_test.sh — 建置 libramulator 並編譯執行 DataLayer Step 1 測試
#
# 用法（在 ramulator2/ 目錄下執行）：
#   chmod +x r2_build_test.sh
#   ./r2_build_test.sh
#
# 若 libramulator 已 build 過、只想重新編譯測試，傳 --no-cmake：
#   ./r2_build_test.sh --no-cmake
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
BIN_DIR="$SCRIPT_DIR/bin"
SRC="$SCRIPT_DIR/r2_test_data_layer.cpp"
OUT="$BIN_DIR/test_data_layer"
CONFIG="$SCRIPT_DIR/r2_data_layer_config.yaml"

# ── 步驟 0：從 Python config script export YAML ──────────────────────────────
echo "=== [0/3] export r2_data_layer_config.yaml ==="
PYTHONPATH="$SCRIPT_DIR/python" python3 -m ramulator export \
    "$SCRIPT_DIR/r2_data_layer_config.py" \
    -o "$CONFIG"

# ── 步驟 1：cmake build（可跳過） ───────────────────────────────────────────
if [[ "${1:-}" != "--no-cmake" ]]; then
  echo ""
  echo "=== [1/3] cmake build libramulator ==="
  cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" --parallel "$(nproc 2>/dev/null || echo 4)"
else
  echo "=== [1/3] 略過 cmake（--no-cmake）==="
fi

# ── 步驟 2：編譯測試程式 ────────────────────────────────────────────────────
echo ""
echo "=== [2/3] 編譯 r2_test_data_layer.cpp ==="
mkdir -p "$BIN_DIR"

g++ -std=c++20 -O2 \
    -I "$SCRIPT_DIR/src" \
    -I "$SCRIPT_DIR/ext/yaml-cpp/include" \
    -I "$SCRIPT_DIR/ext/spdlog/include" \
    "$SRC" \
    -L "$SCRIPT_DIR" -lramulator \
    -Wl,-rpath,"$SCRIPT_DIR" \
    -o "$OUT"

echo "  → $OUT"

# ── 步驟 3：執行測試 ────────────────────────────────────────────────────────
echo ""
echo "=== [3/3] 執行測試 ==="
cd "$SCRIPT_DIR"    # cwd 需與 config yaml 位置一致
"$OUT" "$CONFIG"
