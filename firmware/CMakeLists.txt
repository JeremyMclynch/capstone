cmake_minimum_required(VERSION 3.20.0)

# DW3000 driver module discovery:
#   - West workspace: auto-discovered from west.yml (WEST_TOPDIR is set)
#   - Fallback: build.sh passes -DZEPHYR_EXTRA_MODULES, or detect sibling dir
if(NOT DEFINED ZEPHYR_EXTRA_MODULES AND NOT DEFINED WEST_TOPDIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../zephyr-dw3000-decadriver/zephyr/module.yml")
        list(APPEND ZEPHYR_EXTRA_MODULES
            ${CMAKE_CURRENT_SOURCE_DIR}/../zephyr-dw3000-decadriver
        )
    endif()
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(uwb_mesh_tracker VERSION 1.0.0)

target_sources(app PRIVATE
    src/main.c
    src/uwb_manager.c
    src/thread_coap.c
    src/device_config.c
    src/uci.c
    src/uci_uart.c
    src/uci_coap.c
)

target_include_directories(app PRIVATE src)

# Workaround: pm_sysflash.h in the MCUboot utility library only checks
# CONFIG_SINGLE_APPLICATION_SLOT (an MCUboot-only Kconfig), not
# CONFIG_MCUBOOT_BOOTLOADER_MODE_SINGLE_APP (the app-level equivalent).
# Propagate the define so the correct single-slot code path is used.
if(CONFIG_MCUBOOT_BOOTLOADER_MODE_SINGLE_APP)
    zephyr_compile_definitions(CONFIG_SINGLE_APPLICATION_SLOT=1)
endif()
