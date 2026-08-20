// r2_btree_ram.cpp — RamulatorRAM 實作

#include "r2_btree_ram.hpp"
#include "SSD_core/ssd.hpp"
#include <stdexcept>

namespace Ramulator {

// ── 建構 ─────────────────────────────────────────────────────────────────────

RamulatorRAM::RamulatorRAM(IMemorySystem* mem, SSD* ssd, uint64_t capacity_pages)
    : mem_(mem),
      tCK_ns_(mem->get_tCK()),
      tx_bytes_(mem->get_tx_bytes()),
      ssd_(ssd),
      capacity_pages_(capacity_pages)
{
    if (!ecc_is_ready())
        throw std::runtime_error("RamulatorRAM: ECC plugin not ready (check YAML config)");
    if (!dl_is_ready())
        throw std::runtime_error("RamulatorRAM: DataLayer plugin not ready (check YAML config)");
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
        dl_erase(page_addr(victim));   // 只從 DataLayer 移除；SSD 仍有副本
    }
}

// ── IRAM 介面實作 ─────────────────────────────────────────────────────────────

RAMReadResult RamulatorRAM::read(uint64_t page_id, double /*current_time*/) {
    uint64_t addr = page_addr(page_id);

    // SSD reload：頁被 LRU 逐出後不在 DataLayer → 從 SSD 重載
    bool was_hit = dl_has_page(addr);
    if (ssd_ && !was_hit) {
        std::vector<uint8_t> from_ssd;
        ssd_->read(static_cast<uint32_t>(page_id), from_ssd);
        ecc_write(addr, from_ssd);   // 重建 ECC parity
        do_write(addr);              // DDR4 Write timing（載入 DataLayer）
        if (capacity_pages_ > 0) {
            lru_touch(page_id);      // 先 touch 新頁
            maybe_evict();           // 再踢出舊頁（確保新頁不是 victim）
        }
    } else if (ssd_ && capacity_pages_ > 0) {
        lru_touch(page_id);          // 頁已在 DataLayer：更新 LRU 位置
    }

    // 1. 送 DDR4 Read request（驅動 Ramulator2 timing，同時觸發 BitFlip pre_schedule）
    Clk_t depart = do_read(addr);

    // 2. 透過 ECC plugin 讀出（DataLayer → syndrome decode → 自動修正）
    uint64_t ce_before = ecc_total_ce();
    uint64_t ue_before = ecc_total_ue();
    auto corrected_data = ecc_read(addr);
    int n_ce = static_cast<int>(ecc_total_ce() - ce_before);
    int n_ue = static_cast<int>(ecc_total_ue() - ue_before);

    RAMReadResult result;
    result.data        = std::move(corrected_data);
    result.hit         = was_hit;
    result.latency     = static_cast<double>(depart) * static_cast<double>(tCK_ns_) * 1e-9;
    result.n_corrected = n_ce;
    result.n_ecc_ue    = n_ue;
    result.n_silent    = 0;   // SECDED 不模擬 miscorrection（3-bit → 視為 CE）
    result.phys        = {0, static_cast<uint32_t>(page_id % (1u << 20))};
    return result;
}

void RamulatorRAM::write(uint64_t page_id, const std::vector<uint8_t>& data,
                          double /*current_time*/) {
    uint64_t addr = page_addr(page_id);
    if (ssd_) ssd_->write(static_cast<uint32_t>(page_id), data);  // write-through
    ecc_write(addr, data);   // DataLayer 儲存 + ECC chip 計算 parity
    do_write(addr);          // DDR4 Write timing
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
    dl_erase(addr);   // 從 DataLayer 刪除（ECC chip 的 parity 殘留但不影響正確性）
}

bool RamulatorRAM::is_in_ram(uint64_t page_id) const {
    return dl_has_page(page_addr(page_id));
}

void RamulatorRAM::inject_random_flip(uint64_t page_id) {
    // 只翻在 DataLayer 的頁（已逐出到 SSD 的頁不在 DRAM 中）
    if (dl_has_page(page_addr(page_id)))
        dl_random_flip(page_addr(page_id));
}

}  // namespace Ramulator
