#!/usr/bin/env bash
# r2_build_test.sh — 建置 libramulator 並執行所有測試
#
# 用法（在 ramulator2/ 目錄下執行）：
#   chmod +x r2_build_test.sh
#   ./r2_build_test.sh              # 完整 cmake + 編譯 + 執行
#   ./r2_build_test.sh --no-cmake   # 跳過 cmake（libramulator 已建置）
#   ./r2_build_test.sh --only dl    # 只跑 DataLayer 測試
#   ./r2_build_test.sh --only bf    # 只跑 BitFlip 測試
#   ./r2_build_test.sh --only ttl   # 只重新產生 ttl_theta_experiment 的 YAML
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
BIN_DIR="$SCRIPT_DIR/bin"

# ── 引數解析 ─────────────────────────────────────────────────────────────────
NO_CMAKE=0
ONLY=""
args=("$@")
i=0
while [[ $i -lt ${#args[@]} ]]; do
  case "${args[$i]}" in
    --no-cmake) NO_CMAKE=1 ;;
    --only)     i=$(( i + 1 )); ONLY="${args[$i]}" ;;
  esac
  i=$(( i + 1 ))
done

# ── 步驟 0：export YAML config ────────────────────────────────────────────────
if [[ -z "$ONLY" || "$ONLY" == "dl" ]]; then
  echo "=== [0] export r2_data_layer_config.yaml ==="
  PYTHONPATH="$SCRIPT_DIR/python" python3 -m ramulator export \
      "$SCRIPT_DIR/r2_data_layer_config.py" \
      -o "$SCRIPT_DIR/r2_data_layer_config.yaml"
fi

if [[ -z "$ONLY" || "$ONLY" == "bf" ]]; then
  echo "=== [0] export r2_bit_flip_config.yaml ==="
  PYTHONPATH="$SCRIPT_DIR/python" python3 -m ramulator export \
      "$SCRIPT_DIR/r2_bit_flip_config.py" \
      -o "$SCRIPT_DIR/r2_bit_flip_config.yaml"
fi

if [[ -z "$ONLY" || "$ONLY" == "ecc" ]]; then
  echo "=== [0] export r2_ecc_config.yaml ==="
  PYTHONPATH="$SCRIPT_DIR/python" python3 -m ramulator export \
      "$SCRIPT_DIR/r2_ecc_config.py" \
      -o "$SCRIPT_DIR/r2_ecc_config.yaml"
fi

if [[ -z "$ONLY" || "$ONLY" == "btree" ]]; then
  echo "=== [0] export r2_btree_config.yaml ==="
  PYTHONPATH="$SCRIPT_DIR/python" python3 -m ramulator export \
      "$SCRIPT_DIR/r2_btree_config.py" \
      -o "$SCRIPT_DIR/r2_btree_config.yaml"
fi

# v2/ttl_theta_experiment 用的組態（DataLayer + ECC，兩者 debug 皆關）。
# 沒有對應的測試程式，但一定要 export —— 否則 ttl_theta_experiment 的
# A4/A5 臂會找不到 ECC plugin 而在建構子直接 throw。
if [[ -z "$ONLY" || "$ONLY" == "ttl" ]]; then
  echo "=== [0] export r2_ttl_config.yaml ==="
  PYTHONPATH="$SCRIPT_DIR/python" python3 -m ramulator export \
      "$SCRIPT_DIR/r2_ttl_config.py" \
      -o "$SCRIPT_DIR/r2_ttl_config.yaml"
fi

# ── 步驟 1：cmake build ───────────────────────────────────────────────────────
if [[ $NO_CMAKE -eq 0 ]]; then
  echo ""
  echo "=== [1] cmake build libramulator ==="
  cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" --parallel "$(nproc 2>/dev/null || echo 4)"
else
  echo "=== [1] 略過 cmake（--no-cmake） ==="
fi

mkdir -p "$BIN_DIR"

CXXFLAGS="-std=c++20 -O2"
INCLUDES="-I $SCRIPT_DIR/src -I $SCRIPT_DIR/ext/yaml-cpp/include -I $SCRIPT_DIR/ext/spdlog/include"
LDFLAGS="-L $SCRIPT_DIR -lramulator -Wl,-rpath,$SCRIPT_DIR"

# ── 步驟 2a：編譯 DataLayer 測試 ─────────────────────────────────────────────
if [[ -z "$ONLY" || "$ONLY" == "dl" ]]; then
  echo ""
  echo "=== [2a] 編譯 r2_test_data_layer.cpp ==="
  g++ $CXXFLAGS $INCLUDES \
      "$SCRIPT_DIR/r2_test_data_layer.cpp" \
      $LDFLAGS \
      -o "$BIN_DIR/test_data_layer"
  echo "  → $BIN_DIR/test_data_layer"
fi

# ── 步驟 2b：編譯 BitFlip 測試 ───────────────────────────────────────────────
if [[ -z "$ONLY" || "$ONLY" == "bf" ]]; then
  echo ""
  echo "=== [2b] 編譯 r2_test_bit_flip.cpp ==="
  g++ $CXXFLAGS $INCLUDES \
      "$SCRIPT_DIR/r2_test_bit_flip.cpp" \
      $LDFLAGS \
      -o "$BIN_DIR/test_bit_flip"
  echo "  → $BIN_DIR/test_bit_flip"
fi

# ── 步驟 2c：編譯 ECC 測試 ───────────────────────────────────────────────────
if [[ -z "$ONLY" || "$ONLY" == "ecc" ]]; then
  echo ""
  echo "=== [2c] 編譯 r2_test_ecc.cpp ==="
  g++ $CXXFLAGS $INCLUDES \
      "$SCRIPT_DIR/r2_test_ecc.cpp" \
      $LDFLAGS \
      -o "$BIN_DIR/test_ecc"
  echo "  → $BIN_DIR/test_ecc"
fi

# ── 步驟 2d：編譯 BTree 整合測試 ────────────────────────────────────────────
# V2 B-Tree source 相對路徑（ramulator2/ 的上層 v2/）
V2_DIR="$SCRIPT_DIR/../v2"
if [[ -z "$ONLY" || "$ONLY" == "btree" ]]; then
  echo ""
  echo "=== [2d] 編譯 r2_test_btree.cpp（含 v2 B-Tree source）==="
  g++ $CXXFLAGS $INCLUDES \
      -I "$SCRIPT_DIR" \
      -I "$V2_DIR" \
      "$SCRIPT_DIR/r2_test_btree.cpp" \
      "$SCRIPT_DIR/r2_btree_ram.cpp" \
      "$V2_DIR/BTree_core/btree.cpp" \
      "$V2_DIR/BTree_core/btree_node.cpp" \
      "$V2_DIR/new_RAM_core/new_ram.cpp" \
      "$V2_DIR/SSD_core/ssd.cpp" \
      "$V2_DIR/SSD_core/block.cpp" \
      "$V2_DIR/SSD_core/page.cpp" \
      "$V2_DIR/tools/func.cpp" \
      "$V2_DIR/utility.cpp" \
      $LDFLAGS \
      -o "$BIN_DIR/test_btree_r2"
  echo "  → $BIN_DIR/test_btree_r2"
fi

# ── 步驟 3：執行測試 ──────────────────────────────────────────────────────────
cd "$SCRIPT_DIR"
OVERALL=0

if [[ -z "$ONLY" || "$ONLY" == "dl" ]]; then
  echo ""
  echo "=== [3a] DataLayer 測試 ==="
  "$BIN_DIR/test_data_layer" "$SCRIPT_DIR/r2_data_layer_config.yaml" || OVERALL=1
fi

if [[ -z "$ONLY" || "$ONLY" == "bf" ]]; then
  echo ""
  echo "=== [3b] BitFlip 測試 ==="
  "$BIN_DIR/test_bit_flip" "$SCRIPT_DIR/r2_bit_flip_config.yaml" || OVERALL=1
fi

if [[ -z "$ONLY" || "$ONLY" == "ecc" ]]; then
  echo ""
  echo "=== [3c] ECC 測試 ==="
  "$BIN_DIR/test_ecc" "$SCRIPT_DIR/r2_ecc_config.yaml" || OVERALL=1
fi

if [[ -z "$ONLY" || "$ONLY" == "btree" ]]; then
  echo ""
  echo "=== [3d] BTree 整合測試 ==="
  "$BIN_DIR/test_btree_r2" "$SCRIPT_DIR/r2_btree_config.yaml" || OVERALL=1
fi

echo ""
if [[ $OVERALL -eq 0 ]]; then
  echo "=== 全部測試通過 ==="
else
  echo "=== 有測試失敗 ==="
fi
exit $OVERALL
