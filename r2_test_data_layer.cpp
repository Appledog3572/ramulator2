// r2_test_data_layer.cpp — DataLayer plugin Step 1 測試
//
// 測試項目：
//   T1. DataLayer plugin 成功初始化（dl_is_ready()）
//   T2. store() 儲存後 has_page() 回 true
//   T3. Ramulator2 Write request 送出並完成（callback 觸發）
//   T4. Ramulator2 Read  request 送出並完成（callback 觸發）
//   T5. dl_load() 取回的資料與原始資料完全一致（4096 bytes）
//   T6. erase() 後 has_page() 回 false
//   T7. 多頁次 write-then-read，各頁資料互不干擾
//
// 編譯（在 ramulator2/ 目錄下，先 build 完 libramulator）：
//   g++ -std=c++20 -O2 \
//       -I src \
//       -I ext/yaml-cpp/include \
//       -I ext/spdlog/include \
//       r2_test_data_layer.cpp \
//       -L build -lramulator \
//       -Wl,-rpath,build \
//       -o bin/test_data_layer
//   ./bin/test_data_layer r2_data_layer_config.yaml

#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>
#include <stdexcept>

#include "ramulator/base/base.h"
#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/base/request.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"
#include "ramulator/controller/plugin/impl/data_layer_api.h"

using namespace Ramulator;

// ── NullFrontend：讓 connect_frontend 可以正常呼叫 setup() ───────────────────
// RAMULATOR_REGISTER_IMPLEMENTATION 在此 .cpp 定義，靜態初始化時自動注冊到
// Factory，因此 YAML 的 frontend: impl: NullFrontend 可以正確建立。
class NullFrontend : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, NullFrontend, "NullFrontend")
 public:
  void init()         override {}
  void tick()         override {}
  bool is_finished()  override { return true; }
};

// ── 輔助 ─────────────────────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* msg) {
  if (cond) { printf("  [PASS] %s\n", msg); g_pass++; }
  else       { printf("  [FAIL] %s\n", msg); g_fail++; }
}

// 送出 Write request 並等 callback（同步 tick loop）
static void do_write(IMemorySystem* mem, uint64_t addr, int tx_bytes) {
  bool done = false;
  Request req(addr, Request::Type::Write, 0,
               [&done](Request&) { done = true; });
  req.size_bytes = tx_bytes;
  while (!mem->send(req)) mem->tick();
  while (!done)           mem->tick();
}

// 送出 Read request 並等 callback，回傳 depart cycle
static Clk_t do_read(IMemorySystem* mem, uint64_t addr, int tx_bytes) {
  bool  done   = false;
  Clk_t depart = -1;
  Request req(addr, Request::Type::Read, 0,
               [&](Request& r) { done = true; depart = r.depart; });
  req.size_bytes = tx_bytes;
  while (!mem->send(req)) mem->tick();
  while (!done)           mem->tick();
  return depart;
}

// ── 測試主體 ─────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  const char* cfg_path = (argc > 1) ? argv[1] : "r2_data_layer_config.yaml";
  printf("=== r2_test_data_layer ===\n");
  printf("config: %s\n\n", cfg_path);

  // 1. 建立 Ramulator2 記憶體系統
  auto config       = Config::parse_config_file(cfg_path, {});
  IFrontEnd*     fe = Factory::create_frontend(config);
  IMemorySystem* mem= Factory::create_memory_system(config);

  // connect 觸發所有 plugin 的 setup()（包含 DataLayerPlugin::setup()）
  fe->connect_memory_system(mem);
  mem->connect_frontend(fe);

  // 2. 確認 DataLayer 就緒
  check(dl_is_ready(), "T1: DataLayer plugin 初始化成功");
  if (!dl_is_ready()) {
    printf("[FATAL] DataLayer 未初始化，終止測試\n");
    return 1;
  }

  const int   tx_bytes = mem->get_tx_bytes();
  const float tCK_ns   = mem->get_tCK();
  printf("  DDR4 tx_bytes=%d  tCK=%.3f ns\n\n", tx_bytes, tCK_ns);

  // ── T2–T6：單頁寫入 + 讀取 + 驗證 ─────────────────────────────────────────
  printf("-- 單頁測試 --\n");
  const int      PAGE_SIZE = 4096;
  const uint64_t PAGE_ADDR = 0;   // page_id=0 → addr=0

  // 準備已知 pattern
  std::vector<uint8_t> wdata(PAGE_SIZE);
  for (int i = 0; i < PAGE_SIZE; i++) wdata[i] = (uint8_t)(i & 0xFF);

  dl_store(PAGE_ADDR, wdata);
  check(dl_has_page(PAGE_ADDR), "T2: store() 後 has_page() = true");

  do_write(mem, PAGE_ADDR, tx_bytes);
  check(true, "T3: Write request callback 觸發");

  Clk_t depart = do_read(mem, PAGE_ADDR, tx_bytes);
  check(depart > 0, "T4: Read request callback 觸發");

  auto rdata = dl_load(PAGE_ADDR);
  check(rdata.size() == (size_t)PAGE_SIZE && rdata == wdata,
        "T5: dl_load() 取回資料與寫入完全一致（4096 bytes）");

  dl_erase(PAGE_ADDR);
  check(!dl_has_page(PAGE_ADDR), "T6: erase() 後 has_page() = false");

  printf("  read_depart_cycle=%lld  ≈ %.2f ns\n\n",
         (long long)depart, depart * tCK_ns);

  // ── T7：多頁互不干擾 ───────────────────────────────────────────────────────
  printf("-- 多頁測試（4 頁）--\n");
  const int N_PAGES = 4;

  for (int p = 0; p < N_PAGES; p++) {
    uint64_t addr = (uint64_t)p * PAGE_SIZE;
    std::vector<uint8_t> d(PAGE_SIZE, (uint8_t)p);  // 每頁填不同值
    dl_store(addr, d);
    do_write(mem, addr, tx_bytes);
  }

  bool multi_ok = true;
  for (int p = 0; p < N_PAGES; p++) {
    uint64_t addr = (uint64_t)p * PAGE_SIZE;
    do_read(mem, addr, tx_bytes);
    auto rd = dl_load(addr);
    for (int i = 0; i < PAGE_SIZE; i++) {
      if (rd[i] != (uint8_t)p) { multi_ok = false; break; }
    }
    if (!multi_ok) break;
  }
  check(multi_ok, "T7: 多頁資料互不干擾");
  printf("  dl_page_count()=%zu\n\n", dl_page_count());

  // ── 總結 ──────────────────────────────────────────────────────────────────
  printf("=== 結果：%d pass, %d fail ===\n", g_pass, g_fail);

  fe->finalize();
  mem->finalize();
  return g_fail == 0 ? 0 : 1;
}
