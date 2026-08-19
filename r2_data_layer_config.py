# r2_data_layer_config.py — Step 1 DataLayer 測試用 Python config script
#
# 用法（從 ramulator2/ 目錄）：
#   PYTHONPATH=python python3 -m ramulator export r2_data_layer_config.py \
#       -o r2_data_layer_config.yaml
#
# 此 script 也可直接被 `python -m ramulator run` 執行（不過我們用 C++ test）。

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

# DataLayer plugin — 自定義，用 raw dict 即可
data_layer_plugin = {
    "impl": "DataLayer",
    "page_size": 4096,
    "debug": True,
}

# NullFrontend — 定義在 r2_test_data_layer.cpp，非標準模組，用 raw dict
null_frontend = {"impl": "NullFrontend"}

# DDR4-2400R 8Gb x8 單通道
ddr4 = DDR4(org_preset="DDR4_8Gb_x8", timing_preset="DDR4_2400R", rank=1)

controller = GenericDDR(
    dram=ddr4,
    scheduler=FRFCFS(),
    refresh_manager=NoRefresh(),
    row_policy=Open(),
    addr_mapper=RoBaRaCoCh(),
    controller_plugins=[data_layer_plugin],
)

memory_system = GenericDRAM(
    clock_ratio=3,
    channel_mapper=PassThroughChannelMapper(),
    controllers=[controller],
)

if __name__ == "__main__":
    ramulator.Simulation(null_frontend, memory_system)
