// r2_btree_ram.cpp — RamulatorRAM 實作

#include "r2_btree_ram.hpp"
#include <stdexcept>

namespace Ramulator {

// ── 建構 ─────────────────────────────────────────────────────────────────────

RamulatorRAM::RamulatorRAM(IMemorySystem* mem)
    : mem_(mem),
      tCK_ns_(mem->get_tCK()),
      tx_bytes_(mem->get_tx_bytes())
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
    Request req(addr, Request::Type::Read, 0,
                [&](Request& r) { done = true; depart = r.depart; });
    req.size_bytes = tx_bytes_;
    req.addr_vec   = {0};
    while (!mem_->send(req)) mem_->tick();
    while (!done)            mem_->tick();
    return depart;
}

// ── IRAM 介面實作 ─────────────────────────────────────────────────────────────

RAMReadResult RamulatorRAM::read(uint64_t page_id, double /*current_time*/) {
    uint64_t addr = page_addr(page_id);

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
    result.hit         = true;
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
    ecc_write(addr, data);   // DataLayer 儲存 + ECC chip 計算 parity
    do_write(addr);          // DDR4 Write timing
}

uint64_t RamulatorRAM::allocate(double current_time) {
    uint64_t page_id = next_page_id_++;
    // 初始化零頁並寫進 DataLayer + ECC chip
    std::vector<uint8_t> zeros(PAGE_SIZE, 0);
    write(page_id, zeros, current_time);
    return page_id;
}

void RamulatorRAM::free_page(uint64_t page_id) {
    // 從 DataLayer 刪除（ECC chip 的 parity 殘留但不影響正確性）
    dl_erase(page_addr(page_id));
}

}  // namespace Ramulator
