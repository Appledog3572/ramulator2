#pragma once
// bit_flip_api.h — BitFlip plugin 的外部 C++ API
//
// test 程式只需 include 這個標頭，不需知道 BitFlipPlugin 的完整定義。
// 這些函式由 bit_flip.cpp 實作（編譯進 libramulator）。
//
// 使用前提：
//   1. YAML config 中同時載入 DataLayer 和 BitFlip plugin
//   2. memory_system->connect_frontend() 呼叫後，bf_is_ready() 才為 true

#include <cstdint>

namespace Ramulator {

// plugin 是否初始化完成
bool bf_is_ready();

// 手動注入：翻轉 page_addr 頁面第 byte_pos 個 byte 的第 bit_pos 個 bit
// （byte_pos ∈ [0, PAGE_SIZE)，bit_pos ∈ [0, 7]）
void bf_inject(uint64_t page_addr, int byte_pos, int bit_pos);

// 手動注入隨機 bit flip（使用 plugin 內部 RNG）
// 回傳被翻轉的頁面位址，若無頁面則回傳 UINT64_MAX
uint64_t bf_inject_random();

// 累計注入次數（自動 + 手動）
uint64_t bf_total_flips();

// 執行期動態調整自動注入間隔（ticks；0 = 停用）
void bf_set_interval(uint64_t n);

}  // namespace Ramulator
