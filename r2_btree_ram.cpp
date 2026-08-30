// r2_btree_ram.cpp — RamulatorRAM 實作

#include "r2_btree_ram.hpp"
#include "SSD_core/ssd.hpp"
#include <algorithm>
#include <cmath>       // std::isinf（TTL 政策）
#include <stdexcept>

namespace Ramulator {

// ── 建構 ─────────────────────────────────────────────────────────────────────

RamulatorRAM::RamulatorRAM(IMemorySystem* mem, SSD* ssd, uint64_t capacity_pages,
                           bool use_ecc, uint32_t seed)
    : mem_(mem),
      tCK_ns_(mem->get_tCK()),
      tx_bytes_(mem->get_tx_bytes()),
      use_ecc_(use_ecc),
      // ⚠️ 舊版用 reinterpret_cast<uintptr_t>(mem) 當種子 —— 那是物件的記憶體位址，
      //    受 ASLR 影響，**同一組參數每次執行都會得到不同結果**。
      //    rng_ 決定 inject_random_flip 的 byte/bit 位置，因此整個損毀樣態不可重現。
      //    實測（Batch 2 B2-6）：同 seed 連跑兩次 fn = 5450 / 5265、
      //    n_wrong_total = 198 / 2，差異極大。
      //    改為由呼叫端明確給定種子。
      rng_(seed ^ 0xdeadbeefu),
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
        if (!use_ecc_) page_data_.erase(victim);
        dl_erase(page_addr(victim));   // 從 DataLayer 移除；SSD 仍有副本
        load_time_.erase(victim);
        last_access_.erase(victim);
        stats_.n_lru_evictions++;
    }
}

// ── TTL ──────────────────────────────────────────────────────────────────────
//
// 語意與 ram 後端（RAM::check_ttl，config_.ttl_residency=true）一致：
//   residency 到期判定 —— t − load_time > ttl 即逐出，不論讀取頻率。
//   政策持久化 —— ttl_policy_ 綁 page_id，逐出重載後自動生效。

void RamulatorRAM::evict_page(uint64_t page_id) {
    const uint64_t addr = page_addr(page_id);
    if (!use_ecc_) page_data_.erase(page_id);
    dl_erase(addr);                 // 從 DataLayer 移除；SSD 仍有乾淨副本
    lru_remove(page_id);
    load_time_.erase(page_id);
    last_access_.erase(page_id);
}

bool RamulatorRAM::check_ttl(uint64_t page_id, double t) {
    auto pit = ttl_policy_.find(page_id);
    if (pit == ttl_policy_.end()) return false;          // 非 cold page
    const double ttl = pit->second;
    if (std::isinf(ttl)) return false;

    const auto& base_map = ttl_residency_ ? load_time_ : last_access_;
    auto bit = base_map.find(page_id);
    if (bit == base_map.end()) return false;             // 不在 RAM

    if ((t - bit->second) <= ttl) return false;          // 未到期

    evict_page(page_id);
    stats_.n_ttl_evictions++;
    return true;
}

void RamulatorRAM::note_load(uint64_t page_id, double t) {
    load_time_[page_id]   = t;   // 曝露窗 T_reload 歸零
    last_access_[page_id] = t;
}

void RamulatorRAM::set_ttl(uint64_t page_id, double ttl_seconds) {
    if (std::isinf(ttl_seconds)) ttl_policy_.erase(page_id);
    else                         ttl_policy_[page_id] = ttl_seconds;
    // 無需立即套用：check_ttl 每次讀取時直接查 ttl_policy_，
    // 因此政策天然是持久的（不像 ram 後端把狀態存在 slot 裡）。
}

double RamulatorRAM::residency(uint64_t page_id, double now) const {
    auto it = load_time_.find(page_id);
    if (it == load_time_.end()) return 0.0;
    return (now > it->second) ? (now - it->second) : 0.0;
}

// ── 指定位置的注入（FaultModel 用）──────────────────────────────────────────
//
// 與 inject_random_flip 相同的兩條路徑：
//   use_ecc_  → 改 DataLayer（parity 未更新 → ecc_read 可偵測）
//   !use_ecc_ → 改 page_data_（真實來源），並同步 DataLayer staging

bool RamulatorRAM::flip_bit(uint64_t page_id, uint32_t byte, uint8_t bit) {
    if (byte >= PAGE_SIZE || bit >= 8) return false;
    const uint64_t addr = page_addr(page_id);

    if (use_ecc_) {
        if (!dl_has_page(addr)) return false;
        dl_flip_bit(addr, (int)byte, (int)bit);
        return true;
    }
    auto it = page_data_.find(page_id);
    if (it == page_data_.end() || it->second.size() <= byte) return false;
    it->second[byte] ^= static_cast<uint8_t>(1u << bit);
    if (dl_has_page(addr)) dl_flip_bit(addr, (int)byte, (int)bit);
    return true;
}

bool RamulatorRAM::force_bit(uint64_t page_id, uint32_t byte, uint8_t bit, bool value) {
    if (byte >= PAGE_SIZE || bit >= 8) return false;
    const uint64_t addr = page_addr(page_id);
    const uint8_t  mask = static_cast<uint8_t>(1u << bit);

    if (use_ecc_) {
        if (!dl_has_page(addr)) return false;
        auto data = dl_load(addr);                       // copy
        if (data.size() <= byte) return false;
        const bool cur = (data[byte] & mask) != 0;
        if (cur == value) return false;                  // 已是卡住的值
        dl_flip_bit(addr, (int)byte, (int)bit);          // 差一個 bit → 翻過去即可
        return true;
    }
    auto it = page_data_.find(page_id);
    if (it == page_data_.end() || it->second.size() <= byte) return false;
    const bool cur = (it->second[byte] & mask) != 0;
    if (cur == value) return false;
    if (value) it->second[byte] |=  mask;
    else       it->second[byte] &= static_cast<uint8_t>(~mask);
    if (dl_has_page(addr)) dl_flip_bit(addr, (int)byte, (int)bit);
    return true;
}

// 主動逐出（積極 TTL 用）。與 check_ttl 的逐出路徑相同，
// 差別只在不檢查到期條件 —— 由呼叫端負責判斷。
bool RamulatorRAM::evict(uint64_t page_id) {
    const bool resident = use_ecc_ ? dl_has_page(page_addr(page_id))
                                   : (page_data_.count(page_id) > 0);
    if (!resident) return false;
    evict_page(page_id);
    stats_.n_ttl_evictions++;
    return true;
}

// ── IRAM 介面實作 ─────────────────────────────────────────────────────────────

RAMReadResult RamulatorRAM::read(uint64_t page_id, double current_time) {
    uint64_t addr = page_addr(page_id);
    if (current_time > current_time_) current_time_ = current_time;
    stats_.n_reads++;

    // SSD reload：頁被 LRU 逐出後不在 RAM → 從 SSD 重載
    // use_ecc_=true：以 dl_has_page 判斷；use_ecc_=false：以 page_data_ 判斷
    bool was_hit = use_ecc_ ? dl_has_page(addr)
                            : (page_data_.count(page_id) > 0);

    // TTL 懶惰檢查（先於 hit 判定）：過期即逐出，本次讀取降級為 miss，
    // 從 SSD 取回乾淨副本 —— 這就是 TTL 的「重載即修復」語意。
    // 僅在有 SSD 後端時啟用：無後端時逐出等於直接遺失資料。
    if (ssd_ && was_hit && check_ttl(page_id, current_time)) was_hit = false;

    if (ssd_ && !was_hit) {
        std::vector<uint8_t> from_ssd;
        ssd_->read(static_cast<uint32_t>(page_id), from_ssd);
        if (use_ecc_) {
            ecc_write(addr, from_ssd);       // 重建 ECC parity
        } else {
            page_data_[page_id] = from_ssd;  // 真實來源
            dl_store(addr, from_ssd);         // 讓 DataLayer plugin 不 throw
        }
        do_write(addr);
        note_load(page_id, current_time);    // 曝露窗歸零
        stats_.n_misses++;
        if (capacity_pages_ > 0) {
            lru_touch(page_id);
            maybe_evict();
        }
    } else if (was_hit) {
        stats_.n_hits++;
        last_access_[page_id] = current_time;
        if (ssd_ && capacity_pages_ > 0) lru_touch(page_id);
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

        // ⚠️ 這兩行原本漏了。`result` 只把單次讀取的 CE/UE 交給呼叫端，
        //    但 `get_stats()` 回傳的是 `stats_`，而 `stats_` 從來沒被更新
        //    —— 導致 r2 後端的 `ecc_corrected` / `ecc_ue` 在 CSV 裡**恆為 0**。
        //    ram 後端的 ECC_RAM::read() 一直都有做這件事，兩者因此對不起來：
        //    F16 實測 ram 的 ecc_corrected 為 270/1249/4845/22006/64850，
        //    r2 全部是 0，看起來像「ECC 在 r2 上沒作用」，其實只是沒記帳。
        stats_.n_hw_corrected += static_cast<uint64_t>(result.n_corrected);
        stats_.n_ecc_ue       += static_cast<uint64_t>(result.n_ecc_ue);
        // 註：`n_hw_silent`（誤修正）在 r2 恆為 0 —— ECCPlugin 的
        //     hamming_decode 只回傳 CE(1) / UE(2) 兩類，沒有 MISCORRECTED
        //     這個分類，這是 r2 與 ram 之間真實存在的模型差異，不是 bug。
    } else {
        result.data        = page_data_[page_id];  // 唯一真實來源，不用 dl_load
        result.n_corrected = 0;
        result.n_ecc_ue    = 0;
    }
    return result;
}

void RamulatorRAM::write(uint64_t page_id, const std::vector<uint8_t>& data,
                          double current_time) {
    uint64_t addr = page_addr(page_id);
    if (current_time > current_time_) current_time_ = current_time;
    stats_.n_writes++;

    if (ssd_) ssd_->write(static_cast<uint32_t>(page_id), data);  // write-through
    if (use_ecc_) {
        ecc_write(addr, data);         // DataLayer + ECC chip parity
    } else {
        page_data_[page_id] = data;    // 真實來源
        dl_store(addr, data);          // DataLayer staging（防 plugin throw）
    }
    do_write(addr);                    // DDR4 Write timing
    // 寫入即刷新：資料與 SSD 一致，累積的 flip 已被覆蓋 → 曝露窗歸零
    note_load(page_id, current_time);
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
    stats_.n_allocations++;
    return page_id;
}

void RamulatorRAM::free_page(uint64_t page_id) {
    uint64_t addr = page_addr(page_id);
    if (ssd_) ssd_->trim(static_cast<uint32_t>(page_id));
    if (ssd_ && capacity_pages_ > 0) lru_remove(page_id);
    if (!use_ecc_) page_data_.erase(page_id);
    dl_erase(addr);
    load_time_.erase(page_id);
    last_access_.erase(page_id);
    ttl_policy_.erase(page_id);   // 頁面釋放 → 政策一併移除，避免 page_id 重用時誤套
}

bool RamulatorRAM::is_in_ram(uint64_t page_id) const {
    if (use_ecc_) return dl_has_page(page_addr(page_id));
    return page_data_.count(page_id) > 0;
}

void RamulatorRAM::inject_random_flip(uint64_t page_id) {
    int byte_pos = static_cast<int>(rng_() % PAGE_SIZE);
    int bit_pos  = static_cast<int>(rng_() % 8);

    if (use_ecc_) {
        // ECC 路徑：翻 DataLayer（parity 未更新 → ecc_read 可偵測並修正）
        uint64_t addr = page_addr(page_id);
        if (!dl_has_page(addr)) return;
        dl_flip_bit(addr, byte_pos, bit_pos);
    } else {
        // no-ECC 路徑：翻 page_data_（真實來源），DataLayer 同步保持一致
        auto it = page_data_.find(page_id);
        if (it == page_data_.end()) return;
        it->second[byte_pos] ^= static_cast<uint8_t>(1u << bit_pos);
        uint64_t addr = page_addr(page_id);
        if (dl_has_page(addr)) dl_flip_bit(addr, byte_pos, bit_pos);
    }
}

// ── 空間故障注入（E3：row hammer / column fault）────────────────────────────
//
// 頁面 ↔ 物理位置映射（與 ram 後端一致）：
//   page_id = bank × rows_per_bank + row
//   rows_per_bank = capacity_pages_ / DRAM_NUM_BANKS
//   （capacity_pages_=0 時以已分配頁數 next_page_id_ 推算）
//
// ECC 路徑：修改 DataLayer map（parity 不更新 → 模擬 DRAM 位元翻轉，ecc_read 可偵測）
// no-ECC 路徑：修改 page_data_（真實來源），並同步 DataLayer staging

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
        const uint64_t page_id = (uint64_t)bank * rpb + r;
        const uint64_t addr    = page_addr(page_id);
        if (use_ecc_) {
            if (!dl_has_page(addr)) continue;
            auto data = dl_load(addr);                 // copy
            for (auto& byte : data)
                for (int b = 0; b < 8; ++b)
                    if (flip_dist(rng_))
                        byte ^= static_cast<uint8_t>(1u << b);
            dl_store(addr, data);                      // parity 未更新 = DRAM flip 語意
        } else {
            auto it = page_data_.find(page_id);
            if (it == page_data_.end()) continue;
            for (auto& byte : it->second)
                for (int b = 0; b < 8; ++b)
                    if (flip_dist(rng_))
                        byte ^= static_cast<uint8_t>(1u << b);
            if (dl_has_page(addr)) dl_store(addr, it->second);  // 同步 DataLayer staging
        }
    }
}

void RamulatorRAM::inject_column_error(uint32_t bank, uint32_t start_row,
                                        uint32_t col_byte, uint8_t bit,
                                        uint32_t num_rows) {
    if (bank >= DRAM_NUM_BANKS || col_byte >= PAGE_SIZE || bit >= 8) return;
    const uint64_t rpb = r2_rows_per_bank(capacity_pages_, next_page_id_);
    const uint64_t end_row = std::min((uint64_t)start_row + num_rows, rpb);

    for (uint64_t r = start_row; r < end_row; ++r) {
        const uint64_t page_id = (uint64_t)bank * rpb + r;
        const uint64_t addr    = page_addr(page_id);
        if (use_ecc_) {
            if (!dl_has_page(addr)) continue;
            dl_flip_bit(addr, (int)col_byte, (int)bit);  // parity 未更新 = DRAM flip 語意
        } else {
            auto it = page_data_.find(page_id);
            if (it == page_data_.end()) continue;
            it->second[col_byte] ^= static_cast<uint8_t>(1u << bit);
            if (dl_has_page(addr)) dl_store(addr, it->second);
        }
    }
}

}  // namespace Ramulator
