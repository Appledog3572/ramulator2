// ecc_plugin.cpp — Step 2b: ECC plugin（Hamming SECDED）
//
// 架構：
//   DataLayer     = DRAM 資料晶片（存放明文，BitFlip 可以翻轉這裡的 bit）
//   ECCPlugin     = 獨立的 ECC 晶片（存放 parity，不受 BitFlip 影響）
//
// 演算法：Hamming(72,64) SECDED
//   每 8 data bytes（64 bits）→ 7 個 Hamming parity bits + 1 overall parity bit
//   = 8 check bits（1 byte）
//   → 一個 4096-byte 頁面 = 512 個 8-byte word → 512 bytes check（存在 m_parity）
//
//   單 bit 錯誤（CE）：定位並修正
//   雙 bit 錯誤（UE）：偵測到，無法修正
//
// 外部 API（透過 ecc_plugin_api.h）：
//   ecc_is_ready()
//   ecc_write(page_addr, data)   — 計算 parity、dl_store、存 ECC chip
//   ecc_read(page_addr)          — dl_load、syndrome decode、回傳修正後資料
//   ecc_total_ce()               — 累計修正次數
//   ecc_total_ue()               — 累計無法修正次數

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>
#include <stdexcept>

#include "ramulator/base/base.h"
#include "ramulator/base/param.h"
#include "ramulator/base/request.h"
#include "ramulator/controller/controller_base.h"
#include "ramulator/controller/plugin/i_controller_plugin.h"
#include "ramulator/controller/plugin/impl/data_layer_api.h"

namespace Ramulator {

// ── Hamming(72,64) SECDED ────────────────────────────────────────────────────
//
// 72-bit codeword = 64 data bits + 7 Hamming parity bits + 1 overall parity bit
//
// Hamming parity bit k（k=0..6）位於 codeword 位置 2^k（1-indexed）。
// 其餘 64 個位置（非 2 的次方，1..71 範圍內）為 data bits：
//
//   DATA_POS[i] = data bit i 在 codeword 中的 1-indexed 位置
//
// Hamming parity bit k 覆蓋所有位置中第 k 個 bit 為 1 的 data bits。
// overall parity bit p7 = XOR of all 71 other bits（確保 72-bit codeword 整體 parity = 0）。

// 1..71 範圍內的非 2^k 位置（共 64 個）
static const uint8_t DATA_POS[64] = {
     3,  5,  6,  7,                                              // 4
     9, 10, 11, 12, 13, 14, 15,                                  // 7
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, // 15
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, // 15
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, // 16
    65, 66, 67, 68, 69, 70, 71                                   // 7
};
// 總計 4+7+15+15+16+7 = 64 ✓

// 計算 64-bit word 的 8-bit check value
//   check[0..6] = Hamming parity bits（bit k covers data bits where bit k of DATA_POS[i] is set）
//   check[7]    = overall parity（讓整個 72-bit codeword 的 XOR = 0）
static uint8_t hamming_check(uint64_t word) {
    uint8_t p = 0;
    for (int i = 0; i < 64; i++) {
        if ((word >> i) & 1ULL)
            p ^= DATA_POS[i];  // DATA_POS[i] < 128，只影響 bits 0..6
    }
    // p[7] = parity(word) XOR parity(p[0..6])
    // 理由：total_codeword_parity = parity(data) XOR parity(p[0..6]) XOR p7 = 0
    uint8_t overall = static_cast<uint8_t>(
        (__builtin_parityll(word) ^ (__builtin_popcount(p) & 1)) & 1
    );
    return static_cast<uint8_t>(p | (overall << 7));
}

// 解碼並原位修正 word。回傳：0=無錯, 1=CE（已修正）, 2=UE（無法修正）
static int hamming_decode(uint64_t& word, uint8_t stored_check) {
    // Hamming syndrome：recomputed Hamming bits XOR stored Hamming bits
    uint8_t s_hamming = static_cast<uint8_t>(
        (hamming_check(word) ^ stored_check) & 0x7F
    );
    // Overall syndrome：parity(received_data) XOR parity(stored_check)
    // = parity(error_mask) → 0 表示偶數 bit 錯誤，1 表示奇數 bit 錯誤
    uint8_t s_overall = static_cast<uint8_t>(
        (__builtin_parityll(word) ^ __builtin_parity(stored_check)) & 1
    );

    if (s_hamming == 0 && s_overall == 0) return 0;  // 無錯誤

    if (s_hamming == 0 && s_overall == 1) {
        // 只有 overall parity bit 出錯 → data 正確，ECC chip 內部 bit flip
        return 1;  // CE（data 不需修正）
    }

    if (s_overall == 1) {
        // 單 bit 錯誤 — 找出對應的 data bit 並修正
        for (int i = 0; i < 64; i++) {
            if (DATA_POS[i] == s_hamming) {
                word ^= (1ULL << i);
                return 1;  // CE（data 已修正）
            }
        }
        // s_hamming 對應的是某個 Hamming parity bit 位置（2 的次方）
        // → data 正確，只是 Hamming parity bit 出錯
        return 1;  // CE（data 不需修正）
    }

    // s_overall == 0 且 s_hamming != 0 → 偶數 bit 錯誤，無法修正（UE）
    return 2;
}

// ── ECCPlugin ────────────────────────────────────────────────────────────────

class ECCPlugin : public IControllerPlugin, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, ECCPlugin, "ECC")

 private:
  bool m_debug = false;

  // ECC 晶片：page_addr → vector<uint8_t>，每 byte = 一個 8-byte word 的 check value
  // 4096-byte 頁面 → 512 個 word → 512 bytes check
  std::map<uint64_t, std::vector<uint8_t>> m_parity;

  uint64_t m_total_ce = 0;
  uint64_t m_total_ue = 0;

 public:
  // ── 公開方法（由 ecc_* 自由函式包裝）──────────────────────────────────────

  void write(uint64_t page_addr, const std::vector<uint8_t>& data) {
    if (data.size() % 8 != 0)
      throw std::runtime_error("ECCPlugin::write: data size must be multiple of 8");

    int n_words = static_cast<int>(data.size() / 8);
    std::vector<uint8_t> checks(n_words);

    for (int w = 0; w < n_words; w++) {
      uint64_t word = 0;
      for (int b = 0; b < 8; b++)
        word |= static_cast<uint64_t>(data[w * 8 + b]) << (b * 8);
      checks[w] = hamming_check(word);
    }

    dl_store(page_addr, data);       // 寫進 DRAM（DataLayer）
    m_parity[page_addr] = std::move(checks);  // 存進 ECC 晶片

    if (m_debug)
      printf("[ECC] write addr=0x%016lx words=%d\n",
             (unsigned long)page_addr, n_words);
  }

  std::vector<uint8_t> read(uint64_t page_addr) {
    auto raw = dl_load(page_addr);  // 從 DRAM 讀出（可能有 bit flip）

    auto pit = m_parity.find(page_addr);
    if (pit == m_parity.end())
      throw std::runtime_error("ECCPlugin::read: no parity for page_addr");

    const auto& checks = pit->second;
    int n_words = static_cast<int>(raw.size() / 8);

    for (int w = 0; w < n_words; w++) {
      uint64_t word = 0;
      for (int b = 0; b < 8; b++)
        word |= static_cast<uint64_t>(raw[w * 8 + b]) << (b * 8);

      int status = hamming_decode(word, checks[w]);

      if (status == 1) {
        m_total_ce++;
        // 把修正後的 word 寫回 raw
        for (int b = 0; b < 8; b++)
          raw[w * 8 + b] = static_cast<uint8_t>((word >> (b * 8)) & 0xFF);
        if (m_debug)
          printf("[ECC] CE corrected at word=%d total_ce=%lu\n",
                 w, (unsigned long)m_total_ce);
      } else if (status == 2) {
        m_total_ue++;
        if (m_debug)
          printf("[ECC] UE detected at word=%d total_ue=%lu\n",
                 w, (unsigned long)m_total_ue);
        // data 原樣回傳（已損壞）
      }
    }

    return raw;
  }

  uint64_t total_ce() const { return m_total_ce; }
  uint64_t total_ue() const { return m_total_ue; }

  // ── Ramulator2 lifecycle ─────────────────────────────────────────────────

  void init() override {
    RAMULATOR_PARSE_PARAM(m_debug, bool, "debug").default_val(false);
  }

  void setup(IFrontEnd* /*fe*/, IMemorySystem* /*ms*/) override {
    s_instance = this;
    if (m_debug) printf("[ECC] setup() done.\n");
  }

  // ── 單例 ─────────────────────────────────────────────────────────────────
  static ECCPlugin*  s_instance;
  static ECCPlugin*  instance() { return s_instance; }
};

ECCPlugin* ECCPlugin::s_instance = nullptr;

// ── 自由函式 ─────────────────────────────────────────────────────────────────

bool ecc_is_ready() {
  return ECCPlugin::instance() != nullptr;
}

void ecc_write(uint64_t page_addr, const std::vector<uint8_t>& data) {
  ECCPlugin::instance()->write(page_addr, data);
}

std::vector<uint8_t> ecc_read(uint64_t page_addr) {
  return ECCPlugin::instance()->read(page_addr);
}

uint64_t ecc_total_ce() { return ECCPlugin::instance()->total_ce(); }
uint64_t ecc_total_ue() { return ECCPlugin::instance()->total_ue(); }

}  // namespace Ramulator
