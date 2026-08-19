#pragma once
// data_layer_api.h — DataLayer plugin 的外部 C++ API
//
// test 程式只需 include 這個標頭，不需知道 DataLayerPlugin 的完整定義。
// 這些函式由 data_layer.cpp 實作（編譯進 libramulator）。
//
// 使用前提：必須先透過 YAML config 載入 DataLayer plugin（impl: DataLayer），
// 並呼叫 memory_system->connect_frontend() 完成 setup()，
// 之後 dl_is_ready() 才會回傳 true。

#include <cstdint>
#include <cstddef>
#include <vector>

namespace Ramulator {

// setup() 是否完成（DataLayer plugin 是否成功初始化）
bool dl_is_ready();

// 儲存一整頁資料（應在送出 Write request 前呼叫）
// page_addr = page_id * PAGE_SIZE（必須和 Request.addr 對齊）
void dl_store(uint64_t page_addr, const std::vector<uint8_t>& data);

// 讀取一整頁資料的副本（應在 Read callback 觸發後呼叫）
// 若 page_addr 不存在則拋出 std::runtime_error
std::vector<uint8_t> dl_load(uint64_t page_addr);

// 查詢 page_addr 是否有資料
bool dl_has_page(uint64_t page_addr);

// 刪除（頁面被逐出時呼叫）
void dl_erase(uint64_t page_addr);

// 目前儲存的頁面數量
size_t dl_page_count();

}  // namespace Ramulator
