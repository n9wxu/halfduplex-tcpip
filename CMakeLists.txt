cmake_minimum_required(VERSION 3.20)

# One build per port (the HAL under the slave):
#
#   test    host tests on the test HAL, the template port, and the Linux
#           master (default; what the CI runs first)
#             cmake -B build && cmake --build build && ctest --test-dir build
#   samd21  the slave on the SAMD21 port, for a Cortex-M0+, as a library
#             cmake -B build-samd21 -DHDX_PORT=samd21 \
#                   -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   rp2040  the slave on the RP2040 port, under the pico-sdk, as a library
#             cmake -B build-rp2040 -DHDX_PORT=rp2040 -DPICO_SDK_PATH=...
#
# A firmware uses its port's library (hdx_samd21, hdx_rp2040) and the IP stack
# (hdx_stack_<port>), or compiles the same sources into its own build.

set(HDX_PORT test CACHE STRING "Port to build: test, samd21 or rp2040")
option(HDX_WERROR "Treat warnings in this project's own sources as errors (CI)" OFF)
set_property(CACHE HDX_PORT PROPERTY STRINGS test samd21 rp2040)

if(HDX_PORT STREQUAL "rp2040")
    include(cmake/pico_sdk_import.cmake)
    project(halfduplex_tcpip C CXX ASM)
else()
    project(halfduplex_tcpip C)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

include(cmake/deps.cmake)

set(HDX_SLAVE_SOURCES
    ${PROJECT_SOURCE_DIR}/slave/link485.c
    ${PROJECT_SOURCE_DIR}/slave/linkcheck.c
    ${PROJECT_SOURCE_DIR}/slave/tcplink.c)
set(HDX_SLAVE_INCLUDES ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/slave)

# The slave and one port in one archive, so the port's HAL resolves the
# slave's calls whatever the linker's archive order.
function(hdx_port_library name stack)
    add_library(${name} STATIC ${HDX_SLAVE_SOURCES} ${ARGN})
    target_include_directories(${name} PUBLIC ${HDX_SLAVE_INCLUDES})
    target_link_libraries(${name} PUBLIC ${stack})
    hdx_warnings(${HDX_SLAVE_SOURCES} ${ARGN})
endfunction()

# Warnings for this project's own sources (not the pico-sdk's, which its
# libraries add to ours).
function(hdx_warnings)
    set(options -Wall -Wextra)
    if(HDX_WERROR)
        list(APPEND options -Werror)
    endif()
    set_property(SOURCE ${ARGN} PROPERTY COMPILE_OPTIONS ${options})
endfunction()

# Each library's members and their sizes, after it is built.
function(hdx_size_report name)
    find_program(HDX_SIZE NAMES ${CMAKE_SIZE} arm-none-eabi-size REQUIRED)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${HDX_SIZE} -t $<TARGET_FILE:${name}>
        VERBATIM)
endfunction()

if(HDX_PORT STREQUAL "test")
    enable_testing()
    hdx_stack(hdx_stack_host ON)

    hdx_port_library(hdx_slave_test hdx_stack_host ${PROJECT_SOURCE_DIR}/port/test/hdx_hal_test.c)
    target_include_directories(hdx_slave_test PUBLIC ${PROJECT_SOURCE_DIR}/port/test)

    # The slave alone, for linking against a port's objects (the template).
    hdx_port_library(hdx_slave hdx_stack_host)

    add_subdirectory(test)

    # The README's minimal slave, built (not run: it loops forever) on the
    # test port so the example keeps up with the API.
    add_executable(minimal_slave examples/minimal_slave.c)
    target_link_libraries(minimal_slave PRIVATE hdx_slave_test)
    hdx_warnings(examples/minimal_slave.c)

    # The Linux master.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(tmptun master/linux/tmptun.c)
        target_include_directories(tmptun PRIVATE include)
        hdx_warnings(master/linux/tmptun.c)
    endif()

elseif(HDX_PORT STREQUAL "samd21")
    add_compile_options(-mcpu=cortex-m0plus -mthumb -ffunction-sections -fdata-sections)
    hdx_stack(hdx_stack_samd21 ON)
    hdx_port_library(hdx_samd21 hdx_stack_samd21 ${PROJECT_SOURCE_DIR}/port/samd21/hdx_hal_samd21.c)
    target_include_directories(hdx_samd21 SYSTEM PUBLIC
        ${cmsis_atmel_SOURCE_DIR}/CMSIS-Atmel/CMSIS/Device/ATMEL
        ${cmsis_atmel_SOURCE_DIR}/CMSIS-Atmel/CMSIS/Device/ATMEL/samd21/include
        ${cmsis_core_SOURCE_DIR}/CMSIS/Core/Include)
    target_compile_definitions(hdx_samd21 PUBLIC __SAMD21G18A__)
    hdx_size_report(hdx_samd21)
    hdx_size_report(hdx_stack_samd21)

elseif(HDX_PORT STREQUAL "rp2040")
    pico_sdk_init()
    hdx_stack(hdx_stack_rp2040 ON)
    hdx_port_library(hdx_rp2040 hdx_stack_rp2040
        ${PROJECT_SOURCE_DIR}/port/rp2040/hdx_hal_rp2040.c
        ${PROJECT_SOURCE_DIR}/port/rp2040/dmaring.c)
    target_include_directories(hdx_rp2040 PUBLIC ${PROJECT_SOURCE_DIR}/port/rp2040)
    target_link_libraries(hdx_rp2040 PUBLIC pico_stdlib hardware_uart hardware_dma
                          hardware_watchdog)
    hdx_size_report(hdx_rp2040)

else()
    message(FATAL_ERROR "HDX_PORT is test, samd21 or rp2040, not '${HDX_PORT}'")
endif()
