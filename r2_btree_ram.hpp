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
#include <list>
#include <random>
#include <unordered_map>
#include <vector>

// V2 抽象介面（IRAM + RAMReadResult / DRAMAddress）
#include "new_RAM_core/new_ram.hpp"

// 前置宣告（避免在 hpp 拉入整個 SSD 標頭）
class SSD;

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
    // 對應 YAML 中需同時載入 DataLayer plugin；use_ecc=true 時還需 ECC plugin。
    //
    // ssd（可 nullptr）: write-through 後端 + crash_rescue 乾淨副本來源。
    // capacity_pages（0 = 無上限）: DataLayer 頁數上限；超過時 LRU 逐出到 SSD。
    // use_ecc（預設 true）: 是否透過 ECC plugin 讀寫（true）或直接走 DataLayer
    //   raw read/write（false）。false 時 flip 可直接穿透到 GuardedRAM，
    //   行為更接近 ram 後端（無硬體 ECC 保護），適合 E1–E4 的 vulnerability 量測。
    explicit RamulatorRAM(IMemorySystem* mem,
                          SSD*     ssd            = nullptr,
                          uint64_t capacity_pages = 0,
                          bool     use_ecc        = true);

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
    // 模擬 row hammer：連續 2 row 內以 flip_ratio 機率翻 bit
    void inject_row_error(uint32_t bank, uint32_t start_row,
                          double flip_ratio = 0.5) override;
    // 模擬 column（bit line）故障：連續 num_rows 個 row 各翻 (col_byte, bit)
    void inject_column_error(uint32_t bank, uint32_t start_row,
                              uint32_t col_byte, uint8_t bit,
                              uint32_t num_rows = 512) override;

    // ── 查詢 ──────────────────────────────────────────────────────
    float    tCK_ns()    const { return tCK_ns_; }
    int      tx_bytes()  const { return tx_bytes_; }
    uint64_t page_count()const { return next_page_id_; }

private:
    IMemorySystem* mem_;
    float    tCK_ns_;
    int      tx_bytes_;
    uint64_t next_page_id_ = 0;
    bool     use_ecc_      = true;  // 是否透過 ECC plugin 讀寫
    std::mt19937 rng_;              // inject_random_flip 用（選 byte/bit 位置）

    // SSD 後端（可 nullptr）
    SSD*     ssd_            = nullptr;
    uint64_t capacity_pages_ = 0;   // 0 = 無上限

    // LRU 追蹤（僅 ssd_ != nullptr 且 capacity_pages_ > 0 時有效）
    std::list<uint64_t>                                        lru_order_;
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_map_;

    // !use_ecc_ 時的資料真實來源（DataLayer 的 staging 不保證與 dl_load 一致）
    std::unordered_map<uint64_t, std::vector<uint8_t>> page_data_;

    static constexpr uint32_t PAGE_SIZE = 4096;

    uint64_t page_addr(uint64_t page_id) const {
        return page_id * static_cast<uint64_t>(PAGE_SIZE);
    }

    // 同步 Write：送 request 並等待 callback
    void   do_write(uint64_t addr);
    // 同步 Read：送 request 並等待 callback，回傳 depart−arrive cycles
    Clk_t  do_read (uint64_t addr);

    // LRU 輔助
    void lru_touch (uint64_t page_id);
    void lru_remove(uint64_t page_id);
    void maybe_evict();   // 若超過 capacity_pages_ 則踢出 LRU 頁到 SSD
};

}  // namespace Ramulator
