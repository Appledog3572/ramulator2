// r2_test_bit_flip.cpp — BitFlip plugin Step 2a 測試
//
// 測試項目：
//   BF-T1. BitFlip plugin 成功初始化（bf_is_ready()）
//   BF-T2. bf_inject() 精確翻轉指定 byte/bit
//   BF-T3. bf_inject_random() 恰好翻轉 1 個 bit
//   BF-T4. bf_total_flips() 正確累計
//   BF-T5. bf_set_interval() 啟用自動注入後，ticks 數量匹配翻轉次數
//
// 編譯（在 ramulator2/ 目錄下，先 build 完 libramulator）：
//   g++ -std=c++20 -O2 \
//       -I src \
//       -I ext/yaml-cpp/include \
//       -I ext/spdlog/include \
//       r2_test_bit_flip.cpp \
//       -L . -lramulator \
//       -Wl,-rpath,. \
//       -o bin/test_bit_flip
//   ./bin/test_bit_flip r2_bit_flip_config.yaml

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
#include "ramulator/controller/plugin/impl/bit_flip_api.h"

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

static void do_write(IMemorySystem* mem, uint64_t addr, int tx_bytes) {
  bool done = false;
  Request req(addr, Request::Type::Write, 0,
               [&done](Request&) { done = true; });
  req.size_bytes = tx_bytes;
  req.addr_vec   = {0};
  while (!mem->send(req)) mem->tick();
  while (!done)           mem->tick();
}

static Clk_t do_read(IMemorySystem* mem, uint64_t addr, int tx_bytes) {
  bool  done   = false;
  Clk_t depart = -1;
  Request req(addr, Request::Type::Read, 0,
               [&](Request& r) { done = true; depart = r.depart; });
  req.size_bytes = tx_bytes;
  req.addr_vec   = {0};
  while (!mem->send(req)) mem->tick();
  while (!done)           mem->tick();
  return depart;
}

// 計算兩個 byte vector 之間 bit 差異數
static int count_bit_diffs(const std::vector<uint8_t>& a,
                            const std::vector<uint8_t>& b) {
  int count = 0;
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) {
    uint8_t diff = a[i] ^ b[i];
    while (diff) { count += diff & 1; diff >>= 1; }
  }
  return count;
}

// ── 測試主體 ─────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  const char* cfg_path = (argc > 1) ? argv[1] : "r2_bit_flip_config.yaml";
  printf("=== r2_test_bit_flip ===\n");
  printf("config: %s\n\n", cfg_path);

  // 1. 建立 Ramulator2 記憶體系統
  auto config        = Config::parse_config_file(cfg_path);
  IFrontEnd*     fe  = Factory::create_frontend(config);
  IMemorySystem* mem = Factory::create_memory_system(config);

  fe->connect_memory_system(mem);
  mem->connect_frontend(fe);

  const int tx_bytes = mem->get_tx_bytes();

  // ── BF-T1：plugin 就緒 ────────────────────────────────────────────────────
  printf("-- BF-T1: 初始化 --\n");
  check(dl_is_ready(), "BF-T1a: DataLayer plugin 初始化成功");
  check(bf_is_ready(), "BF-T1b: BitFlip  plugin 初始化成功");
  if (!bf_is_ready()) {
    printf("[FATAL] BitFlip 未初始化，終止測試\n");
    return 1;
  }
  printf("\n");

  // ── BF-T2：精確翻轉指定 bit ───────────────────────────────────────────────
  printf("-- BF-T2: bf_inject() 精確翻轉 --\n");
  const int      PAGE_SIZE  = 4096;
  const uint64_t PAGE_ADDR  = 0;
  const int      TEST_BYTE  = 7;    // 翻轉第 7 個 byte
  const int      TEST_BIT   = 3;    // 翻轉第 3 個 bit（bit mask = 0x08）

  std::vector<uint8_t> wdata2(PAGE_SIZE, 0x00);   // 全零
  dl_store(PAGE_ADDR, wdata2);
  do_write(mem, PAGE_ADDR, tx_bytes);
  do_read(mem, PAGE_ADDR, tx_bytes);

  bf_inject(PAGE_ADDR, TEST_BYTE, TEST_BIT);

  auto rdata2 = dl_load(PAGE_ADDR);
  // 只有 byte TEST_BYTE 的 bit TEST_BIT 應該被翻成 1
  bool inject_correct = (rdata2[TEST_BYTE] == (uint8_t)(1u << TEST_BIT));
  // 其餘 bytes 全為 0
  bool others_zero = true;
  for (int i = 0; i < PAGE_SIZE; i++) {
    if (i != TEST_BYTE && rdata2[i] != 0) { others_zero = false; break; }
  }
  check(inject_correct, "BF-T2a: 目標 byte/bit 已翻轉為 1");
  check(others_zero,    "BF-T2b: 其餘 bytes 不受影響");
  printf("  byte[%d] = 0x%02X（期望 0x%02X）\n\n",
         TEST_BYTE, rdata2[TEST_BYTE], (1u << TEST_BIT));

  // ── BF-T3：隨機翻轉恰好 1 個 bit ─────────────────────────────────────────
  printf("-- BF-T3: bf_inject_random() --\n");
  // 先恢復乾淨狀態：重新存一頁已知資料
  std::vector<uint8_t> wdata3(PAGE_SIZE, 0xAA);
  dl_store(PAGE_ADDR, wdata3);

  uint64_t flip_addr = bf_inject_random();
  check(flip_addr != UINT64_MAX, "BF-T3a: inject_random() 回傳有效頁面位址");
  check(flip_addr == PAGE_ADDR,  "BF-T3b: 翻轉的頁面位址正確（唯一頁面）");

  auto rdata3 = dl_load(PAGE_ADDR);
  int diff_bits = count_bit_diffs(wdata3, rdata3);
  check(diff_bits == 1, "BF-T3c: 恰好翻轉 1 個 bit");
  printf("  bit diff count = %d\n\n", diff_bits);

  // ── BF-T4：bf_total_flips() 累計 ─────────────────────────────────────────
  printf("-- BF-T4: bf_total_flips() --\n");
  // 到目前為止：T2 inject 1 次，T3 inject_random 1 次 → total = 2
  uint64_t flips_after_manual = bf_total_flips();
  check(flips_after_manual == 2,
        "BF-T4: bf_total_flips() == 2（T2 + T3 各 1 次）");
  printf("  bf_total_flips() = %lu\n\n", (unsigned long)flips_after_manual);

  // ── BF-T5：自動注入（flip_interval） ─────────────────────────────────────
  printf("-- BF-T5: 自動注入 bf_set_interval(10) × 100 ticks --\n");
  // 確保有頁面可以翻
  std::vector<uint8_t> wdata5(PAGE_SIZE, 0xFF);
  dl_store(PAGE_ADDR, wdata5);

  // 重設為固定 interval，並空轉 100 ticks
  // 因為 m_tick_count 從 0 開始（interval=0 時不遞增），
  // 設定後 100 ticks → 10 次注入（在 tick 10, 20, …, 100）
  bf_set_interval(10);
  for (int i = 0; i < 100; i++) mem->tick();
  bf_set_interval(0);   // 回到停用

  uint64_t flips_after_auto = bf_total_flips();
  uint64_t auto_flips = flips_after_auto - flips_after_manual;
  check(auto_flips == 10,
        "BF-T5: 100 ticks / interval=10 → 恰好 10 次自動注入");
  printf("  auto_flips=%lu  total=%lu\n\n",
         (unsigned long)auto_flips, (unsigned long)flips_after_auto);

  // ── 總結 ──────────────────────────────────────────────────────────────────
  printf("=== 結果：%d pass, %d fail ===\n", g_pass, g_fail);

  fe->finalize();
  mem->finalize();
  return g_fail == 0 ? 0 : 1;
}
