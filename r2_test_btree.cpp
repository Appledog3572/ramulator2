// r2_test_btree.cpp — B-Tree + Ramulator2 整合測試
//
// 測試項目：
//   BT-T1. 正確性：insert N 個 key → search 全部命中，值正確
//   BT-T2. DDR4 時序：B-Tree search 的實際延遲來自 Ramulator2 depart cycle
//   BT-T3. ECC 透明修正：BitFlip 自動注入 bit error，B-Tree 仍能正確讀取
//   BT-T4. ECC 統計：大量操作後 CE > 0（ECC 確實在工作）
//
// 架構：
//   BTree → RamulatorRAM → Ramulator2 IMemorySystem
//                           ├─ DataLayer（儲存 B-Tree page 資料）
//                           ├─ BitFlip（週期性自動注入 DRAM bit flip）
//                           └─ ECC（透明修正，B-Tree 不感知）
//
// 編譯（由 r2_build_test.sh --only btree 自動完成）

#include <cstdio>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <random>
#include <algorithm>

// Ramulator2
#include "ramulator/base/base.h"
#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/base/request.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"
#include "ramulator/controller/plugin/impl/data_layer_api.h"
#include "ramulator/controller/plugin/impl/bit_flip_api.h"
#include "ramulator/controller/plugin/impl/ecc_plugin_api.h"

// RamulatorRAM adapter
#include "r2_btree_ram.hpp"

// V2 B-Tree
#include "BTree_core/btree.hpp"

using namespace Ramulator;

// ── NullFrontend ─────────────────────────────────────────────────────────────
class NullFrontend : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, NullFrontend, "NullFrontend")
 public:
  void init()        override {}
  void tick()        override {}
  bool is_finished() override { return true; }
};

// ── 輔助 ─────────────────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* msg) {
  if (cond) { printf("  [PASS] %s\n", msg); g_pass++; }
  else       { printf("  [FAIL] %s\n", msg); g_fail++; }
}

// ── 測試主體 ─────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
  const char* cfg_path = (argc > 1) ? argv[1] : "r2_btree_config.yaml";
  printf("=== r2_test_btree ===\n");
  printf("config: %s\n\n", cfg_path);

  // ── 建立 Ramulator2 記憶體系統 ────────────────────────────────────────────
  auto config        = Config::parse_config_file(cfg_path);
  IFrontEnd*     fe  = Factory::create_frontend(config);
  IMemorySystem* mem = Factory::create_memory_system(config);
  fe->connect_memory_system(mem);
  mem->connect_frontend(fe);

  printf("DDR4 tCK=%.3f ns  tx_bytes=%d\n\n",
         mem->get_tCK(), mem->get_tx_bytes());

  // ── Plugin 就緒確認 ───────────────────────────────────────────────────────
  printf("-- Plugin 狀態 --\n");
  check(dl_is_ready(),  "DataLayer plugin 就緒");
  check(bf_is_ready(),  "BitFlip  plugin 就緒");
  check(ecc_is_ready(), "ECC      plugin 就緒");
  printf("\n");

  // ── 建立 RamulatorRAM + BTree ─────────────────────────────────────────────
  RamulatorRAM r2ram(mem);
  const uint32_t ORDER     = 4;
  const uint32_t PAGE_SIZE = 4096;
  BTree btree(&r2ram, ORDER, PAGE_SIZE);

  // ── BT-T1：正確性測試（N=50 個 key）─────────────────────────────────────
  printf("-- BT-T1: insert + search 正確性（N=50）--\n");
  const int N = 50;
  std::vector<uint64_t> keys(N), vals(N);
  for (int i = 0; i < N; i++) {
    keys[i] = (uint64_t)(i + 1) * 10;
    vals[i] = (uint64_t)(i + 1) * 100;
  }

  for (int i = 0; i < N; i++)
    btree.insert(keys[i], vals[i]);

  bool all_found = true;
  bool all_correct = true;
  for (int i = 0; i < N; i++) {
    uint64_t v = 0;
    bool found = btree.search(keys[i], v);
    if (!found)   all_found   = false;
    if (v != vals[i]) all_correct = false;
  }
  check(all_found,   "BT-T1a: 50 個 key 全部 search 命中");
  check(all_correct, "BT-T1b: 所有 value 與 insert 時一致");

  // 搜尋不存在的 key
  uint64_t dummy = 0;
  bool not_found = !btree.search(9999, dummy);
  check(not_found, "BT-T1c: 不存在的 key 回傳 false");
  printf("  pages allocated=%lu\n\n", r2ram.page_count());

  // ── BT-T2：DDR4 時序 ─────────────────────────────────────────────────────
  printf("-- BT-T2: DDR4 latency（最後一次 search 的延遲）--\n");
  // 做一次 search，觀察 RAMReadResult.latency（由 do_read depart 換算）
  // （BTree 內部的 read_node 使用 ram_->read()，latency 存在 result 裡）
  // 這裡我們手動做一次讀取來拿到 latency sample
  auto sample = r2ram.read(0, 0.0);  // page 0 = root
  double lat_ns = sample.latency * 1e9;
  check(lat_ns > 0, "BT-T2a: DDR4 latency > 0 ns");
  check(lat_ns < 1000.0, "BT-T2b: DDR4 latency < 1000 ns（合理範圍）");
  printf("  page 0 read latency = %.2f ns  (depart cycle × tCK)\n\n", lat_ns);

  // ── BT-T3：ECC 透明修正（BitFlip 已在運行中自動注入）───────────────────
  printf("-- BT-T3: ECC 透明修正 + B-Tree 正確性 --\n");
  // 手動注入 1 bit flip 到某頁，然後 B-Tree search 仍應正確
  uint64_t ce_before = ecc_total_ce();
  // 翻 page 0（root）的 byte[16] bit 0
  dl_flip_bit(0 * PAGE_SIZE, 16, 0);

  bool inject_ok = true;
  for (int i = 0; i < N; i++) {
    uint64_t v = 0;
    if (!btree.search(keys[i], v) || v != vals[i]) {
      inject_ok = false;
      break;
    }
  }
  uint64_t ce_after = ecc_total_ce();

  check(inject_ok, "BT-T3a: 注入 1-bit flip 後，B-Tree search 仍全部正確");
  check(ce_after > ce_before, "BT-T3b: ECC CE 計數增加（確認 ECC 在修正）");
  printf("  CE before=%lu → after=%lu（delta=%lu）\n\n",
         (unsigned long)ce_before, (unsigned long)ce_after,
         (unsigned long)(ce_after - ce_before));

  // ── BT-T4：自動 BitFlip 累計（操作夠多次後 CE > 0）────────────────────
  printf("-- BT-T4: 大量操作後 BitFlip 自動注入 CE > 0 --\n");
  // 插入更多 key 觸發更多 DRAM ticks → BitFlip 自動注入更多錯誤
  const int N2 = 100;
  for (int i = N; i < N + N2; i++)
    btree.insert((uint64_t)(i + 1) * 10, (uint64_t)(i + 1) * 100);

  bool search_ok = true;
  for (int i = 0; i < N + N2; i++) {
    uint64_t v = 0;
    if (!btree.search((uint64_t)(i + 1) * 10, v)) {
      search_ok = false; break;
    }
  }
  check(search_ok, "BT-T4a: insert 150 個 key 全部可搜到");
  check(ecc_total_ce() > 0,  "BT-T4b: ECC CE > 0（BitFlip 自動注入後 ECC 有動作）");
  check(ecc_total_ue() == 0, "BT-T4c: ECC UE == 0（無雙 bit error）");
  printf("  total CE=%lu  UE=%lu  pages=%lu\n\n",
         (unsigned long)ecc_total_ce(),
         (unsigned long)ecc_total_ue(),
         r2ram.page_count());

  // ── 總結 ──────────────────────────────────────────────────────────────────
  printf("=== 結果：%d pass, %d fail ===\n", g_pass, g_fail);
  fe->finalize();
  mem->finalize();
  return g_fail == 0 ? 0 : 1;
}
