#pragma once
// r2_btree_ram.hpp — RamulatorRAM：用 Ramulator2 DDR4 模擬器實作 IRAM 介面
//
// 架構：
//   BTree → IRAM* → RamulatorRAM → Ramulator2 IMemorySystem
//                                    ├─ DataLayer plugin  （儲存 B-Tree page 資料）
//                                    ├─ BitFlip  plugin  （週期性注入 DRAM bit flip）
//                                    └─ ECC      plugin  （透明修正 1-bit error）
//
// 映射：
//   page_id → DRAM 位址 = page_id × PAGE_SIZE（線性，不用 bank/row 管理）
//   page 的 parity 由 ECC plugin 維護（模擬獨立 ECC 晶片）
//
// 與 v2/RAM 的差異：
//   - 無 SSD 後端（純 DRAM，不做 LRU 逐出）
//   - 讀寫延遲來自真實 DDR4 時序（Ramulator2 depart cycle × tCK）
//   - ECC 由 ECC plugin 處理（不用 v2/ECC_RAM）

#pragma once
#include <cstdint>
#include <vector>

// V2 抽象介面（IRAM + RAMReadResult / DRAMAddress）
#include "new_RAM_core/new_ram.hpp"

// Ramulator2
#include "ramulator/base/base.h"
#include "ramulator/base/request.h"
#include "ramulator/memory_system/i_memory_system.h"
#include "ramulator/controller/plugin/impl/data_layer_api.h"
#include "ramulator/controller/plugin/impl/ecc_plugin_api.h"

namespace Ramulator {

class RamulatorRAM : public IRAM {
public:
    // mem 必須已完成 connect_frontend() / connect_memory_system()
    // 對應 YAML 中需同時載入 DataLayer + ECC plugin
    explicit RamulatorRAM(IMemorySystem* mem);

    // ── IRAM 介面 ─────────────────────────────────────────────────
    RAMReadResult read(uint64_t page_id, double current_time) override;
    void          write(uint64_t page_id,
                        const std::vector<uint8_t>& data,
                        double current_time) override;
    uint64_t      allocate(double current_time) override;
    void          free_page(uint64_t page_id)   override;

    // ── IRAM 擴充（GuardedRAM / FaultInjector 使用）──────────────────
    // DataLayer 有此頁資料 → 視為 in RAM
    bool is_in_ram(uint64_t page_id) const override;
    // 隨機翻一個 bit（透過 DataLayer API）
    void inject_random_flip(uint64_t page_id) override;
    // inject_row_error / inject_column_error 用 IRAM 預設空實作

    // ── 查詢 ──────────────────────────────────────────────────────
    float    tCK_ns()    const { return tCK_ns_; }
    int      tx_bytes()  const { return tx_bytes_; }
    uint64_t page_count()const { return next_page_id_; }

private:
    IMemorySystem* mem_;
    float    tCK_ns_;
    int      tx_bytes_;
    uint64_t next_page_id_ = 0;

    static constexpr uint32_t PAGE_SIZE = 4096;

    uint64_t page_addr(uint64_t page_id) const {
        return page_id * static_cast<uint64_t>(PAGE_SIZE);
    }

    // 同步 Write：送 request 並等待 callback
    void   do_write(uint64_t addr);
    // 同步 Read：送 request 並等待 callback，回傳 depart cycle
    Clk_t  do_read (uint64_t addr);
};

}  // namespace Ramulator
