// bit_flip.cpp — Step 2a: BitFlip plugin
//
// 功能：在 DataLayer 管理的頁面資料中注入 bit flip，
// 模擬 DRAM 中因為電荷流失、宇宙射線等造成的資料損壞。
//
// 注入模式：
//   1. 自動（週期性）：每隔 flip_interval ticks，呼叫 dl_random_flip() 隨機翻一個 bit。
//      flip_interval = 0（預設）表示停用自動注入。
//   2. 手動（由外部透過 bf_inject() 指定頁面與位置）。
//
// 外部 API（透過 bit_flip_api.h）：
//   bf_is_ready()                          — plugin 是否初始化完成
//   bf_inject(addr, byte_pos, bit_pos)     — 手動注入單一 bit flip
//   bf_inject_random()                     — 手動注入隨機 bit flip
//   bf_total_flips()                       — 累計注入次數
//   bf_set_interval(n)                     — 執行期動態調整 flip_interval
//
// 依賴：
//   - DataLayer plugin 必須同時載入且先完成 setup()
//   - data_layer_api.h 中的 dl_flip_bit() / dl_random_flip()

#include <cstdint>
#include <cstdio>
#include <random>

#include "ramulator/base/base.h"
#include "ramulator/base/param.h"
#include "ramulator/base/request.h"
#include "ramulator/controller/controller_base.h"
#include "ramulator/controller/plugin/i_controller_plugin.h"
#include "ramulator/controller/plugin/impl/data_layer_api.h"

namespace Ramulator {

// ── Plugin 類別 ──────────────────────────────────────────────────────────────

class BitFlipPlugin : public IControllerPlugin, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, BitFlipPlugin, "BitFlip")

 private:
  uint64_t m_flip_interval = 0;   // 0 = 停用自動注入；> 0 = 每 N ticks 注入一次
  uint64_t m_seed          = 42;
  bool     m_debug         = false;

  uint64_t         m_tick_count  = 0;
  uint64_t         m_total_flips = 0;
  std::mt19937_64  m_rng;

 public:
  // ── 公開方法（由 bf_* 自由函式包裝）──────────────────────────────────────

  void inject(uint64_t page_addr, int byte_pos, int bit_pos) {
    dl_flip_bit(page_addr, byte_pos, bit_pos);
    m_total_flips++;
    if (m_debug)
      printf("[BitFlip] inject addr=0x%016lx byte=%d bit=%d total=%lu\n",
             (unsigned long)page_addr, byte_pos, bit_pos,
             (unsigned long)m_total_flips);
  }

  uint64_t inject_random() {
    uint64_t rand_val = m_rng();
    uint64_t flipped_addr = dl_random_flip(rand_val);
    if (flipped_addr != UINT64_MAX) {
      m_total_flips++;
      if (m_debug)
        printf("[BitFlip] random flip addr=0x%016lx total=%lu\n",
               (unsigned long)flipped_addr, (unsigned long)m_total_flips);
    }
    return flipped_addr;
  }

  uint64_t total_flips() const { return m_total_flips; }

  void set_interval(uint64_t n) { m_flip_interval = n; }

  // ── Ramulator2 lifecycle ─────────────────────────────────────────────────

  void init() override {
    RAMULATOR_PARSE_PARAM(m_flip_interval, uint64_t, "flip_interval").default_val(0ULL);
    RAMULATOR_PARSE_PARAM(m_seed,          uint64_t, "seed"         ).default_val(42ULL);
    RAMULATOR_PARSE_PARAM(m_debug,         bool,     "debug"        ).default_val(false);
    m_rng.seed(m_seed);
  }

  void setup(IFrontEnd* /*frontend*/, IMemorySystem* /*memory_system*/) override {
    s_instance = this;
    if (m_debug)
      printf("[BitFlip] setup() done. flip_interval=%lu seed=%lu\n",
             (unsigned long)m_flip_interval, (unsigned long)m_seed);
  }

  void pre_schedule() override {
    if (m_flip_interval == 0) return;
    m_tick_count++;
    if (m_tick_count % m_flip_interval != 0) return;
    if (!dl_is_ready()) return;
    inject_random();
  }

  // ── 單例存取 ─────────────────────────────────────────────────────────────
  static BitFlipPlugin*  s_instance;
  static BitFlipPlugin*  instance() { return s_instance; }
};

BitFlipPlugin* BitFlipPlugin::s_instance = nullptr;

// ── 自由函式 ─────────────────────────────────────────────────────────────────

bool bf_is_ready() {
  return BitFlipPlugin::instance() != nullptr;
}

void bf_inject(uint64_t page_addr, int byte_pos, int bit_pos) {
  BitFlipPlugin::instance()->inject(page_addr, byte_pos, bit_pos);
}

uint64_t bf_inject_random() {
  return BitFlipPlugin::instance()->inject_random();
}

uint64_t bf_total_flips() {
  return BitFlipPlugin::instance()->total_flips();
}

void bf_set_interval(uint64_t n) {
  BitFlipPlugin::instance()->set_interval(n);
}

}  // namespace Ramulator
