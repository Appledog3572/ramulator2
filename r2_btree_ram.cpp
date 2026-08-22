// r2_btree_ram.cpp — RamulatorRAM 實作

#include "r2_btree_ram.hpp"
#include "SSD_core/ssd.hpp"
#include <algorithm>
#include <stdexcept>

namespace Ramulator {

// ── 建構 ─────────────────────────────────────────────────────────────────────

RamulatorRAM::RamulatorRAM(IMemorySystem* mem, SSD* ssd, uint64_t capacity_pages,
                           bool use_ecc)
    : mem_(mem),
      tCK_ns_(mem->get_tCK()),
      tx_bytes_(mem->get_tx_bytes()),
      use_ecc_(use_ecc),
      rng_(reinterpret_cast<uintptr_t>(mem) ^ 0xdeadbeef9876543ull),
      ssd_(ssd),
      capacity_pages_(capacity_pages)
{
    if (!dl_is_ready())
        throw std::runtime_error("RamulatorRAM: DataLayer plugin not ready (check YAML config)");
    if (use_ecc_ && !ecc_is_ready())
        throw std::runtime_error("RamulatorRAM: ECC plugin not ready (use_ecc=true but ECC plugin missing)");
}

// ── 同步 DRAM 操作 ────────────────────────────────────────────────────────────

void RamulatorRAM::do_write(uint64_t addr) {
    bool done = false;
    Request req(addr, Request::Type::Write, 0,
                [&done](Request&) { done = true; });
    req.size_bytes = tx_bytes_;
    req.addr_vec   = {0};   // PassThroughChannelMapper：單通道，ch=0
    while (!mem_->send(req)) mem_->tick();
    while (!done)            mem_->tick();
}

Clk_t RamulatorRAM::do_read(uint64_t addr) {
    bool  done   = false;
    Clk_t depart = -1;
    Clk_t arrive = -1;
    Request req(addr, Request::Type::Read, 0,
                [&](Request& r) { done = true; depart = r.depart; arrive = r.arrive; });
    req.size_bytes = tx_bytes_;
    req.addr_vec   = {0};
    while (!mem_->send(req)) mem_->tick();
    while (!done)            mem_->tick();
    // 回傳實際延遲（cycles）而非絕對 depart cycle
    return (arrive >= 0) ? (depart - arrive) : depart;
}

// ── LRU 輔助 ─────────────────────────────────────────────────────────────────

void RamulatorRAM::lru_touch(uint64_t page_id) {
    auto it = lru_map_.find(page_id);
    if (it != lru_map_.end()) lru_order_.erase(it->second);
    lru_order_.push_front(page_id);
    lru_map_[page_id] = lru_order_.begin();
}

void RamulatorRAM::lru_remove(uint64_t page_id) {
    auto it = lru_map_.find(page_id);
    if (it != lru_map_.end()) {
        lru_order_.erase(it->second);
        lru_map_.erase(it);
    }
}

void RamulatorRAM::maybe_evict() {
    if (!ssd_ || capacity_pages_ == 0) return;
    while (lru_map_.size() > capacity_pages_) {
        uint64_t victim = lru_order_.back();
        lru_order_.pop_back();
        lru_map_.erase(victim);
        dl_erase(page_addr(victim));   // 從 DataLayer 移除；SSD 仍有副本
    }
}

// ── IRAM 介面實作 ─────────────────────────────────────────────────────────────

RAMReadResult RamulatorRAM::read(uint64_t page_id, double /*current_time*/) {
    uint64_t addr = page_addr(page_id);

    // SSD reload：頁被 LRU 逐出後不在 RAM → 從 SSD 重載
    bool was_hit = dl_has_page(addr);
    if (ssd_ && !was_hit) {
        std::vector<uint8_t> from_ssd;
        ssd_->read(static_cast<uint32_t>(page_id), from_ssd);
        if (use_ecc_) {
            ecc_write(addr, from_ssd);   // 重建 ECC parity
        } else {
            dl_store(addr, from_ssd);    // DataLayer 作為資料來源
        }
        do_write(addr);
        if (capacity_pages_ > 0) {
            lru_touch(page_id);
            maybe_evict();
        }
    } else if (ssd_ && capacity_pages_ > 0) {
        lru_touch(page_id);
    }

    // DDR4 Read timing
    Clk_t depart = do_read(addr);

    RAMReadResult result;
    result.latency  = static_cast<double>(depart) * static_cast<double>(tCK_ns_) * 1e-9;
    result.hit      = was_hit;
    result.phys     = {0, static_cast<uint32_t>(page_id % (1u << 20))};
    result.n_silent = 0;

    if (use_ecc_) {
        uint64_t ce_before = ecc_total_ce();
        uint64_t ue_before = ecc_total_ue();
        result.data        = ecc_read(addr);
        result.n_corrected = static_cast<int>(ecc_total_ce() - ce_before);
        result.n_ecc_ue    = static_cast<int>(ecc_total_ue() - ue_before);
    } else {
        result.data        = dl_load(addr);
        result.n_corrected = 0;
        result.n_ecc_ue    = 0;
    }
    return result;
}

void RamulatorRAM::write(uint64_t page_id, const std::vector<uint8_t>& data,
                          double /*current_time*/) {
    uint64_t addr = page_addr(page_id);
    if (ssd_) ssd_->write(static_cast<uint32_t>(page_id), data);  // write-through
    if (use_ecc_) {
        ecc_write(addr, data);   // DataLayer + ECC chip parity
    } else {
        dl_store(addr, data);    // DataLayer 作為資料來源
    }
    do_write(addr);              // DDR4 Write timing
    if (ssd_ && capacity_pages_ > 0) {
        lru_touch(page_id);
        maybe_evict();
    }
}

uint64_t RamulatorRAM::allocate(double current_time) {
    uint64_t page_id = next_page_id_++;
    // 初始化零頁並寫進 DataLayer + ECC chip（write() 同時寫 SSD）
    std::vector<uint8_t> zeros(PAGE_SIZE, 0);
    write(page_id, zeros, current_time);
    return page_id;
}

void RamulatorRAM::free_page(uint64_t page_id) {
    uint64_t addr = page_addr(page_id);
    if (ssd_) ssd_->trim(static_cast<uint32_t>(page_id));
    if (ssd_ && capacity_pages_ > 0) lru_remove(page_id);
    dl_erase(addr);
}

bool RamulatorRAM::is_in_ram(uint64_t page_id) const {
    return dl_has_page(page_addr(page_id));
}

void RamulatorRAM::inject_random_flip(uint64_t page_id) {
    uint64_t addr = page_addr(page_id);
    if (!dl_has_page(addr)) return;
    int byte_pos = static_cast<int>(rng_() % PAGE_SIZE);
    int bit_pos  = static_cast<int>(rng_() % 8);
    dl_flip_bit(addr, byte_pos, bit_pos);
    // ECC 路徑：parity 未更新 → ecc_read 可偵測
    // no-ECC 路徑：dl_load 直接讀回翻後的值
}

// ── 空間故障注入（E3：row hammer / column fault）────────────────────────────
//
// 頁面 ↔ 物理位置映射（與 ram 後端一致）：
//   page_id = bank × rows_per_bank + row
//   rows_per_bank = capacity_pages_ / DRAM_NUM_BANKS
//   （capacity_pages_=0 時以已分配頁數 next_page_id_ 推算）
//
// 兩路徑均直接修改 DataLayer map；ECC parity 不更新 = 模擬 DRAM 位元翻轉。
// ecc_read 可偵測（ECC 路徑）；dl_load 直接讀回翻後值（no-ECC 路徑）。

static uint64_t r2_rows_per_bank(uint64_t capacity_pages, uint64_t next_page_id) {
    const uint64_t total = capacity_pages ? capacity_pages : next_page_id;
    if (total < DRAM_NUM_BANKS) return 1;
    return total / DRAM_NUM_BANKS;
}

void RamulatorRAM::inject_row_error(uint32_t bank, uint32_t start_row,
                                     double flip_ratio) {
    if (bank >= DRAM_NUM_BANKS) return;
    const uint64_t rpb = r2_rows_per_bank(capacity_pages_, next_page_id_);
    std::bernoulli_distribution flip_dist(flip_ratio);

    for (uint32_t r = start_row; r < start_row + 2 && (uint64_t)r < rpb; ++r) {
        const uint64_t addr = page_addr((uint64_t)bank * rpb + r);
        if (!dl_has_page(addr)) continue;
        auto data = dl_load(addr);
        for (auto& byte : data)
            for (int b = 0; b < 8; ++b)
                if (flip_dist(rng_))
                    byte ^= static_cast<uint8_t>(1u << b);
        dl_store(addr, data);  // ECC parity 未更新 = DRAM flip 語意
    }
}

void RamulatorRAM::inject_column_error(uint32_t bank, uint32_t start_row,
                                        uint32_t col_byte, uint8_t bit,
                                        uint32_t num_rows) {
    if (bank >= DRAM_NUM_BANKS || col_byte >= PAGE_SIZE || bit >= 8) return;
    const uint64_t rpb = r2_rows_per_bank(capacity_pages_, next_page_id_);
    const uint64_t end_row = std::min((uint64_t)start_row + num_rows, rpb);

    for (uint64_t r = start_row; r < end_row; ++r) {
        const uint64_t addr = page_addr((uint64_t)bank * rpb + r);
        if (!dl_has_page(addr)) continue;
        dl_flip_bit(addr, (int)col_byte, (int)bit);  // ECC parity 未更新 = DRAM flip 語意
    }
}

}  // namespace Ramulator
