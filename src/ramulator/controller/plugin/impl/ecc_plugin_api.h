#pragma once
// ecc_plugin_api.h — ECCPlugin 的外部 C++ API
//
// test 程式只需 include 這個標頭，不需知道 ECCPlugin 的完整定義。
// 這些函式由 ecc_plugin.cpp 實作（編譯進 libramulator）。
//
// 使用前提：
//   1. YAML config 同時載入 DataLayer 和 ECC plugin（impl: DataLayer, impl: ECC）
//   2. DataLayer 必須在 ECC 之前出現（ECC 的 write/read 內部呼叫 dl_store/dl_load）
//   3. memory_system->connect_frontend() 呼叫後，ecc_is_ready() 才為 true
//
// 資料流：
//   test 呼叫 ecc_write(addr, data)
//     ↓ ECCPlugin 計算 Hamming parity，呼叫 dl_store，parity 存 ECC 晶片（plugin 內部）
//   BitFlip / dl_flip_bit 翻轉 DataLayer 中的 bit（DRAM 資料晶片）
//   test 呼叫 ecc_read(addr)
//     ↓ ECCPlugin 呼叫 dl_load，用 ECC 晶片 parity 計算 syndrome，修正並回傳

#include <cstdint>
#include <vector>

namespace Ramulator {

// plugin 是否初始化完成
bool ecc_is_ready();

// 寫入一整頁資料：計算 Hamming check → dl_store → 存 ECC chip
// data.size() 必須是 8 的倍數（= PAGE_SIZE）
void ecc_write(uint64_t page_addr, const std::vector<uint8_t>& data);

// 讀取一整頁資料：dl_load → syndrome decode → 修正後回傳
// CE：自動修正並計數（ecc_total_ce()）
// UE：計數（ecc_total_ue()），回傳未修正的損壞資料
std::vector<uint8_t> ecc_read(uint64_t page_addr);

// 累計 corrected errors（1-bit 錯誤被修正的次數）
uint64_t ecc_total_ce();

// 累計 uncorrectable errors（2-bit 同 word 錯誤被偵測的次數）
uint64_t ecc_total_ue();

}  // namespace Ramulator
