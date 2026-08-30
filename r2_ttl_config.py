# r2_ttl_config.py — v2/ttl_theta_experiment 專用的 Ramulator2 組態
#
# 載入 DataLayer + ECC 兩個 plugin，DDR4 參數與 r2_data_layer_config.py /
# r2_ecc_config.py 完全相同（DDR4_8Gb_x8、DDR4_2400R、rank=1）。
#
# ── 為什麼要另開一份 ────────────────────────────────────────────────────────
# ① `r2_data_layer_config.py` 只有 DataLayer。ttl_theta_experiment 的
#    A4 / A5（ECC 臂）在 `RamulatorRAM` 的建構子會呼叫 `ecc_is_ready()`，
#    plugin 不在就直接 throw：
#      RamulatorRAM: ECC plugin not ready (use_ecc=true but ECC plugin missing)
#
# ② `r2_ecc_config.py` 的 plugin 組合正好一樣，但它的 `ECC` 是 `debug=True`
#    ——那是給 ECC 單元測試看 `[ECC] CE corrected at word=...` 用的。
#    掃描要跑上百次、每次數萬筆讀取，debug 輸出會把 log 灌爆。
#    因此這份把兩個 plugin 的 debug 都關掉，其餘一字未改。
#
# ⚠️ **DataLayer 必須排在 ECC 之前** —— ECCPlugin 的 write/read 內部會呼叫
#    dl_store / dl_load，順序反了就抓不到 DataLayer。
#
# ── 對非 ECC 實驗臂的影響：沒有 ─────────────────────────────────────────────
# ECCPlugin 只實作 init() 與 setup()，沒有覆寫 IControllerPlugin 的
# pre_schedule / on_issue / post_schedule（介面就只有這三個，且都是預設
# no-op），因此它完全不介入 DRAM 控制器的排程與時序，純粹是個由
# ecc_read / ecc_write 明確驅動的旁路資料結構。而 use_ecc=false 時
# 那兩個函式根本不會被呼叫。所以一份組態可以通吃所有臂。
#
# ── 產生 YAML ───────────────────────────────────────────────────────────────
#   PYTHONPATH=./python python3 -m ramulator export \
#       ./r2_ttl_config.py -o ./r2_ttl_config.yaml
# 或直接跑 `./r2_build_test.sh`（已含這一步）。

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "python"))

import ramulator
from ramulator.dram.ddr4 import DDR4
from ramulator.controller.generic_ddr import GenericDDR
from ramulator.memory_system.generic_dram import GenericDRAM
from ramulator.channel_mapper.pass_through_channel_mapper import PassThroughChannelMapper
from ramulator.scheduler.frfcfs import FRFCFS
from ramulator.refresh_manager.no_refresh import NoRefresh
from ramulator.row_policy.open import Open
from ramulator.addr_mapper.ro_ba_ra_co_ch import RoBaRaCoCh

null_frontend = {"impl": "NullFrontend"}

ddr4 = DDR4(org_preset="DDR4_8Gb_x8", timing_preset="DDR4_2400R", rank=1)

controller = GenericDDR(
    dram=ddr4,
    scheduler=FRFCFS(),
    refresh_manager=NoRefresh(),
    row_policy=Open(),
    addr_mapper=RoBaRaCoCh(),
    controller_plugins=[
        {"impl": "DataLayer", "page_size": 4096, "debug": False},
        {"impl": "ECC",       "debug": False},   # ← 與 r2_ecc_config.py 的唯一差別
    ],
)

memory_system = GenericDRAM(
    clock_ratio=3,
    channel_mapper=PassThroughChannelMapper(),
    controllers=[controller],
)

if __name__ == "__main__":
    ramulator.Simulation(null_frontend, memory_system)
