// r2_test_ecc.cpp — ECC plugin Step 2b 測試
//
// 測試項目：
//   ECC-T1. ecc_is_ready() == true（plugin 成功初始化）
//   ECC-T2. ecc_write + ecc_read 無錯誤 → 資料完整，CE/UE = 0
//   ECC-T3. 注入 1 bit flip → ecc_read 自動修正（CE），回傳原始資料
//   ECC-T4. 同一 8-byte word 注入 2 bit flips → 偵測 UE，CE 不遞增
//   ECC-T5. 多頁測試 — 各頁 CE/UE 累計正確
//
// 說明：
//   dl_flip_bit() 模擬 DRAM 資料晶片的 bit flip（DRAM 硬體錯誤）。
//   ECC 晶片（ECCPlugin 內部 parity）不受 dl_flip_bit 影響，
//   因此 ecc_read() 可以用 ECC 晶片的 parity 來偵測/修正 DRAM 的錯誤。
//
// 編譯（在 ramulator2/ 目錄下）：
//   g++ -std=c++20 -O2 \
//       -I src -I ext/yaml-cpp/include -I ext/spdlog/include \
//       r2_test_ecc.cpp -L . -lramulator -Wl,-rpath,. \
//       -o bin/test_ecc
//   ./bin/test_ecc r2_ecc_config.yaml

#include <cstdio>
#include <cstring>
#include <vector>
#include <stdexcept>

#include "ramulator/base/base.h"
#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/base/request.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"
#include "ramulator/controller/plugin/impl/data_layer_api.h"
#include "ramulator/controller/plugin/impl/ecc_plugin_api.h"

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

static void do_read(IMemorySystem* mem, uint64_t addr, int tx_bytes) {
  bool done = false;
  Request req(addr, Request::Type::Read, 0,
               [&done](Request&) { done = true; });
  req.size_bytes = tx_bytes;
  req.addr_vec   = {0};
  while (!mem->send(req)) mem->tick();
  while (!done)           mem->tick();
}

// ── 測試主體 ─────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  const char* cfg_path = (argc > 1) ? argv[1] : "r2_ecc_config.yaml";
  printf("=== r2_test_ecc ===\n");
  printf("config: %s\n\n", cfg_path);

  auto config        = Config::parse_config_file(cfg_path);
  IFrontEnd*     fe  = Factory::create_frontend(config);
  IMemorySystem* mem = Factory::create_memory_system(config);
  fe->connect_memory_system(mem);
  mem->connect_frontend(fe);

  const int tx_bytes = mem->get_tx_bytes();

  // ── ECC-T1：plugin 就緒 ───────────────────────────────────────────────────
  printf("-- ECC-T1: 初始化 --\n");
  check(dl_is_ready(),  "ECC-T1a: DataLayer plugin 初始化成功");
  check(ecc_is_ready(), "ECC-T1b: ECC plugin 初始化成功");
  if (!ecc_is_ready()) {
    printf("[FATAL] ECC plugin 未初始化，終止測試\n");
    return 1;
  }
  printf("\n");

  const int      PAGE_SIZE = 4096;
  const uint64_t PAGE_ADDR = 0;

  // ── ECC-T2：無錯誤 round-trip ─────────────────────────────────────────────
  printf("-- ECC-T2: 無錯誤 ecc_write → ecc_read --\n");
  {
    // 準備已知 pattern
    std::vector<uint8_t> wdata(PAGE_SIZE);
    for (int i = 0; i < PAGE_SIZE; i++) wdata[i] = static_cast<uint8_t>(i & 0xFF);

    ecc_write(PAGE_ADDR, wdata);    // DataLayer + ECC chip
    do_write(mem, PAGE_ADDR, tx_bytes);
    do_read(mem, PAGE_ADDR, tx_bytes);

    auto rdata = ecc_read(PAGE_ADDR);

    check(rdata == wdata,           "ECC-T2a: 資料完整（無任何 bit flip）");
    check(ecc_total_ce() == 0,      "ECC-T2b: CE == 0");
    check(ecc_total_ue() == 0,      "ECC-T2c: UE == 0");
    printf("\n");
  }

  // ── ECC-T3：單 bit flip → CE，資料被修正 ─────────────────────────────────
  printf("-- ECC-T3: 1 bit flip → CE，ecc_read 修正 --\n");
  {
    std::vector<uint8_t> wdata(PAGE_SIZE, 0x00);
    ecc_write(PAGE_ADDR, wdata);
    do_write(mem, PAGE_ADDR, tx_bytes);

    // 翻轉 DRAM 資料晶片中 byte[0] 的 bit 0（模擬 1 bit DRAM error）
    // 這在 word 0（bytes 0..7）的 data bit 0 → DATA_POS[0] = 3 → s_hamming = 3，s_overall = 1
    dl_flip_bit(PAGE_ADDR, 0, 0);

    do_read(mem, PAGE_ADDR, tx_bytes);
    auto rdata = ecc_read(PAGE_ADDR);

    check(rdata == wdata,           "ECC-T3a: ecc_read 修正後資料與原始一致");
    check(ecc_total_ce() == 1,      "ECC-T3b: CE == 1（修正 1 次）");
    check(ecc_total_ue() == 0,      "ECC-T3c: UE == 0");
    printf("\n");
  }

  // ── ECC-T4：同一 word 2 bit flips → UE，CE 不遞增 ───────────────────────
  printf("-- ECC-T4: 同一 word 2 bit flips → UE --\n");
  {
    uint64_t ce_before = ecc_total_ce();

    std::vector<uint8_t> wdata(PAGE_SIZE, 0x00);
    ecc_write(PAGE_ADDR, wdata);
    do_write(mem, PAGE_ADDR, tx_bytes);

    // 在 word 0（bytes 0..7）翻轉 2 個 bit：byte[0] bit 0 和 byte[0] bit 1
    // data bit 0 → DATA_POS[0] = 3；data bit 1 → DATA_POS[1] = 5
    // s_hamming = 3 XOR 5 = 6（非零），s_overall = parity(0x03) = 0 → UE
    dl_flip_bit(PAGE_ADDR, 0, 0);
    dl_flip_bit(PAGE_ADDR, 0, 1);

    do_read(mem, PAGE_ADDR, tx_bytes);
    auto rdata = ecc_read(PAGE_ADDR);

    check(ecc_total_ue() == 1,               "ECC-T4a: UE == 1（偵測到 2-bit error）");
    check(ecc_total_ce() == ce_before,       "ECC-T4b: CE 未遞增（UE 不算 CE）");
    check(rdata != wdata,                    "ECC-T4c: 資料確實損壞（無法修正）");
    printf("\n");
  }

  // ── ECC-T5：多頁，CE/UE 各自累計 ────────────────────────────────────────
  printf("-- ECC-T5: 多頁 CE/UE 累計 --\n");
  {
    uint64_t ce_before = ecc_total_ce();
    uint64_t ue_before = ecc_total_ue();

    // 頁面 A: 1 bit flip → CE
    const uint64_t ADDR_A = 0 * PAGE_SIZE;
    std::vector<uint8_t> wdata_a(PAGE_SIZE, 0xAA);
    ecc_write(ADDR_A, wdata_a);
    do_write(mem, ADDR_A, tx_bytes);
    dl_flip_bit(ADDR_A, 8, 3);   // word 1（bytes 8..15），byte[8] bit 3
    do_read(mem, ADDR_A, tx_bytes);
    auto rdata_a = ecc_read(ADDR_A);
    check(rdata_a == wdata_a, "ECC-T5a: 頁面 A 的 1-bit error 被修正");

    // 頁面 B: 2 bit flips → UE
    const uint64_t ADDR_B = 1 * PAGE_SIZE;
    std::vector<uint8_t> wdata_b(PAGE_SIZE, 0x55);
    ecc_write(ADDR_B, wdata_b);
    do_write(mem, ADDR_B, tx_bytes);
    dl_flip_bit(ADDR_B, 0, 2);
    dl_flip_bit(ADDR_B, 0, 4);   // 同一 word（word 0），DATA_POS[2]=6, DATA_POS[4]=9
    do_read(mem, ADDR_B, tx_bytes);
    ecc_read(ADDR_B);

    // 頁面 C: 無 bit flip
    const uint64_t ADDR_C = 2 * PAGE_SIZE;
    std::vector<uint8_t> wdata_c(PAGE_SIZE, 0x42);
    ecc_write(ADDR_C, wdata_c);
    do_write(mem, ADDR_C, tx_bytes);
    do_read(mem, ADDR_C, tx_bytes);
    auto rdata_c = ecc_read(ADDR_C);
    check(rdata_c == wdata_c, "ECC-T5b: 頁面 C 無錯誤，資料完整");

    uint64_t new_ce = ecc_total_ce() - ce_before;
    uint64_t new_ue = ecc_total_ue() - ue_before;
    check(new_ce == 1, "ECC-T5c: 本輪 CE == 1（來自頁面 A）");
    check(new_ue == 1, "ECC-T5d: 本輪 UE == 1（來自頁面 B）");
    printf("  ce_delta=%lu  ue_delta=%lu\n\n",
           (unsigned long)new_ce, (unsigned long)new_ue);
  }

  // ── 總結 ──────────────────────────────────────────────────────────────────
  printf("=== 結果：%d pass, %d fail ===\n", g_pass, g_fail);
  printf("  total CE=%lu  UE=%lu\n",
         (unsigned long)ecc_total_ce(), (unsigned long)ecc_total_ue());

  fe->finalize();
  mem->finalize();
  return g_fail == 0 ? 0 : 1;
}
