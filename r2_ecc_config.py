# r2_ecc_config.py — Step 2b ECC 測試用 Python config script
#
# 載入 DataLayer 和 ECC plugin。
# 注意：DataLayer 必須在 ECC 之前（ECC 內部會呼叫 dl_store/dl_load）。
# BitFlip 不載入，測試直接使用 dl_flip_bit() 模擬 DRAM bit flip。

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
        {"impl": "ECC",       "debug": True},
    ],
)

memory_system = GenericDRAM(
    clock_ratio=3,
    channel_mapper=PassThroughChannelMapper(),
    controllers=[controller],
)

if __name__ == "__main__":
    ramulator.Simulation(null_frontend, memory_system)
