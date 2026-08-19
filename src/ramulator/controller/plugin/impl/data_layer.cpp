// data_layer.cpp — Step 1: 資料層 plugin
//
// 功能：在 Ramulator2 的 controller plugin 框架內維護一個頁面資料 map。
// Ramulator2 本身只做時序/狀態機模擬，沒有任何 data bytes。
// 本 plugin 額外儲存 (page_addr → vector<uint8_t>) 的對應，
// 供上層 wrapper 在請求完成後存取實際資料內容。
//
// 外部 API（透過 data_layer_api.h 存取，不暴露 class 定義）：
//   dl_store(addr, data)  — 儲存頁面（在送出 Write request 前呼叫）
//   dl_load(addr)         — 讀取頁面（在 Read callback 後呼叫）
//   dl_has_page(addr)     — 查詢是否存在
//   dl_erase(addr)        — 刪除（頁面被逐出時）
//   dl_is_ready()         — setup() 後才為 true
//
// 注意：key = page_base_addr（page_id * PAGE_SIZE），與 Ramulator2
// Request.addr 的對齊方式由 wrapper 負責一致。

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <stdexcept>

#include "ramulator/base/base.h"
#include "ramulator/base/param.h"
#include "ramulator/base/request.h"
#include "ramulator/controller/controller_base.h"
#include "ramulator/controller/plugin/i_controller_plugin.h"

namespace Ramulator {

// ── Plugin 類別 ──────────────────────────────────────────────────────────────

class DataLayerPlugin : public IControllerPlugin, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, DataLayerPlugin, "DataLayer")

 private:
  ControllerBase* m_ctrl       = nullptr;
  uint32_t        m_page_size  = 4096;
  bool            m_debug      = false;

  // final_command id（用於 on_issue debug log）
  int m_rd_cmd = -1;
  int m_wr_cmd = -1;

  // 資料 map：page_base_addr → 頁面 bytes
  std::unordered_map<uint64_t, std::vector<uint8_t>> m_pages;

 public:
  // ── 外部 API（由 dl_* 自由函式包裝，test 程式透過那些函式存取）────────────

  void store(uint64_t addr, const std::vector<uint8_t>& data) {
    m_pages[addr] = data;
  }

  std::vector<uint8_t> load(uint64_t addr) const {
    auto it = m_pages.find(addr);
    if (it == m_pages.end())
      throw std::runtime_error("DataLayer: load() addr not found");
    return it->second;  // return copy
  }

  bool has_page(uint64_t addr) const {
    return m_pages.count(addr) > 0;
  }

  void erase(uint64_t addr) {
    m_pages.erase(addr);
  }

  size_t page_count() const { return m_pages.size(); }

  // ── Ramulator2 lifecycle ─────────────────────────────────────────────────

  void init() override {
    RAMULATOR_PARSE_PARAM(m_page_size, uint32_t, "page_size").default_val(4096u);
    RAMULATOR_PARSE_PARAM(m_debug,     bool,     "debug"    ).default_val(false);
  }

  void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
    m_ctrl   = cast_parent<ControllerBase>();
    auto* spec = m_ctrl->m_device.m_spec;
    m_rd_cmd = spec->get_command_id("RD");
    m_wr_cmd = spec->get_command_id("WR");

    // 全域指標供 dl_* 自由函式使用
    s_instance = this;

    if (m_debug)
      printf("[DataLayer] setup() done. page_size=%u, rd_cmd=%d, wr_cmd=%d\n",
             m_page_size, m_rd_cmd, m_wr_cmd);
  }

  void on_issue(const Request& req) override {
    if (!m_debug) return;
    if (req.command != req.final_command) return;
    if (req.type_id == Request::Type::Read)
      printf("[DataLayer] READ  final cmd issued  addr=0x%016lx\n",
             (unsigned long)req.addr);
    else if (req.type_id == Request::Type::Write)
      printf("[DataLayer] WRITE final cmd issued  addr=0x%016lx\n",
             (unsigned long)req.addr);
  }

  // ── 單例存取 ─────────────────────────────────────────────────────────────
  static DataLayerPlugin* s_instance;
  static DataLayerPlugin* instance() { return s_instance; }
};

DataLayerPlugin* DataLayerPlugin::s_instance = nullptr;

// ── 自由函式（暴露給 test 程式，避免 test 需引入完整 class 定義）────────────

bool dl_is_ready() {
  return DataLayerPlugin::instance() != nullptr;
}

void dl_store(uint64_t page_addr, const std::vector<uint8_t>& data) {
  DataLayerPlugin::instance()->store(page_addr, data);
}

std::vector<uint8_t> dl_load(uint64_t page_addr) {
  return DataLayerPlugin::instance()->load(page_addr);
}

bool dl_has_page(uint64_t page_addr) {
  return DataLayerPlugin::instance()->has_page(page_addr);
}

void dl_erase(uint64_t page_addr) {
  DataLayerPlugin::instance()->erase(page_addr);
}

size_t dl_page_count() {
  return DataLayerPlugin::instance()->page_count();
}

}  // namespace Ramulator
