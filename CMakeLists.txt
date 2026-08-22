# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Audio pipeline is real-time and crackles under -O0. Default to Release;
# override explicitly with -DCMAKE_BUILD_TYPE=Debug only when debugging.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if (WIN32)
    set(USERHOME $ENV{USERPROFILE})
else ()
    set(USERHOME $ENV{HOME})
endif ()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif ()
# ====================================================================================

option(PICO_W_BUILD "Build for Rasberry Pi Pico W (RP2040, audio is missing)" OFF)
option(WAVESHARE_RP2350B_PLUS_W_BUILD "Build for Waveshare RP2350B-Plus-W (USB-C, 16MB flash, RM2 wireless)" OFF)

# --- Multi-slot (Phase 1) ---
# Number of concurrent DualSense controllers (1 = upstream single-controller
# behavior). Sizes BTstack connection caps, USB HID interfaces, and slot state.
set(MULTI_SLOT_COUNT 4 CACHE STRING "Concurrent controller slots (1-4)")
if (MULTI_SLOT_COUNT LESS 1 OR MULTI_SLOT_COUNT GREATER 4)
        message(FATAL_ERROR "MULTI_SLOT_COUNT must be 1-4 (got ${MULTI_SLOT_COUNT})")
endif ()

# --- Phase 2/3 hardware options: declared for pin-map stability, not yet
# implemented. All default OFF; enabling one before its phase lands only
# reserves the GPIO. See PLAN GPIO map (GP28 strip / GP3 power).
option(ENABLE_POWER_PIN "Pulse a GPIO (optocoupler to PWR_SW header) as wake fallback" OFF)
set(POWER_PIN_GPIO 3 CACHE STRING "GPIO for the power-button pulse")
# Controller-status LEDs: an 8-pixel WS2812B chain with alternating spacers
# (4 active slot indicators), 10% brightness cap. Default data pin GP28
# (physical pin 34): the PIO can drive any GPIO, and GP28 puts the whole LED
# harness on one corner of the board next to GND (pin 38) and VBUS 5V
# (pin 40).
option(ENABLE_LED_STRIP "WS2812B controller-status LEDs via PIO" OFF)
set(LED_STRIP_GPIO 28 CACHE STRING "GPIO for LED strip data")

if (PICO_W_BUILD AND WAVESHARE_RP2350B_PLUS_W_BUILD)
        message(FATAL_ERROR "PICO_W_BUILD and WAVESHARE_RP2350B_PLUS_W_BUILD are mutually exclusive")
endif ()

if (PICO_W_BUILD)
        set(PICO_BOARD pico_w CACHE STRING "Board type")
        set(PICO_RP2350 OFF CACHE BOOL "Not using RP2350")
elseif (WAVESHARE_RP2350B_PLUS_W_BUILD)
        set(PICO_BOARD waveshare_rp2350b_plus_w CACHE STRING "Board type")
        # The Waveshare board header is not in upstream pico-sdk; ship it here
        # and tell the SDK to search this directory when resolving PICO_BOARD.
        # It wires the RM2/CYW43 module to different RP2350 GPIOs than the Pico 2 W
        # (WL_REG_ON GP36, WL_DATA GP37, WL_CS GP38, WL_CLOCK GP39), so a stock
        # pico2_w build can't drive the radio on this board.
        list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/boards/headers")
else ()
        set(PICO_BOARD pico2_w CACHE STRING "Board type")
endif ()

message(STATUS "DS5-Linux-Bridge build target -- PICO_BOARD=${PICO_BOARD}")
message(STATUS "DS5-Linux-Bridge build target -- PICO_W_BUILD=${PICO_W_BUILD}")
message(STATUS "DS5-Linux-Bridge build target -- WAVESHARE_RP2350B_PLUS_W_BUILD=${WAVESHARE_RP2350B_PLUS_W_BUILD}")
#set(PICO_CYW43_SUPPORTED 1)
#set(PICO_CYW43_ARCH_THREADSAFE_BACKGROUND 1)

# Diagnostics go over UART0 (GP0 TX, 115200 8N1), always available -- there is
# no USB-CDC serial. USB serial was removed because it perturbed the very USB
# timing it was meant to diagnose (and was unreliable). See README "Debugging".

# WiFi uplink (STA mode on the onboard CYW43 -- the same chip BTstack drives;
# the WiFi firmware blob is already in the image) for Wake-on-LAN only.
# Configuration is over USB in every build (src/hid_config.cpp). Sets
# CYW43_LWIP=1 so the SDK's cyw43_arch owns lwIP (DHCP client + the UDP magic
# packet) and wires it to the WiFi netif. OFF = no Wake-on-LAN.
option(ENABLE_WIFI_WOL "Join the home Wi-Fi (STA) to send Wake-on-LAN magic packets" ON)
if (ENABLE_WIFI_WOL)
    set(CYW43_LWIP_VALUE 1)
else ()
    set(CYW43_LWIP_VALUE 0)
endif ()
option(ENABLE_VERBOSE "Enable Log Verbose" OFF)
if(ENABLE_VERBOSE)
    set(ENABLE_VERBOSE_VALUE 1)
else()
    set(ENABLE_VERBOSE_VALUE 0)
endif ()
add_compile_definitions(ENABLE_VERBOSE=${ENABLE_VERBOSE_VALUE})
option(ENABLE_BATT_LED "Blink Pico LED when DualSense battery is low (<=10%)" ON)
if (ENABLE_BATT_LED)
        set(ENABLE_BATT_LED_VALUE 1)
else ()
        set(ENABLE_BATT_LED_VALUE 0)
endif ()
option(DISABLE_SPEAKER_PROC "Disable Speaker in controller" OFF)
if (DISABLE_SPEAKER_PROC)
        set(DISABLE_SPEAKER_PROC_VALUE 1)
else ()
        set(DISABLE_SPEAKER_PROC_VALUE 0)
endif ()
# Per-board clock / PIO config (PICO_W_BUILD declared near the top).
if (PICO_W_BUILD)
        set(CYW43_PIO_CLOCK_DIV_VALUE 3)
        set(SYS_CLOCK_KHZ_VALUE 200000)
        set(DISABLE_SPEAKER_PROC_VALUE 1)
else ()
        # Both RP2350 targets (Pico 2 W and Waveshare RP2350B-Plus-W) run the
        # stock 150 MHz clock. Selective RAM relocation (relocate_to_ram) keeps the
        # hot paths out of flash XIP, so no overclock is needed for stable audio +
        # 1000Hz polling. PIO div 2 matches the 150 MHz system clock.
        set(CYW43_PIO_CLOCK_DIV_VALUE 2)
        set(SYS_CLOCK_KHZ_VALUE 150000)
endif ()



# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(ds5-bridge C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(ds5-bridge
        src/usb_descriptors.cpp
        src/main.cpp
        src/usb.cpp
        src/audio.cpp
        src/bt.cpp
        src/dse.cpp
        src/config.cpp
        src/state_mgr.cpp
        src/tier.cpp
        src/flash_bank_safe.cpp
        src/weblog.cpp
        src/web_api.cpp     # config API (transport-agnostic route layer)
        src/hid_config.cpp  # USB HID config tunnel (web/index.html drives it)
        src/ram_mem.c
)

# RAM-resident memcpy/memset/memmove (src/ram_mem.c). Must be built with
# -fno-builtin and -fno-tree-loop-distribute-patterns so the compiler does not
# lower the word-copy loops back into calls to memcpy/memset (self-recursion).
set_source_files_properties(src/ram_mem.c PROPERTIES COMPILE_OPTIONS
        "-fno-builtin;-fno-tree-loop-distribute-patterns")

if (ENABLE_WIFI_WOL)
    # WiFi is the Wake-on-LAN uplink only (STA join + DHCP client + one UDP
    # magic packet). No web server, mDNS or captive portal -- configuration
    # is over USB (src/hid_config.cpp), in every build.
    target_sources(ds5-bridge PRIVATE src/wifi_net.cpp)
    target_compile_definitions(ds5-bridge PRIVATE ENABLE_WIFI_WOL)
    target_link_libraries(ds5-bridge pico_lwip_nosys)
endif ()

if (ENABLE_BATT_LED)
        target_sources(ds5-bridge PRIVATE src/battery_led.cpp)
endif ()

if (ENABLE_LED_STRIP)
        target_sources(ds5-bridge PRIVATE src/ledstrip.cpp)
        pico_generate_pio_header(ds5-bridge ${CMAKE_CURRENT_SOURCE_DIR}/src/ws2812.pio)
        target_compile_definitions(ds5-bridge PRIVATE
                ENABLE_LED_STRIP=1
                LED_STRIP_GPIO=${LED_STRIP_GPIO}
        )
        target_link_libraries(ds5-bridge hardware_pio)
endif ()


# Opus
#set(OPUS_FIXED_POINT ON)
add_subdirectory(lib/opus)

# Move libopus to RAM via .time_critical sections. libopus is built by
# add_subdirectory(lib/opus); the target `opus` is defined in the subdirectory's
# CMakeLists.txt, so we can't use POST_BUILD here. Instead use a stamp-file
# custom command + custom target that runs after opus is built and before
# ds5-bridge links. Renames .text/.rodata of libopus.a to .time_critical.* which
# the Pico SDK linker places in RAM (loaded from flash at boot, executed from RAM
# at runtime). ~220 KB relocated, ~5-6 ms boot-time impact for the copy.
#
# LOAD-BEARING: this is required for clean duplex audio. Opus's per-frame DSP AND
# its lookup tables (mdct/fft twiddles, CELT_PVQ_U_DATA, SILK codebooks) thrash the
# XIP cache when run from flash, causing audio stutter at stock 150 MHz. Upstream
# relocates the whole library for the same reason (commit 0adca8f). Do NOT drop
# this to free heap -- it trades clean audio for stutter. The web-config/audio RAM
# coexistence must be solved within the remaining heap, not by evicting Opus.
#
# EXCEPTION -- keep the tonality analyzer in flash (recover ~21 KB of RAM):
# analysis.c (run_analysis/tonality_analysis, ~16.5 KB) and its mlp_data.c table
# (~5 KB) are only used for auto-bitrate/complexity>=7 decisions. This firmware
# encodes at COMPLEXITY 0 with VBR OFF and a fixed bitrate (see audio.cpp), so the
# analysis path is NEVER executed at runtime -- it is pure dead weight in RAM.
# After the blanket relocation above, rename ONLY these two member objects' sections
# back to .text/.rodata so the linker leaves them in flash. Zero audio risk (never
# called => never XIP-fetched), ~21 KB of heap reclaimed. (Upstream relocated the
# whole library wholesale via one objcopy for submodule-pristine convenience, not
# because all of it is per-frame hot; this fork has heap pressure they did not.)
# NOTE: `ar x` extracts members into the process's working directory, and the
# subsequent objcopy/`ar r`/rm must all agree on that directory. add_custom_command
# does not guarantee a stable CWD (it differs between an in-source `build/` and an
# out-of-tree dir like the release CI's `build/standard`), which made objcopy fail
# with "analysis.c.obj: No such file". Pin an explicit scratch dir and reference the
# extracted members by absolute path so the sequence is build-dir-independent.
set(OPUS_RELOC_DIR ${CMAKE_BINARY_DIR}/opus_reloc)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp
    # Blanket-relocate libopus code + lookup tables into RAM. String literals
    # (.rodata.str1.4, ~11 KB of error/assert messages) deliberately stay in
    # flash: they are never touched on the per-frame audio path, so they cost
    # zero XIP pressure -- and the heap needs the RAM (see the OOM note in
    # audio.cpp core1_entry).
    COMMAND ${CMAKE_OBJCOPY} --rename-section .text=.time_critical.opus_text --rename-section .rodata=.time_critical.opus_rodata $<TARGET_FILE:opus>
    # Then un-relocate the never-executed analysis path back to flash. This is a
    # cmake -P helper (not inline COMMANDs) because the archive member names are
    # not stable across toolchains (.c.obj here vs .c.o in CI), so it must query
    # `ar t` and match by basename. See cmake/opus_unrelocate_analysis.cmake.
    COMMAND ${CMAKE_COMMAND}
        -DAR=${CMAKE_AR}
        -DOBJCOPY=${CMAKE_OBJCOPY}
        -DARCHIVE=$<TARGET_FILE:opus>
        -DWORKDIR=${OPUS_RELOC_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/opus_unrelocate_analysis.cmake
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp
    DEPENDS $<TARGET_FILE:opus> ${CMAKE_SOURCE_DIR}/cmake/opus_unrelocate_analysis.cmake
    COMMENT "Relocating libopus to RAM (tonality analyzer kept in flash)"
    VERBATIM
)
add_custom_target(opus_ram_relocate ALL DEPENDS ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp)
add_dependencies(ds5-bridge opus_ram_relocate)

target_compile_options(ds5-bridge PRIVATE -fsigned-char)

# Flash XIP timing. boot2 (w25q080) applies PICO_FLASH_SPI_CLKDIV; div 2 gives a
# safe flash SPI clock at our stock 150 MHz system clock (a higher overclock
# would need div 4). On RP2350 boot2 only runs when PICO_EMBED_XIP_SETUP=1.
if (${SYS_CLOCK_KHZ_VALUE} GREATER 266000)
        set(PICO_FLASH_SPI_CLKDIV 4)
else ()
        set(PICO_FLASH_SPI_CLKDIV 2)
endif ()
target_compile_definitions(bs2_default PRIVATE PICO_FLASH_SPI_CLKDIV=${PICO_FLASH_SPI_CLKDIV})

if (WAVESHARE_RP2350B_PLUS_W_BUILD)
        # The Waveshare board ships a Puya PY25Q128HA flash (not the schematic's
        # Winbond). Without forcing boot2 to run, the bootrom auto-detects from
        # SFDP and leaves XIP at CLKDIV=3 + Fast Read Dual I/O, which exceeds
        # Puya's dual-mode spec and intermittently corrupts XIP reads (choppy
        # audio). PICO_EMBED_XIP_SETUP=1 makes boot2_w25q080 actually run and
        # apply our PICO_FLASH_SPI_CLKDIV above + Fast Read Quad I/O.
        target_compile_definitions(ds5-bridge PRIVATE PICO_EMBED_XIP_SETUP=1)
endif ()

target_compile_definitions(ds5-bridge PRIVATE
        MULTI_SLOT_COUNT=${MULTI_SLOT_COUNT}
        # BTstack's TLV flash bank (link keys / bonds). The SDK default puts
        # the 2-sector bank in the TOP two sectors -- but the RP2350 bootrom
        # erases the LAST sector on every BOOTSEL/picotool reflash (the same
        # quirk that used to wipe the config), so half the bank died on every
        # other reflash: bonds vanished and pads silently re-paired. Move the
        # bank down one sector, leaving the last sector sacrificial:
        #   image .. | cfg(-4) | btstack bank(-3,-2) | bootrom-erased(-1)
        # One-time effect: bonds stored at the old location are lost once.
        PICO_FLASH_BANK_STORAGE_OFFSET=\(PICO_FLASH_SIZE_BYTES-3*FLASH_SECTOR_SIZE\)
        # WiFi builds hand lwIP to the SDK's cyw43_arch (it creates/owns the
        # WiFi netif and calls lwip_init); with ENABLE_WIFI_WOL off there is
        # no lwIP at all.
        CYW43_LWIP=${CYW43_LWIP_VALUE}
        # 0 (not 1): flash_safe_execute() must really park core1 while flash is
        # erased/programmed (config_save). With =1 the lockout is skipped and the
        # flash op races the audio core -> buzzing / crash when saving during
        # playback. core1_entry registers via flash_safe_execute_core_init().
        PICO_FLASH_ASSUME_CORE1_SAFE=0
        CYW43_PIO_CLOCK_DIV_INT=${CYW43_PIO_CLOCK_DIV_VALUE}
        ENABLE_VERBOSE=${ENABLE_VERBOSE_VALUE}
        ENABLE_BATT_LED=${ENABLE_BATT_LED_VALUE}
        SYS_CLOCK_KHZ=${SYS_CLOCK_KHZ_VALUE}
        DISABLE_SPEAKER_PROC=${DISABLE_SPEAKER_PROC_VALUE}
)

option(ENABLE_WAKE_HID "Wake the Windows host on PS-button press" ON)
if(ENABLE_WAKE_HID)
    target_compile_definitions(ds5-bridge PRIVATE
            ENABLE_WAKE_HID
    )
    target_sources(ds5-bridge PRIVATE
            src/wake.cpp
    )
endif()

option(WAKE_DEBUG "Print wake-FSM transitions to UART (developer use)" OFF)
if(WAKE_DEBUG)
    target_compile_definitions(ds5-bridge PRIVATE
            WAKE_DEBUG
    )
endif()

# Program version, shown in the web UI (/api/config "version") and by
# picotool. CI passes -DVERSION=<release tag | commit sha> explicitly. For
# local builds LEAVE IT UNSET: an auto version "<git-sha>[+] <UTC stamp>" is
# generated at configure time ("+" = uncommitted changes in the tree). The
# empty default is the "auto" sentinel; the old "dev" default is treated as
# auto too so stale caches upgrade themselves. RULE: never pass a hand-typed
# label here -- a cached "-DVERSION=ps-create-combo" from a long-dead branch
# haunted every local pico2_w build until 2026-07-19, which is exactly the
# confusion the auto stamp exists to prevent.
# Semver base for this fork. Bump by hand on meaningful milestones.
# 0.4.0: dynamic exposed-slot enumeration (grow on join, shrink only when the
#        last pad leaves); LED strip per-state animation/colors (idle default
#        off, critical keeps blinking); player-LED lock made reliable (1 Hz
#        re-assert, audio-slot output-report deferral fix). A pairing-gated
#        web UI shipped briefly and was reverted (broke page access).
# 0.3.0: Wake-on-LAN -- PS button wakes the PC from S4/S5 via magic packet
#        (S3 keeps USB remote-wakeup); Network tab gains device name, WOL
#        targets, Find-MAC and Wake-now.
# 0.2.0: NCM transport removed -- config page is WiFi-only, USB face is pure
#        DualSense (breaking for NCM-era clients; the Decky plugin should
#        discover via ds5.local from here on).
set(DS5_BASE_VERSION "0.4.0")
# Auto version: "<semver>-<build minute UTC>", e.g. 0.1.0-20260720-0142.
# Minute-sensitive so a flashed image is never ambiguous; the stamped UF2
# copy below carries the same string in its filename. Configure-time, and
# docker/build.sh reconfigures on every invocation, so each build.sh run
# stamps fresh (a bare `cmake --build` keeps the last configure's stamp).
set(VERSION "" CACHE STRING "Program version string (empty = auto: <semver>-<build minute>)")
string(TIMESTAMP DS5_FILE_STAMP "%Y%m%d-%H%M" UTC)
if (VERSION STREQUAL "" OR VERSION STREQUAL "dev")
    set(DS5_VERSION "${DS5_BASE_VERSION}-${DS5_FILE_STAMP}")
else ()
    set(DS5_VERSION "${VERSION}") # CI release tag / explicit override
endif ()
message(STATUS "DS5-Linux-Bridge version string: ${DS5_VERSION}")

pico_set_program_name(ds5-bridge "ds5-bridge")
pico_set_program_version(ds5-bridge "${DS5_VERSION}")

# Diagnostics go over UART0 only (GP0 TX, 115200 8N1). No USB stdio: USB-CDC
# serial perturbed the USB audio timing it was meant to diagnose.
pico_enable_stdio_uart(ds5-bridge 1)
pico_enable_stdio_usb(ds5-bridge 0)

# Add the standard include files to the build
#target_include_directories(ds5-bridge PRIVATE
#        ${CMAKE_CURRENT_LIST_DIR}
#)

target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add any user requested libraries
target_link_libraries(ds5-bridge
        pico_stdlib
        pico_multicore
        hardware_watchdog
        hardware_timer
        hardware_flash
        pico_btstack_classic
#        pico_cyw43_arch_threadsafe_background
        pico_cyw43_arch_poll
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
        opus
)

pico_add_extra_outputs(ds5-bridge)

# Stamped UF2 copy: ds5-bridge-<version>.uf2 next to the canonical
# ds5-bridge.uf2 (which CI and docs reference -- keep it). The filename
# carries the exact version string compiled into the image, so the file you
# flashed and the version the web UI reports pair up by name. Prior stamped
# copies are removed first so the directory holds exactly one. MUST come
# AFTER pico_add_extra_outputs: POST_BUILD steps run in declaration order,
# and until 2026-08-22 this sat above it, so the "stamped" file was the
# PREVIOUS build's UF2 (one build stale, wrong version inside).
add_custom_command(TARGET ds5-bridge POST_BUILD
        COMMAND sh -c "rm -f ds5-bridge-[0-9]*.uf2"
        COMMAND ${CMAKE_COMMAND} -E copy ds5-bridge.uf2 ds5-bridge-${DS5_VERSION}.uf2
        COMMENT "Stamped UF2: ds5-bridge-${DS5_VERSION}.uf2"
        VERBATIM)

# --- Selective RAM relocation ------------------------------------------------
# Instead of relocating the whole image into RAM (the old memmap_custom.ld
# approach, which needed the overclock to absorb the resulting pressure), keep
# .text in flash (stock linker script) and relocate only the specific hot
# functions that thrash the XIP cache. relocate_to_ram() renames the chosen
# .text.<fn> sections to .time_critical.<name> on the compiled objects in a
# PRE_LINK step; the Pico SDK linker places .time_critical.* in RAM (LMA flash,
# VMA RAM). The helper locates each object by its stable SDK-relative path
# SUFFIX. RENAMES are |-separated old=new pairs; objcopy skips absent sections,
# so this is idempotent across incremental relinks.
function(relocate_to_ram _suffix _renames _comment)
    add_custom_command(TARGET ds5-bridge PRE_LINK
        COMMAND ${CMAKE_COMMAND}
            "-DOBJROOT=${CMAKE_BINARY_DIR}/CMakeFiles/ds5-bridge.dir"
            "-DOBJCOPY=${CMAKE_OBJCOPY}"
            "-DSUFFIX=${_suffix}"
            "-DRENAMES=${_renames}"
            -P "${CMAKE_SOURCE_DIR}/cmake/relocate_to_ram.cmake"
        COMMENT "${_comment}"
        VERBATIM)
endfunction()

# Tier-1: per-packet BT receive + USB FIFO data path
relocate_to_ram(lib/btstack/src/hci.c.o
    ".text.packet_handler=.time_critical.bt_packet_handler"
    "relocate: hci.c packet_handler -> RAM")
relocate_to_ram(lib/btstack/src/l2cap.c.o
    ".text.l2cap_acl_handler=.time_critical.l2cap_acl_handler"
    "relocate: l2cap.c l2cap_acl_handler -> RAM")
relocate_to_ram(lib/tinyusb/src/common/tusb_fifo.c.o
    ".text.tu_fifo_read=.time_critical.tu_fifo_read|.text.tu_fifo_read_n=.time_critical.tu_fifo_read_n|.text.tu_fifo_write=.time_critical.tu_fifo_write|.text.tu_fifo_write_n=.time_critical.tu_fifo_write_n|.text.tu_fifo_count=.time_critical.tu_fifo_count"
    "relocate: tusb_fifo.c data path -> RAM")
relocate_to_ram(src/rp2_common/pico_cyw43_driver/cyw43_bus_pio_spi.c.o
    ".text.cyw43_spi_transfer=.time_critical.cyw43_spi_transfer|.text.cyw43_read_bytes=.time_critical.cyw43_read_bytes|.text.cyw43_write_bytes=.time_critical.cyw43_write_bytes|.text.cyw43_read_reg_u32=.time_critical.cyw43_read_reg_u32|.text.cyw43_read_reg_u16=.time_critical.cyw43_read_reg_u16|.text.cyw43_read_reg_u8=.time_critical.cyw43_read_reg_u8|.text.cyw43_write_reg_u32=.time_critical.cyw43_write_reg_u32|.text.cyw43_write_reg_u16=.time_critical.cyw43_write_reg_u16|.text.cyw43_write_reg_u8=.time_critical.cyw43_write_reg_u8|.text.read_reg_u32_swap=.time_critical.read_reg_u32_swap|.text.write_reg_u32_swap=.time_critical.write_reg_u32_swap|.text.ns_delay.constprop.0=.time_critical.cyw43_ns_delay"
    "relocate: cyw43 PIO-SPI transport -> RAM")
relocate_to_ram(lib/cyw43-driver/src/cyw43_ll.c.o
    ".text.cyw43_ll_sdpcm_poll_device=.time_critical.cyw43_ll_sdpcm_poll_device|.text.cyw43_ll_process_packets=.time_critical.cyw43_ll_process_packets|.text.cyw43_sdpcm_send_common=.time_critical.cyw43_sdpcm_send_common|.text.cyw43_do_ioctl.part.0=.time_critical.cyw43_do_ioctl|.text.cyw43_ll_bt_has_work=.time_critical.cyw43_ll_bt_has_work|.text.cyw43_ll_parse_async_event=.time_critical.cyw43_ll_parse_async_event"
    "relocate: cyw43 SDPCM pump -> RAM")
relocate_to_ram(lib/cyw43-driver/src/cyw43_ctrl.c.o
    ".text.cyw43_poll_func=.time_critical.cyw43_poll_func|.text.cyw43_bluetooth_hci_read=.time_critical.cyw43_bluetooth_hci_read|.text.cyw43_bluetooth_hci_write=.time_critical.cyw43_bluetooth_hci_write"
    "relocate: cyw43 poll + BT HCI bus -> RAM")

# Tier-2: run-loop TX pumps + per-transfer submit paths.
# state_update is intentionally NOT moved: it runs on the host->controller
# HID-OUT (rumble/LED) path, not the audio frame path, so it stays in flash.
relocate_to_ram(lib/btstack/src/hci.c.o
    ".text.hci_run=.time_critical.hci_run|.text.hci_send_acl_packet_fragments=.time_critical.hci_send_acl_packet_fragments|.text.hci_send_acl_packet_buffer=.time_critical.hci_send_acl_packet_buffer"
    "relocate: hci.c TX pump + ACL out -> RAM")
relocate_to_ram(lib/btstack/src/l2cap.c.o
    ".text.l2cap_run=.time_critical.l2cap_run|.text.l2cap_send=.time_critical.l2cap_send|.text.l2cap_send_prepared=.time_critical.l2cap_send_prepared|.text.l2cap_notify_channel_can_send=.time_critical.l2cap_notify_channel_can_send"
    "relocate: l2cap.c TX pump + sends -> RAM")
relocate_to_ram(lib/cyw43-driver/src/cyw43_ll.c.o
    ".text.cyw43_ll_write_backplane_mem=.time_critical.cyw43_ll_write_backplane_mem|.text.cyw43_ll_read_backplane_mem=.time_critical.cyw43_ll_read_backplane_mem|.text.cyw43_write_backplane=.time_critical.cyw43_write_backplane|.text.cyw43_read_backplane.constprop.0=.time_critical.cyw43_read_backplane"
    "relocate: cyw43 backplane accessors -> RAM")
relocate_to_ram(lib/tinyusb/src/device/usbd.c.o
    ".text.tud_task_ext=.time_critical.tud_task_ext|.text.usbd_edpt_xfer=.time_critical.usbd_edpt_xfer|.text.usbd_edpt_xfer_fifo=.time_critical.usbd_edpt_xfer_fifo"
    "relocate: usbd.c task pump + xfer -> RAM")
relocate_to_ram(lib/tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c.o
    ".text.dcd_edpt_xfer=.time_critical.dcd_edpt_xfer|.text.dcd_edpt_iso_activate=.time_critical.dcd_edpt_iso_activate"
    "relocate: dcd_rp2040.c submit + iso -> RAM")
relocate_to_ram(lib/tinyusb/src/class/audio/audio_device.c.o
    ".text.tud_audio_n_read=.time_critical.tud_audio_n_read|.text.tud_audio_n_write=.time_critical.tud_audio_n_write|.text.audiod_calc_tx_packet_sz.isra.0=.time_critical.audiod_calc_tx_packet_sz|.text.audiod_xfer_isr=.time_critical.audiod_xfer_isr"
    "relocate: audio_device.c data path -> RAM")

# Tier-3: input-report path tail (controller -> host).
# queue_try_* also back the audio inter-core FIFOs, so this helps both paths.
relocate_to_ram(lib/tinyusb/src/class/hid/hid_device.c.o
    ".text.tud_hid_n_report=.time_critical.tud_hid_n_report|.text.tud_hid_n_ready=.time_critical.tud_hid_n_ready|.text.hidd_xfer_cb=.time_critical.hidd_xfer_cb"
    "relocate: hid_device.c report path -> RAM")
relocate_to_ram(src/common/pico_util/queue.c.o
    ".text.queue_try_add=.time_critical.queue_try_add|.text.queue_try_remove=.time_critical.queue_try_remove"
    "relocate: pico_util queue add/remove -> RAM")
relocate_to_ram(src/rp2_common/pico_cyw43_arch/cyw43_arch.c.o
    ".text.cyw43_arch_poll=.time_critical.cyw43_arch_poll"
    "relocate: cyw43_arch_poll -> RAM")
relocate_to_ram(lib/btstack/src/btstack_run_loop.c.o
    ".text.btstack_run_loop_base_poll_data_sources=.time_critical.btstack_run_loop_base_poll_data_sources|.text.btstack_run_loop_poll_data_sources_from_irq=.time_critical.btstack_run_loop_poll_data_sources_from_irq"
    "relocate: btstack run-loop poll -> RAM")
