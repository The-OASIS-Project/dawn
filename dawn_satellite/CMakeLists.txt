cmake_minimum_required(VERSION 3.10)

project(dawn_satellite LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# Local voice processing: VAD + ASR + TTS on the satellite
# This enables fully local wake word detection and speech processing
# Default ON for production builds; use -DENABLE_VOICE=OFF for text-only debugging
option(ENABLE_VOICE "Enable local voice processing (VAD + ASR + TTS)" ON)

# Set individual flags from the unified option
set(ENABLE_LOCAL_VAD ${ENABLE_VOICE})
set(ENABLE_LOCAL_ASR ${ENABLE_VOICE})
set(ENABLE_LOCAL_TTS ${ENABLE_VOICE})

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
    # Voice processing requires DAP2 (text-based protocol)
    list(APPEND SATELLITE_SOURCES src/voice_processing.c)
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

# Platform-specific CUDA configuration (same as main server)
set(CUDA_TOOLKIT_ROOT_DIR "/usr/local/cuda-12.6")
find_library(CUDA_LIBRARIES NAMES cuda HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
if(CUDA_LIBRARIES)
    set(ENABLE_WHISPER_CUDA ON)
    find_library(CUDART_LIBRARIES NAMES cudart HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    find_library(CUSPARSE_LIBRARIES NAMES cusparse HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    find_library(CUBLAS_LIBRARIES NAMES cublas HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    find_library(CUSOLVER_LIBRARIES NAMES cusolver HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    find_library(CURAND_LIBRARIES NAMES curand HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    message(STATUS "CUDA support: ENABLED")
else()
    set(ENABLE_WHISPER_CUDA OFF)
    message(STATUS "CUDA support: DISABLED (not found)")
endif()

# Include whisper.cpp for local ASR (must be before dawn_common so target exists)
if(ENABLE_LOCAL_ASR)
    set(WHISPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../whisper.cpp")
    if(EXISTS "${WHISPER_DIR}/CMakeLists.txt")
        set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GGML_METAL OFF CACHE BOOL "" FORCE)

        # Enable CUDA if available
        if(ENABLE_WHISPER_CUDA)
            set(GGML_CUDA ON CACHE BOOL "" FORCE)
            message(STATUS "whisper.cpp: ENABLED (CUDA mode)")
        else()
            set(GGML_CUDA OFF CACHE BOOL "" FORCE)
            message(STATUS "whisper.cpp: ENABLED (CPU mode)")
        endif()

        add_subdirectory("${WHISPER_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/whisper.cpp")
    else()
        message(FATAL_ERROR "whisper.cpp not found at ${WHISPER_DIR} - required for local ASR")
    endif()
endif()

# Include dawn_common library from parent project
# This provides ring_buffer, sentence_buffer, string_utils, logging_common
# Optional: dawn_common_vad, dawn_common_asr, dawn_common_tts
set(DAWN_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
if(EXISTS "${DAWN_COMMON_DIR}/CMakeLists.txt")
    # Configure common library options based on satellite options
    set(DAWN_COMMON_WITH_VAD ${ENABLE_LOCAL_VAD} CACHE BOOL "" FORCE)
    set(DAWN_COMMON_WITH_ASR ${ENABLE_LOCAL_ASR} CACHE BOOL "" FORCE)
    set(DAWN_COMMON_WITH_TTS ${ENABLE_LOCAL_TTS} CACHE BOOL "" FORCE)

    # Reduce sentence buffer limit for Pi Zero 2 W (256KB instead of 10MB)
    # Prevents OOM on memory-constrained satellite devices
    set(DAWN_SENTENCE_BUFFER_MAX_SIZE 262144 CACHE STRING "" FORCE)

    add_subdirectory("${DAWN_COMMON_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dawn_common")
    include_directories("${DAWN_COMMON_DIR}/include")
    # Also include main project's include for piper.hpp, json.hpp etc
    include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../include")
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

# Link optional VAD/ASR/TTS libraries
if(ENABLE_LOCAL_VAD AND TARGET dawn_common_vad)
    target_link_libraries(dawn_satellite dawn_common_vad)
    target_compile_definitions(dawn_satellite PRIVATE ENABLE_LOCAL_VAD HAVE_VAD_SILERO)
    message(STATUS "Local VAD: ENABLED")
endif()

if(ENABLE_LOCAL_ASR AND TARGET dawn_common_asr)
    target_link_libraries(dawn_satellite dawn_common_asr)
    target_compile_definitions(dawn_satellite PRIVATE ENABLE_LOCAL_ASR HAVE_ASR_WHISPER)
    message(STATUS "Local ASR: ENABLED")

    # Link CUDA libraries if available (must be after ASR library for linker order)
    if(ENABLE_WHISPER_CUDA)
        target_link_libraries(dawn_satellite
            ${CUDA_LIBRARIES}
            ${CUDART_LIBRARIES}
            ${CUSPARSE_LIBRARIES}
            ${CUBLAS_LIBRARIES}
            ${CUSOLVER_LIBRARIES}
            ${CURAND_LIBRARIES})
        message(STATUS "Linking CUDA libraries for ASR")
    endif()
endif()

if(ENABLE_LOCAL_TTS AND TARGET dawn_common_tts)
    target_link_libraries(dawn_satellite dawn_common_tts)
    target_compile_definitions(dawn_satellite PRIVATE ENABLE_LOCAL_TTS HAVE_TTS_PIPER)
    message(STATUS "Local TTS: ENABLED")
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
message(STATUS "Local VAD: ${ENABLE_LOCAL_VAD}")
message(STATUS "Local ASR: ${ENABLE_LOCAL_ASR}")
message(STATUS "Local TTS: ${ENABLE_LOCAL_TTS}")
message(STATUS "=====================================")
