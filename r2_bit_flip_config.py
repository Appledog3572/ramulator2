# r2_bit_flip_config.py — Step 2a BitFlip 測試用 Python config script
#
# 同時載入 DataLayer 和 BitFlip 兩個 plugin。
# BitFlip 的自動注入由 flip_interval 控制（0 = 停用；測試用手動注入）。

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
        {"impl": "DataLayer",  "page_size": 4096, "debug": False},
        {"impl": "BitFlip",    "flip_interval": 0, "seed": 42, "debug": True},
    ],
)

memory_system = GenericDRAM(
    clock_ratio=3,
    channel_mapper=PassThroughChannelMapper(),
    controllers=[controller],
)

if __name__ == "__main__":
    ramulator.Simulation(null_frontend, memory_system)
