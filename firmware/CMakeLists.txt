cmake_minimum_required(VERSION 3.20.0)

# Add the local DW3000 Zephyr driver module before find_package(Zephyr)
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../zephyr-dw3000-decadriver
)

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
