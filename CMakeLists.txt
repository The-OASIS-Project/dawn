cmake_minimum_required(VERSION 3.10)

project(dawn LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform selection
set(PLATFORM "AUTO" CACHE STRING "Choose platform: AUTO, JETSON, RPI")
set_property(CACHE PLATFORM PROPERTY STRINGS AUTO JETSON RPI)

# Platform detection
if(PLATFORM STREQUAL "AUTO")
    # Check if it's a Jetson board first
    if(EXISTS "/etc/nv_tegra_release")
        set(PLATFORM "JETSON")
        file(READ "/etc/nv_tegra_release" JETSON_MODEL)
        message(STATUS "Detected Jetson: ${JETSON_MODEL}")
    # Then check if it's a Raspberry Pi (both ARM and ARM64)
    elseif(EXISTS "/sys/firmware/devicetree/base/model")
        file(READ "/sys/firmware/devicetree/base/model" RPI_MODEL)
        if(RPI_MODEL MATCHES "Raspberry Pi")
            set(PLATFORM "RPI")
            message(STATUS "Detected RPi: ${RPI_MODEL}")
        endif()
    # Default platform detection based on processor architecture
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        message(STATUS "Detected generic ARM64 platform (assuming no GPU)")
        set(PLATFORM "RPI")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv")
        message(STATUS "Detected generic ARM platform (assuming no GPU)")
        set(PLATFORM "RPI")
    endif()
endif()

# Platform definitions
add_definitions(-DPLATFORM_${PLATFORM})
message(STATUS "Building for platform: ${PLATFORM}")

# Define the version number in the header file directly
set(VERSION_NUMBER "1.0.0")

# Try to get the current Git SHA
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_SHA_RESULT
)

# Check if the command was successful
if(NOT GIT_SHA_RESULT EQUAL 0)
    set(GIT_SHA "unknown")
endif()

# Pass version info to the compiler
add_definitions(-DGIT_SHA=\"${GIT_SHA}\")
add_definitions(-DVERSION_NUMBER=\"${VERSION_NUMBER}\")

# Include directories for new source layout
include_directories(include)

# HTTP connections (AI and such)
find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIRS})

# Local config, MQTT commmands, AI APIs
pkg_check_modules(JSONC REQUIRED json-c)
include_directories(${JSONC_INCLUDE_DIRS})

# MQTT
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
include_directories(${MOSQUITTO_INCLUDE_DIRS})

# Audio file playback
pkg_check_modules(FLAC REQUIRED flac)
include_directories(${FLAC_INCLUDE_DIRS})

# Base64 Encoding
find_package(OpenSSL REQUIRED)
include_directories(${OpenSSL_INCLUDE_DIR})

# TUI (ncurses)
option(ENABLE_TUI "Enable Terminal UI" ON)
if(ENABLE_TUI)
    find_package(Curses REQUIRED)
    if(CURSES_FOUND)
        include_directories(${CURSES_INCLUDE_DIR})
        add_definitions(-DENABLE_TUI)
        message(STATUS "TUI support: ENABLED (ncurses)")
    else()
        message(WARNING "ncurses not found, TUI support disabled")
        set(ENABLE_TUI OFF)
    endif()
endif()

# WebUI (libwebsockets + libopus)
option(ENABLE_WEBUI "Enable WebUI server" ON)
if(ENABLE_WEBUI)
    pkg_check_modules(WEBSOCKETS REQUIRED libwebsockets)
    pkg_check_modules(OPUS opus)
    if(WEBSOCKETS_FOUND)
        include_directories(${WEBSOCKETS_INCLUDE_DIRS})
        add_definitions(-DENABLE_WEBUI)
        message(STATUS "WebUI support: ENABLED (libwebsockets ${WEBSOCKETS_VERSION})")
        if(OPUS_FOUND)
            include_directories(${OPUS_INCLUDE_DIRS})
            add_definitions(-DENABLE_WEBUI_AUDIO)
            message(STATUS "WebUI audio: ENABLED (libopus ${OPUS_VERSION})")
        else()
            message(WARNING "libopus not found, WebUI audio support disabled (text-only mode)")
            message(WARNING "Install with: sudo apt install libopus-dev")
        endif()
    else()
        message(WARNING "libwebsockets not found, WebUI support disabled")
        message(WARNING "Install with: sudo apt install libwebsockets-dev")
        set(ENABLE_WEBUI OFF)
    endif()
endif()

# Platform-specific CUDA configuration (Jetson only)
if(PLATFORM STREQUAL "JETSON")
    set(CUDA_TOOLKIT_ROOT_DIR "/usr/local/cuda-12.6")
    set(ENABLE_WHISPER_CUDA ON CACHE BOOL "Enable CUDA for Whisper on Jetson" FORCE)

    # Find the required CUDA libraries and add them to the link libraries
    find_library(CUDA_LIBRARIES NAMES cuda HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    find_library(CUDART_LIBRARIES NAMES cudart HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    find_library(CUSPARSE_LIBRARIES NAMES cusparse HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    find_library(CUBLAS_LIBRARIES NAMES cublas HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    find_library(CUSOLVER_LIBRARIES NAMES cusolver HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)
    find_library(CURAND_LIBRARIES NAMES curand HINTS ${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux/lib)

    add_definitions(-DHAVE_CUDA_GPU)
    message(STATUS "CUDA support: ENABLED (Jetson platform)")
else()
    set(ENABLE_WHISPER_CUDA OFF CACHE BOOL "CUDA disabled on non-Jetson platforms" FORCE)
    message(STATUS "CUDA support: DISABLED (Raspberry Pi or other non-CUDA platform)")
endif()

# Wave file handling
#find_package(PkgConfig QUIET)
#if(PKG_CONFIG_FOUND)
#    pkg_check_modules(LIBSNDFILE_PKGCONF sndfile)
#endif(PKG_CONFIG_FOUND)

# Text to Speech
add_library(piper STATIC src/tts/piper.cpp)
target_link_libraries(piper onnxruntime)

# Audio Backend - Runtime Selectable (both ALSA and PulseAudio supported)
# The backend can be selected at runtime via config file: [audio] backend = "auto|alsa|pulse"
# At compile time, we need both libraries for runtime selection

# Find PulseAudio (optional, but recommended for desktop systems)
find_package(PulseAudio QUIET)
if(PULSEAUDIO_FOUND)
    include_directories(${PULSEAUDIO_INCLUDE_DIRS})
    message(STATUS "Audio backend: Runtime selectable (ALSA + PulseAudio)")
else()
    message(STATUS "Audio backend: ALSA only (PulseAudio not found)")
endif()

# Legacy option - kept for backward compatibility. No longer affects compilation.
# Audio backend selection is now done at runtime via --audio-backend CLI option (default: auto).
option(USE_ALSA "Deprecated: Audio backend is now selected at runtime" ON)

# =============================================================================
# Resampler library (required for 48kHz→16kHz capture conversion)
# =============================================================================
pkg_check_modules(SAMPLERATE REQUIRED samplerate)
if(NOT SAMPLERATE_FOUND)
    message(FATAL_ERROR
        "libsamplerate not found.\n"
        "Install with: sudo apt-get install libsamplerate0-dev")
endif()
include_directories(${SAMPLERATE_INCLUDE_DIRS})
message(STATUS "libsamplerate: Found (${SAMPLERATE_VERSION})")

# =============================================================================
# AEC Support (Acoustic Echo Cancellation) - WebRTC AEC3
# =============================================================================
# Note: Speex AEC was evaluated but removed due to ineffective echo cancellation
# in our architecture. WebRTC AEC3 provides significantly better performance.
# See git history for Speex implementation if needed.
#
# Options: ON (WebRTC), OFF
option(ENABLE_AEC "Enable WebRTC AEC3 echo cancellation" ON)

if(NOT ENABLE_AEC)
    message(STATUS "AEC: DISABLED (use -DENABLE_AEC=ON to enable WebRTC AEC3)")
    set(AEC_ENABLED FALSE)
else()
    set(AEC_ENABLED TRUE)
    message(STATUS "AEC: Checking WebRTC dependencies...")

    # WebRTC AEC3 backend
    set(WEBRTC_AP_ROOT ${CMAKE_SOURCE_DIR}/webrtc-audio-processing)
    set(WEBRTC_AP_BUILD ${WEBRTC_AP_ROOT}/build)

    # Check that submodule exists and was built
    if(NOT EXISTS "${WEBRTC_AP_ROOT}/webrtc/modules/audio_processing/include/audio_processing.h")
        message(FATAL_ERROR
            "WebRTC audio processing submodule not found or not initialized.\n"
            "Run: git submodule update --init --recursive\n"
            "Then: cd webrtc-audio-processing && meson setup build && ninja -C build")
    endif()

    if(NOT EXISTS "${WEBRTC_AP_BUILD}/webrtc/modules/audio_processing/libwebrtc-audio-processing-1.a")
        message(FATAL_ERROR
            "WebRTC audio processing not built.\n"
            "Run: cd webrtc-audio-processing && meson setup build && ninja -C build")
    endif()

    # Include paths for headers
    include_directories(${WEBRTC_AP_ROOT})
    include_directories(${WEBRTC_AP_ROOT}/webrtc)
    include_directories(${WEBRTC_AP_BUILD})
    include_directories(${WEBRTC_AP_BUILD}/webrtc)

    # Abseil includes for WebRTC
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ABSL REQUIRED
        absl_base
        absl_strings
        absl_synchronization
        absl_bad_optional_access
        absl_optional
        absl_bad_variant_access
        absl_variant
        absl_status
        absl_statusor
        absl_flags
        absl_flags_parse
    )
    include_directories(${ABSL_INCLUDE_DIRS})

    message(STATUS "  webrtc-audio-processing: Found (local build v1.3)")
    message(STATUS "AEC: ENABLED (WebRTC AEC3)")

    set(AEC_SOURCES
        src/audio/aec_webrtc.cpp
        src/audio/aec_calibration.c
    )
    set(AEC_LIBRARIES
        ${WEBRTC_AP_BUILD}/webrtc/modules/audio_processing/libwebrtc-audio-processing-1.a
        ${SAMPLERATE_LIBRARIES}
        ${ABSL_LIBRARIES}
    )
endif()

# ASR Engine Options
option(ENABLE_VOSK "Enable Vosk ASR engine (legacy support)" OFF)

# Whisper ASR (only build if Vosk is not enabled)
if(NOT ENABLE_VOSK)
    # Disable examples, tests, and server for DAWN integration
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)

    # Platform-specific Whisper CUDA configuration
    if(ENABLE_WHISPER_CUDA)
        set(GGML_CUDA ON CACHE BOOL "Enable CUDA for Whisper" FORCE)
        message(STATUS "Whisper CUDA: ENABLED")
    else()
        set(GGML_CUDA OFF CACHE BOOL "Disable CUDA for Whisper" FORCE)
        message(STATUS "Whisper CUDA: DISABLED")
    endif()

    add_subdirectory(whisper.cpp)
    message(STATUS "Building Whisper ASR")
else()
    message(STATUS "Skipping Whisper build (Vosk enabled)")
endif()

link_directories(${CMAKE_SOURCE_DIR})

# Base source files (always compiled)
set(DAWN_SOURCES
    # Core files
    src/dawn.c
    src/input_queue.c
    src/logging.c
    src/mosquitto_comms.c
    src/text_to_command_nuevo.c
    src/word_to_number.c

    # Multi-threaded core (session management, worker pool, command routing)
    src/core/session_manager.c
    src/core/worker_pool.c
    src/core/command_router.c

    # ASR subsystem
    src/asr/asr_interface.c
    src/asr/vad_silero.c
    src/asr/chunking_manager.c

    # LLM subsystem
    src/llm/llm_interface.c
    src/llm/llm_openai.c
    src/llm/llm_claude.c
    src/llm/llm_streaming.c
    src/llm/llm_command_parser.c
    src/llm/sse_parser.c
    src/llm/sentence_buffer.c

    # TTS subsystem
    src/tts/text_to_speech.cpp
    src/tts/tts_preprocessing.cpp

    # Network subsystem
    src/network/dawn_server.c
    src/network/dawn_network_audio.c
    src/network/dawn_wav_utils.c
    src/network/accept_thread.c

    # Audio subsystem
    src/audio/audio_capture_thread.c
    src/audio/ring_buffer.c
    src/audio/flac_playback.c
    src/audio/mic_passthrough.c
    src/audio/resampler.c
    src/audio/audio_backend.c
    src/audio/audio_alsa.c
    src/audio/audio_pulse.c
    src/audio/audio_converter.c

    # UI subsystem (TUI metrics)
    src/ui/metrics.c

    # Tools subsystem (web search, weather, calculator, etc.)
    src/tools/web_search.c
    src/tools/weather_service.c
    src/tools/calculator.c
    src/tools/tinyexpr.c
    src/tools/search_summarizer.c
    src/tools/url_fetcher.c
    src/tools/html_parser.c
    src/tools/string_utils.c
    src/tools/toml.c

    # Config subsystem
    src/config/config_defaults.c
    src/config/config_parser.c
    src/config/config_validate.c
    src/config/config_env.c
)

# Conditionally add TUI source file
if(ENABLE_TUI)
    list(APPEND DAWN_SOURCES src/ui/tui.c)
endif()

# Conditionally add WebUI source files
if(ENABLE_WEBUI)
    list(APPEND DAWN_SOURCES src/webui/webui_server.c)
    if(OPUS_FOUND)
        list(APPEND DAWN_SOURCES src/webui/webui_audio.c)
    endif()
endif()

# Note: ALSA_DEVICE define removed - audio backend is now selected at runtime via --audio-backend
# The USE_ALSA option is kept for backwards compatibility but has no effect on compilation.
# Both ALSA and PulseAudio backends are always compiled in and selected at runtime.

# Conditionally add ASR engine source files
if(ENABLE_VOSK)
    list(APPEND DAWN_SOURCES src/asr/asr_vosk.c)
    add_definitions(-DENABLE_VOSK)
    message(STATUS "ASR Engine: Vosk")
else()
    list(APPEND DAWN_SOURCES src/asr/asr_whisper.c)
    message(STATUS "ASR Engine: Whisper")
endif()

# Conditionally add AEC source files
if(AEC_ENABLED)
    list(APPEND DAWN_SOURCES ${AEC_SOURCES})
endif()

add_executable(dawn ${DAWN_SOURCES})

# AEC compile definitions (target-specific to avoid rebuilding whisper/ggml)
if(AEC_ENABLED)
    target_compile_definitions(dawn PRIVATE ENABLE_AEC)
    target_compile_definitions(dawn PRIVATE AEC_BACKEND_WEBRTC)
endif()

target_link_libraries(dawn
                      piper
                      piper_phonemize
                      espeak-ng
                      onnxruntime
                      pthread
                      sndfile
                      ${CURL_LIBRARIES}
                      ${JSONC_LIBRARIES}
                      ${SPDLOG_LIBRARIES}
                      ${MOSQUITTO_LIBRARIES}
                      ${FLAC_LIBRARIES}
                      ${SAMPLERATE_LIBRARIES}
                      OpenSSL::SSL
                      OpenSSL::Crypto)

# Link both audio libraries for runtime backend selection
# Both backends are statically linked; runtime selection via vtable
target_link_libraries(dawn asound pulse-simple pulse)

# Conditionally link ncurses for TUI
if(ENABLE_TUI)
    target_link_libraries(dawn ${CURSES_LIBRARIES})
endif()

# Conditionally link libwebsockets and libopus for WebUI
if(ENABLE_WEBUI)
    target_link_libraries(dawn ${WEBSOCKETS_LIBRARIES})
    if(OPUS_FOUND)
        target_link_libraries(dawn ${OPUS_LIBRARIES})
    endif()
endif()

# Conditionally link ASR library (Vosk OR Whisper, not both)
if(ENABLE_VOSK)
    target_link_libraries(dawn vosk)
    message(STATUS "Linking Vosk ASR library")
else()
    target_link_libraries(dawn whisper)
    message(STATUS "Linking Whisper ASR library")
endif()

# Conditionally link CUDA libraries AFTER ASR libraries (linker order matters!)
# CUDA is needed when:
# 1. Jetson platform with Whisper (for GPU acceleration)
# 2. Vosk is enabled (Vosk was built with CUDA support)
if((PLATFORM STREQUAL "JETSON" AND NOT ENABLE_VOSK) OR ENABLE_VOSK)
    target_link_libraries(dawn
                          ${CUDA_LIBRARIES}
                          ${CUDART_LIBRARIES}
                          ${CUSPARSE_LIBRARIES}
                          ${CUBLAS_LIBRARIES}
                          ${CUSOLVER_LIBRARIES}
                          ${CURAND_LIBRARIES})
    message(STATUS "Linking CUDA libraries")
endif()

# Link AEC libraries (must come after add_executable)
if(AEC_ENABLED)
    target_link_libraries(dawn ${AEC_LIBRARIES})
    message(STATUS "Linking AEC libraries (${AEC_BACKEND} backend)")
endif()

target_include_directories(piper PUBLIC
                           /usr/local/include/piper-phonemize
                           ${SPDLOG_INCLUDE_DIRS}
                           /usr/local/include/onnxruntime/)

set(DESTINATION_DIR
    ${CMAKE_BINARY_DIR}
)

file(COPY ${SOURCE_FILES} DESTINATION ${DESTINATION_DIR})

# Conditionally create Vosk model symlink
if(ENABLE_VOSK)
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/vosk-model-en-us-0.22 ${CMAKE_BINARY_DIR}/model)
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/commands_config_nuevo.json ${CMAKE_BINARY_DIR}/commands_config_nuevo.json)

# =============================================================================
# Tests (build with: make tests)
# =============================================================================
# Tests are not built by default. Run "make tests" to build all test executables.
# Individual tests can be built with "make test_<name>" after running cmake.
add_subdirectory(tests EXCLUDE_FROM_ALL)

