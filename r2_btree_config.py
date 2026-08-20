# r2_btree_config.py — B-Tree 整合測試用 Python config
#
# 載入全部三個 plugin：
#   DataLayer  — 儲存 B-Tree page 資料（模擬 DRAM 資料晶片）
#   BitFlip    — 週期性注入 DRAM bit flip（flip_interval=2000 ticks）
#   ECC        — 透明偵測/修正 1-bit error（模擬 ECC 晶片）
#
# flip_interval=2000 ticks：典型 DDR4 Read 需 ~100 ticks，
# 因此大約每 20 次讀寫就有一次自動 bit flip 被注入。

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
        {"impl": "BitFlip",   "flip_interval": 2000, "seed": 123, "debug": False},
        {"impl": "ECC",       "debug": False},
    ],
)

memory_system = GenericDRAM(
    clock_ratio=3,
    channel_mapper=PassThroughChannelMapper(),
    controllers=[controller],
)

if __name__ == "__main__":
    ramulator.Simulation(null_frontend, memory_system)
