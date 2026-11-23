# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
#
# eds_select_nvm_backend(<app-target> <diag-root>)
#
# Links exactly one nvm_store_* implementation into the Zephyr app, plus
# platform/zephyr/eds_nvm_diag_nvs.c, the one call that gives an example its
# partition (EDS#643). The same choice as examples/basic_ecu/CMakeLists.txt:
#
#   native_sim                              nvm_store_mock.c (RAM)
#   nucleo_h743zi, nucleo_h753zi            nvm_store_append.c (128 KB sectors
#                                           overflow NVS's uint16_t, EDS#504)
#   frdm_mcxn947                            nvm_store_append.c (128-byte write
#                                           block on Zephyr 3.7.0, NVS takes 32,
#                                           EDS#640)
#   everything else (mr_canhubk3)           nvm_store.c (NVS, write block 8 B)
#
# Call after the app's other sources are added. Needs CONFIG_FLASH, CONFIG_FLASH_MAP
# and (NVS) CONFIG_NVS from the board's .conf, as for basic_ecu.
function(eds_select_nvm_backend app_target diag_root)
    target_sources(${app_target} PRIVATE ${diag_root}/platform/zephyr/eds_nvm_diag_nvs.c)
    if(CONFIG_BOARD_NATIVE_SIM)
        target_sources(${app_target} PRIVATE ${diag_root}/platform/zephyr/nvm_store_mock.c)
    elseif(CONFIG_BOARD_NUCLEO_H743ZI OR CONFIG_BOARD_NUCLEO_H753ZI OR CONFIG_BOARD_FRDM_MCXN947)
        target_sources(${app_target} PRIVATE ${diag_root}/platform/zephyr/nvm_store_append.c)
        target_compile_definitions(${app_target} PRIVATE DIAG_NVM_BACKEND_APPEND=1)
    else()
        target_sources(${app_target} PRIVATE ${diag_root}/platform/zephyr/nvm_store.c)
    endif()
endfunction()
