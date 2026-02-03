cmake_minimum_required(VERSION 3.10)

project(dawn_satellite LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2")

# Find required packages
find_package(PkgConfig REQUIRED)

# Protocol mode selection
# DAP = Original audio streaming (Tier 2)
# DAP2 = WebSocket text protocol (Tier 1, requires local ASR/TTS)
option(ENABLE_DAP2 "Enable DAP2 WebSocket protocol (Tier 1)" ON)

# ALSA for audio
pkg_check_modules(ALSA REQUIRED alsa)
include_directories(${ALSA_INCLUDE_DIRS})

# Optional: libgpiod for GPIO control (button, LEDs)
pkg_check_modules(GPIOD libgpiod)
if(GPIOD_FOUND)
    include_directories(${GPIOD_INCLUDE_DIRS})
    add_definitions(-DHAVE_GPIOD)
    message(STATUS "GPIO support: ENABLED (libgpiod)")
else()
    message(STATUS "GPIO support: DISABLED (install libgpiod-dev to enable)")
endif()

# DAP2 dependencies
if(ENABLE_DAP2)
    # libwebsockets for WebSocket client
    pkg_check_modules(LWS REQUIRED libwebsockets)
    include_directories(${LWS_INCLUDE_DIRS})

    # json-c for JSON parsing
    pkg_check_modules(JSONC REQUIRED json-c)
    include_directories(${JSONC_INCLUDE_DIRS})

    add_definitions(-DENABLE_DAP2)
    message(STATUS "Protocol: DAP2 (WebSocket, Tier 1)")
else()
    message(STATUS "Protocol: DAP (TCP audio, Tier 2)")
endif()

# Optional: framebuffer for SPI display (disabled by default)
option(ENABLE_DISPLAY "Enable SPI display support via framebuffer" OFF)
if(ENABLE_DISPLAY)
    add_definitions(-DENABLE_DISPLAY)
    message(STATUS "Display support: ENABLED (framebuffer)")
else()
    message(STATUS "Display support: DISABLED (console output)")
endif()

# Optional: NeoPixel LED support via SPI
option(ENABLE_NEOPIXEL "Enable NeoPixel/WS2812 LED support via SPI" ON)
if(ENABLE_NEOPIXEL)
    add_definitions(-DENABLE_NEOPIXEL)
    message(STATUS "NeoPixel support: ENABLED (SPI)")
else()
    message(STATUS "NeoPixel support: DISABLED")
endif()

# Source files - common
set(SATELLITE_SOURCES
    src/main.c
    src/audio_capture.c
    src/audio_playback.c
    src/satellite_state.c
    src/satellite_config.c
    src/toml.c
)

# Protocol-specific sources
if(ENABLE_DAP2)
    list(APPEND SATELLITE_SOURCES src/ws_client.c)
else()
    list(APPEND SATELLITE_SOURCES src/dap_client.c)
endif()

# Conditional sources
if(GPIOD_FOUND)
    list(APPEND SATELLITE_SOURCES src/gpio_control.c)
endif()

if(ENABLE_DISPLAY)
    list(APPEND SATELLITE_SOURCES src/display.c)
endif()

if(ENABLE_NEOPIXEL)
    list(APPEND SATELLITE_SOURCES src/neopixel.c)
endif()

# Include directories
include_directories(include)

# Include dawn_common library from parent project
# This provides ring_buffer, sentence_buffer, string_utils, logging_common
set(DAWN_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
if(EXISTS "${DAWN_COMMON_DIR}/CMakeLists.txt")
    add_subdirectory("${DAWN_COMMON_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dawn_common")
    include_directories("${DAWN_COMMON_DIR}/include")
    message(STATUS "Using dawn_common library from parent project")
else()
    message(WARNING "dawn_common not found - some features may not work")
endif()

# Executable
add_executable(dawn_satellite ${SATELLITE_SOURCES})

# Link libraries
target_link_libraries(dawn_satellite
    ${ALSA_LIBRARIES}
    pthread
    m
)

# Link dawn_common if available
if(TARGET dawn_common)
    target_link_libraries(dawn_satellite dawn_common)
endif()

if(GPIOD_FOUND)
    target_link_libraries(dawn_satellite ${GPIOD_LIBRARIES})
endif()

if(ENABLE_DAP2)
    target_link_libraries(dawn_satellite ${LWS_LIBRARIES} ${JSONC_LIBRARIES})
endif()

# Installation
install(TARGETS dawn_satellite DESTINATION bin)
install(FILES config/satellite.toml DESTINATION etc/dawn)

# Print configuration summary
message(STATUS "")
message(STATUS "=== DAWN Satellite Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Target: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "=====================================")
