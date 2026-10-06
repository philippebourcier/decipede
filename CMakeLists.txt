cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD w5500_evb_pico2)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
# Dependencies: git submodules in lib/ (pinned), plus the Arm toolchain and a
# picotool build that tools/setup.sh puts in .deps/ (see build.sh).
if (NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk)
endif()
if (NOT DEFINED PICO_TOOLCHAIN_PATH AND NOT DEFINED ENV{PICO_TOOLCHAIN_PATH}
    AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/.deps/arm-none-eabi/bin)
    set(PICO_TOOLCHAIN_PATH ${CMAKE_CURRENT_LIST_DIR}/.deps/arm-none-eabi)
endif()
# picotool with seal/hash support, built from lib/picotool by tools/setup.sh
# (the SDK's prebuilt download lacks it).
if (NOT DEFINED picotool_DIR AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/.deps/picotool/picotool)
    set(picotool_DIR ${CMAKE_CURRENT_LIST_DIR}/.deps/picotool/picotool)
endif()
set(IOLIBRARY_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/ioLibrary_Driver CACHE PATH "WIZnet ioLibrary_Driver checkout")

# mbedTLS (TLS client) configuration, read by pico_sdk_init().
set(PICO_MBEDTLS_CONFIG_FILE rtk_mbedtls_config.h)

include(pico_sdk_import.cmake)

project(rtkbase C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# ---------------------------------------------------------------------------
# Firmware version: VERSION holds MAJOR.MINOR.PATCH. The bootrom picks the
# A/B partition with the higher (major, minor) image version, so minor
# encodes MINOR*256+PATCH and every release must bump VERSION.
# ---------------------------------------------------------------------------
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/VERSION)
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/VERSION FW_VERSION LIMIT_COUNT 1)
string(STRIP "${FW_VERSION}" FW_VERSION)
if (NOT FW_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "VERSION must be MAJOR.MINOR.PATCH, got '${FW_VERSION}'")
endif()
set(FW_VERSION_MAJOR ${CMAKE_MATCH_1})
set(FW_VERSION_MINOR ${CMAKE_MATCH_2})
set(FW_VERSION_PATCH ${CMAKE_MATCH_3})
math(EXPR FW_IMAGE_MINOR "${FW_VERSION_MINOR} * 256 + ${FW_VERSION_PATCH}")

# ---------------------------------------------------------------------------
# Provisioning defaults (baked in, used until a config is stored in flash).
# provision/device.env holds this deployment's CONFIG_URL (see Provisioning in README).
# ---------------------------------------------------------------------------
set(PROVISION_FILE ${CMAKE_CURRENT_LIST_DIR}/provision/device.env CACHE FILEPATH "Provisioning env file")
# Network / branding provisioning, two files with the same KEY=value format:
#   provision/defaults.env  tracked: the project's defaults (a fork edits this)
#   provision/device.env    git-ignored: this deployment's values (override)
# See provision/device.env.example for the keys.
# The config.json URL is the only URL built in: everything else (telemetry,
# OTA, UM980 firmware, NTRIP caster) comes from config.json.
set(PROV_KEYS CONFIG_URL BRAND_NAME BRAND_TAGLINE BRAND_LOGO)
set(PROV_REMOTE_KEYS TELEMETRY_URL OTA_URL GNSS_FW_URL FIRMWARE_URL NTRIP_SERVER NTRIP_PORT)
foreach(_k IN LISTS PROV_KEYS)
    set(PROV_${_k} "")
endforeach()
set(PROV_BRAND_NAME "RTK Base")
set(PROV_BRAND_TAGLINE "MCU Base Status")
set(PROV_BRAND_LOGO "web/logo.svg")
set(PROV_DEPENDS "")
function(prov_load file)
    file(STRINGS ${file} _lines)
    foreach(_line IN LISTS _lines)
        if (_line MATCHES "^[ \t]*#" OR _line MATCHES "^[ \t]*$")
            continue()
        endif()
        if (NOT _line MATCHES "^[ \t]*([A-Za-z_]+)[ \t]*=[ \t]*(.*)$")
            message(FATAL_ERROR "${file}: cannot parse line '${_line}'")
        endif()
        string(TOUPPER "${CMAKE_MATCH_1}" _key)
        string(STRIP "${CMAKE_MATCH_2}" _val)
        if (_key IN_LIST PROV_REMOTE_KEYS)
            string(TOLOWER "${_key}" _remote)
            if (_key MATCHES "^(OTA|GNSS_FW)_URL$")
                set(_remote "firmware_url")
            endif()
            message(FATAL_ERROR "${file}: ${_key} is no longer built in: set \"${_remote}\" in config.json")
        endif()
        if (NOT _key IN_LIST PROV_KEYS)
            message(FATAL_ERROR "${file}: unknown key ${_key} (known: ${PROV_KEYS})")
        endif()
        # Values end up in C strings and HTML: keep them plain.
        if (_val MATCHES "[\"\\<>]")
            message(FATAL_ERROR "${file}: ${_key} must not contain quotes, backslashes or < >")
        endif()
        if (_key MATCHES "^BRAND_" AND _val MATCHES "%")
            message(FATAL_ERROR "${file}: ${_key} must not contain %")
        endif()
        set(PROV_${_key} "${_val}" PARENT_SCOPE)
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${file})
    set(PROV_DEPENDS ${PROV_DEPENDS} ${file} PARENT_SCOPE)  # brand changes rebuild the dashboard
    message(STATUS "Provisioning from ${file}")
endfunction()
foreach(_f ${CMAKE_CURRENT_LIST_DIR}/provision/defaults.env ${PROVISION_FILE})
    if (EXISTS ${_f})
        prov_load(${_f})
    endif()
endforeach()
if (NOT PROV_CONFIG_URL)
    message(WARNING "No CONFIG_URL in provision/*.env: the firmware will have no config server until one is set")
endif()
if (NOT IS_ABSOLUTE "${PROV_BRAND_LOGO}")
    set(PROV_BRAND_LOGO "${CMAKE_CURRENT_LIST_DIR}/${PROV_BRAND_LOGO}")
endif()
if (NOT EXISTS "${PROV_BRAND_LOGO}")
    message(FATAL_ERROR "BRAND_LOGO: ${PROV_BRAND_LOGO} not found")
endif()
message(STATUS "Brand: ${PROV_BRAND_NAME} / ${PROV_BRAND_TAGLINE}, logo ${PROV_BRAND_LOGO}")
# TLS trust store: Let's Encrypt roots (X1/X2 and their Gen Y successors
# YR/YE) + optional provision/ca.pem (private CA). The build fails if any of
# them expires within CA_MIN_YEARS; it is re-checked on every VERSION bump.
set(CA_MIN_YEARS 5 CACHE STRING "Minimum remaining validity of TLS trust anchors, in years")
set(CA_FILES ${CMAKE_CURRENT_LIST_DIR}/third_party/certs/isrg_root_x1.pem
             ${CMAKE_CURRENT_LIST_DIR}/third_party/certs/isrg_root_x2.pem
             ${CMAKE_CURRENT_LIST_DIR}/third_party/certs/isrg_root_yr.pem
             ${CMAKE_CURRENT_LIST_DIR}/third_party/certs/isrg_root_ye.pem
             ${CMAKE_CURRENT_LIST_DIR}/provision/ca.pem)
# The list of CA files that exist is written to a stamp file (rewritten only
# when it changes), so adding or removing provision/ca.pem regenerates the
# trust store, not just editing it.
set(CA_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tools/gen_ca_bundle.py ${CMAKE_CURRENT_LIST_DIR}/VERSION)
set(CA_PRESENT "")
foreach(f IN LISTS CA_FILES)
    if (EXISTS ${f})
        list(APPEND CA_DEPENDS ${f})
        string(APPEND CA_PRESENT "${f}\n")
    endif()
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/ca_files.txt CONTENT "${CA_PRESENT}")
list(APPEND CA_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/ca_files.txt)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/ca_bundle.c
    COMMAND python3 -I ${CMAKE_CURRENT_LIST_DIR}/tools/gen_ca_bundle.py --min-years ${CA_MIN_YEARS} ${CMAKE_CURRENT_BINARY_DIR}/generated/ca_bundle.c ${CA_FILES}
    DEPENDS ${CA_DEPENDS}
    COMMENT "Generating TLS trust store"
)
# Status dashboard: web/index.html (+ logo) -> gzipped C array.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/web_index.c
    COMMAND python3 -I ${CMAKE_CURRENT_LIST_DIR}/tools/build_web.py --version ${FW_VERSION}
            --brand-name ${PROV_BRAND_NAME} --brand-tagline ${PROV_BRAND_TAGLINE}
            ${CMAKE_CURRENT_LIST_DIR}/web/index.html ${PROV_BRAND_LOGO}
            ${CMAKE_CURRENT_BINARY_DIR}/generated/web_index.c
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tools/build_web.py ${CMAKE_CURRENT_LIST_DIR}/web/index.html
            ${PROV_BRAND_LOGO} ${CMAKE_CURRENT_LIST_DIR}/VERSION ${PROV_DEPENDS}
    VERBATIM
    COMMENT "Generating status dashboard"
)
# Re-configure when files are added to or removed from provision/.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/provision)
configure_file(src/build_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/build_config.h @ONLY)

# ---------------------------------------------------------------------------
# WIZnet ioLibrary (W5500 sockets, DHCP, DNS)
# ---------------------------------------------------------------------------
add_library(iolibrary STATIC
    ${IOLIBRARY_PATH}/Ethernet/socket.c
    ${IOLIBRARY_PATH}/Ethernet/wizchip_conf.c
    ${IOLIBRARY_PATH}/Ethernet/W5500/w5500.c
    ${IOLIBRARY_PATH}/Internet/DHCP/dhcp.c
)
target_include_directories(iolibrary PUBLIC
    ${IOLIBRARY_PATH}/Ethernet
    ${IOLIBRARY_PATH}/Ethernet/W5500
    ${IOLIBRARY_PATH}/Internet/DHCP
)
target_compile_definitions(iolibrary PUBLIC _WIZCHIP_=5500)
target_compile_options(iolibrary PRIVATE -w)
target_link_libraries(iolibrary PRIVATE pico_stdlib)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(rtkbase
    src/main.c
    src/app_status.c
    src/config.c
    src/cpu_load.c
    src/dns_proto.c
    src/flash_store.c
    src/fw_manifest.c
    src/gnss_io.c
    src/gnss_logic.c
    src/gnss_watchdog.c
    src/http_client.c
    src/http_server.c
    src/idle.c
    src/json_util.c
    src/led.c
    src/lsm6dsv.c
    src/mcu_temp.c
    src/net.c
    src/ntp_proto.c
    src/ntp_server.c
    src/ntrip.c
    src/ota.c
    src/rtcm.c
    src/sht4x.c
    src/stack_mon.c
    src/tls.c
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ca_bundle.c
    ${CMAKE_CURRENT_BINARY_DIR}/generated/web_index.c
    src/um980.c
    src/um980_fw.c
    src/utc.c
    src/util.c
    src/wdt.c
)
target_include_directories(rtkbase PRIVATE
    src
    third_party/jsmn
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_definitions(rtkbase PRIVATE
    PICO_CORE1_STACK_SIZE=0x1000
    FW_VERSION_STRING=\"${FW_VERSION}\"
    FW_VERSION_MAJOR=${FW_VERSION_MAJOR}
    FW_VERSION_MINOR=${FW_VERSION_MINOR}
    FW_VERSION_PATCH=${FW_VERSION_PATCH}
    PICO_CRT0_IMAGE_TYPE_TBYB=1
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
)
target_compile_options(rtkbase PRIVATE -Wall -Wextra -Wno-unused-parameter)
# CPU load measurement: count the time spent in sleep_ms/sleep_us (src/cpu_load.c).
target_link_options(rtkbase PRIVATE -Wl,--wrap=sleep_ms -Wl,--wrap=sleep_us)
target_link_libraries(rtkbase
    pico_stdlib
    pico_multicore
    pico_flash
    pico_sha256
    pico_unique_id
    pico_rand
    pico_bootrom
    boot_uf2_headers
    hardware_adc
    hardware_dma
    hardware_i2c
    hardware_pwm
    hardware_spi
    hardware_uart
    hardware_watchdog
    iolibrary
    pico_mbedtls
)
pico_enable_stdio_usb(rtkbase 1)
pico_enable_stdio_uart(rtkbase 0)

# Image version (A/B selection), embedded SHA-256 hash (checked by the
# bootrom), and an explicit UF2 family so OTA images land in App A/B.
pico_set_binary_version(rtkbase MAJOR ${FW_VERSION_MAJOR} MINOR ${FW_IMAGE_MINOR})
pico_hash_binary(rtkbase)
pico_set_uf2_family(rtkbase rp2350-arm-s)
pico_add_extra_outputs(rtkbase)

# ---------------------------------------------------------------------------
# Partition table UF2 (flashed once, before the first application image)
# ---------------------------------------------------------------------------
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/partitions.uf2
    COMMAND picotool partition create ${CMAKE_CURRENT_LIST_DIR}/partitions.json ${CMAKE_CURRENT_BINARY_DIR}/partitions.uf2
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/partitions.json
    COMMENT "Creating partitions.uf2"
)
add_custom_target(partitions ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/partitions.uf2)
add_dependencies(partitions rtkbase)
