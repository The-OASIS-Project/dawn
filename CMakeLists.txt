cmake_minimum_required(VERSION 3.10)

project(dawn LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standard install paths (/usr/local/bin, /usr/local/lib, etc.)
include(GNUInstallDirs)

# RPATH: build-tree binary finds libs in-tree; installed binary uses install libdir
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)

# Platform selection
set(PLATFORM "AUTO" CACHE STRING "Choose platform: AUTO, JETSON, RPI")
set_property(CACHE PLATFORM PROPERTY STRINGS AUTO JETSON RPI)

# Server-only mode: skip local audio hardware requirements
# Enables WebUI + satellite connections without local microphone/speaker
option(SERVER_ONLY "Server-only mode: no local audio capture/playback" OFF)

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

# VERSION_NUMBER is the single source of truth in include/version.h — do NOT also
# define it here (a -D would redefine the header's value and warn).  Only GIT_SHA,
# computed at configure time, is passed through as a -D below.

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

# Pass the Git SHA to the compiler (VERSION_NUMBER lives in include/version.h).
add_definitions(-DGIT_SHA=\"${GIT_SHA}\")

# Include directories for new source layout
include_directories(include)
include_directories(common/include)

# =============================================================================
# Common Library (shared between daemon and satellites)
# =============================================================================
# Added after whisper.cpp so that dawn_common_asr can find the whisper target.
# See the add_subdirectory(common) call after the whisper.cpp block below.

# HTTP connections (AI and such)
find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIRS})

# Local config, MQTT commmands, AI APIs
pkg_check_modules(JSONC REQUIRED json-c)
include_directories(${JSONC_INCLUDE_DIRS})

# MQTT
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
include_directories(${MOSQUITTO_INCLUDE_DIRS})

# Audio file playback - FLAC (always required)
pkg_check_modules(FLAC REQUIRED flac)
include_directories(${FLAC_INCLUDE_DIRS})

# Audio file playback - MP3 (optional, libmpg123)
option(DAWN_ENABLE_MP3 "Enable MP3 decoding (libmpg123)" ON)
if(DAWN_ENABLE_MP3)
    pkg_check_modules(MPG123 libmpg123)
    if(MPG123_FOUND)
        include_directories(${MPG123_INCLUDE_DIRS})
        add_definitions(-DDAWN_ENABLE_MP3)
        message(STATUS "MP3 support: ENABLED (libmpg123 ${MPG123_VERSION})")
    else()
        message(WARNING "libmpg123 not found - MP3 support disabled")
        message(WARNING "Install with: sudo apt install libmpg123-dev")
        set(DAWN_ENABLE_MP3 OFF)
    endif()
endif()

# Audio file playback - Ogg Vorbis (optional, libvorbis)
option(DAWN_ENABLE_OGG "Enable Ogg Vorbis decoding (libvorbis)" ON)
if(DAWN_ENABLE_OGG)
    pkg_check_modules(VORBISFILE vorbisfile)
    pkg_check_modules(VORBIS vorbis)
    if(VORBISFILE_FOUND AND VORBIS_FOUND)
        include_directories(${VORBISFILE_INCLUDE_DIRS})
        add_definitions(-DDAWN_ENABLE_OGG)
        message(STATUS "Ogg support: ENABLED (libvorbisfile ${VORBISFILE_VERSION}, libvorbis ${VORBIS_VERSION})")
    else()
        message(WARNING "libvorbisfile or libvorbis not found - Ogg Vorbis support disabled")
        message(WARNING "Install with: sudo apt install libvorbis-dev")
        set(DAWN_ENABLE_OGG OFF)
    endif()
endif()

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

# Authentication subsystem (required if WebUI is enabled)
# This is a derived option - not directly configurable
if(ENABLE_WEBUI)
    set(ENABLE_AUTH ON)
    add_definitions(-DENABLE_AUTH)
    message(STATUS "Authentication: ENABLED (network features active)")

    # Auth dependencies: libsodium (Argon2id) and SQLite3 (auth database)
    pkg_check_modules(SODIUM REQUIRED libsodium)
    if(NOT SODIUM_FOUND)
        message(FATAL_ERROR
            "libsodium not found.\n"
            "Install with: sudo apt-get install libsodium-dev")
    endif()
    include_directories(${SODIUM_INCLUDE_DIRS})
    message(STATUS "  libsodium: Found (${SODIUM_VERSION})")

    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    if(NOT SQLITE3_FOUND)
        message(FATAL_ERROR
            "SQLite3 not found.\n"
            "Install with: sudo apt-get install libsqlite3-dev")
    endif()
    include_directories(${SQLITE3_INCLUDE_DIRS})
    message(STATUS "  SQLite3: Found (${SQLITE3_VERSION})")
else()
    set(ENABLE_AUTH OFF)
    message(STATUS "Authentication: DISABLED (no network features)")
endif()

# SQLite3 is always required (music metadata database + auth when enabled)
if(NOT SQLITE3_FOUND)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    if(NOT SQLITE3_FOUND)
        message(FATAL_ERROR
            "SQLite3 not found.\n"
            "Install with: sudo apt-get install libsqlite3-dev")
    endif()
    include_directories(${SQLITE3_INCLUDE_DIRS})
    message(STATUS "SQLite3: Found (${SQLITE3_VERSION}) - required for music metadata")
endif()

# libsodium is always required (crypto_store + memory + calendar use it,
# independent of the auth/WebUI subsystem — they are compiled in every preset).
if(NOT SODIUM_FOUND)
    pkg_check_modules(SODIUM REQUIRED libsodium)
    if(NOT SODIUM_FOUND)
        message(FATAL_ERROR
            "libsodium not found.\n"
            "Install with: sudo apt-get install libsodium-dev")
    endif()
    include_directories(${SODIUM_INCLUDE_DIRS})
    message(STATUS "libsodium: Found (${SODIUM_VERSION}) - required for crypto_store")
endif()

# Multi-client session support (required if WebUI is enabled)
# Provides session manager and worker pool for concurrent client handling
if(ENABLE_WEBUI)
    add_definitions(-DENABLE_MULTI_CLIENT)
    message(STATUS "Multi-client: ENABLED (session manager + worker pool)")
endif()

# Document extraction libraries (PDF, DOCX support for document upload)
# MuPDF: PDF text extraction (AGPL-3.0, GPL-compatible; dual-licensed AGPL/commercial)
# MuPDF ships as two static libraries: libmupdf.a + libmupdf-third.a (bundled freetype/zlib/etc)
find_library(MUPDF_LIB mupdf)
find_library(MUPDF_THIRD_LIB mupdf-third)
pkg_check_modules(MUPDF_PC mupdf)
if(MUPDF_LIB AND MUPDF_THIRD_LIB)
    set(MUPDF_FOUND TRUE)
    if(MUPDF_PC_FOUND)
        include_directories(${MUPDF_PC_INCLUDE_DIRS})
    endif()
    add_definitions(-DHAVE_MUPDF)
    message(STATUS "MuPDF: Found (PDF document extraction enabled)")
    message(STATUS "  libmupdf: ${MUPDF_LIB}")
    message(STATUS "  libmupdf-third: ${MUPDF_THIRD_LIB}")
else()
    set(MUPDF_FOUND FALSE)
    message(WARNING "MuPDF not found - PDF document extraction disabled")
    message(WARNING "Install with: sudo apt install libmupdf-dev")
endif()

# libzip: DOCX archive extraction (BSD-3-Clause)
pkg_check_modules(LIBZIP libzip)
if(LIBZIP_FOUND)
    include_directories(${LIBZIP_INCLUDE_DIRS})
    add_definitions(-DHAVE_LIBZIP)
    message(STATUS "libzip: Found (DOCX document extraction enabled)")
else()
    message(WARNING "libzip not found - DOCX document extraction disabled")
    message(WARNING "Install with: sudo apt install libzip-dev")
endif()

# libxml2: XML parsing (required for DOCX extraction and CalDAV calendar)
pkg_check_modules(LIBXML2 libxml-2.0)
if(LIBXML2_FOUND)
    include_directories(${LIBXML2_INCLUDE_DIRS})
    add_definitions(-DHAVE_LIBXML2)
    message(STATUS "libxml2: Found (${LIBXML2_VERSION})")
else()
    message(WARNING "libxml2 not found - DOCX extraction and CalDAV disabled")
    message(WARNING "Install with: sudo apt install libxml2-dev")
endif()

# libical: CalDAV calendar event parsing (RFC 5545 iCalendar)
pkg_check_modules(LIBICAL libical)
if(LIBICAL_FOUND)
    include_directories(${LIBICAL_INCLUDE_DIRS})
    add_definitions(-DHAVE_LIBICAL)
    message(STATUS "libical: Found (${LIBICAL_VERSION})")
else()
    message(WARNING "libical not found - CalDAV calendar integration disabled")
    message(WARNING "Install with: sudo apt install libical-dev")
endif()

# Tool subsystem - compile-time tool configuration
# Provides options to enable/disable individual tools
include(cmake/DawnTools.cmake)

# CUDA configuration — shared module handles auto-detection on any platform
include(cmake/DawnCUDA.cmake)
if(DAWN_CUDA_FOUND)
    set(ENABLE_WHISPER_CUDA ON CACHE BOOL "Enable CUDA for Whisper" FORCE)
    add_definitions(-DHAVE_CUDA_GPU)
else()
    set(ENABLE_WHISPER_CUDA OFF CACHE BOOL "CUDA not available" FORCE)
endif()

# Wave file handling
#find_package(PkgConfig QUIET)
#if(PKG_CONFIG_FOUND)
#    pkg_check_modules(LIBSNDFILE_PKGCONF sndfile)
#endif(PKG_CONFIG_FOUND)

# Text to Speech - check if target already exists (may be created by common/)
if(NOT TARGET piper)
    add_library(piper STATIC src/tts/piper.cpp)
    target_link_libraries(piper onnxruntime)
endif()

# Audio Backend - Runtime Selectable (both ALSA and PulseAudio supported)
# The backend can be selected at runtime via config file: [audio] backend = "auto|alsa|pulse"
# At compile time, we need both libraries for runtime selection
# In SERVER_ONLY mode, audio backends are optional (no local mic/speaker needed)

if(NOT SERVER_ONLY)
    # Find PulseAudio (optional, but recommended for desktop systems)
    find_package(PulseAudio QUIET)
    if(PULSEAUDIO_FOUND)
        include_directories(${PULSEAUDIO_INCLUDE_DIRS})
        message(STATUS "Audio backend: Runtime selectable (ALSA + PulseAudio)")
    else()
        message(STATUS "Audio backend: ALSA only (PulseAudio not found)")
    endif()
    set(HAVE_LOCAL_AUDIO ON)
else()
    # Server mode: check for audio libs but don't require them
    find_package(PulseAudio QUIET)
    if(PULSEAUDIO_FOUND)
        include_directories(${PULSEAUDIO_INCLUDE_DIRS})
    endif()
    # Check if ALSA is available (for satellite audio relay even in server mode)
    find_package(ALSA QUIET)
    if(ALSA_FOUND AND PULSEAUDIO_FOUND)
        set(HAVE_LOCAL_AUDIO ON)
        message(STATUS "Audio backend: Available (ALSA + PulseAudio found)")
    elseif(ALSA_FOUND)
        set(HAVE_LOCAL_AUDIO ON)
        message(STATUS "Audio backend: Available (ALSA only)")
    else()
        set(HAVE_LOCAL_AUDIO OFF)
        message(STATUS "Audio backend: DISABLED (server-only mode, no audio libs found)")
    endif()
endif()

if(SERVER_ONLY)
    add_definitions(-DSERVER_ONLY)
    message(STATUS "Server-only mode: ENABLED (no local audio capture/playback)")
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

# libgit2 (coding-harness code-projects: in-process git client). Built from
# source >= 1.6 (jammy apt is 1.1). See scripts/lib/libs.sh install_libgit2.
if(DAWN_ENABLE_CODE_PROJECTS)
    pkg_check_modules(LIBGIT2 REQUIRED libgit2)
    if(LIBGIT2_VERSION VERSION_LESS "1.6")
        message(FATAL_ERROR
            "libgit2 ${LIBGIT2_VERSION} too old (need >= 1.6).\n"
            "Build from source: INSTALL_LIBGIT2=true ./scripts/install.sh")
    endif()
    include_directories(${LIBGIT2_INCLUDE_DIRS})
    message(STATUS "libgit2: Found (${LIBGIT2_VERSION}) for code projects")
endif()

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
    message(STATUS "AEC: Checking WebRTC dependencies...")

    # WebRTC AEC3 backend
    set(WEBRTC_AP_ROOT ${CMAKE_SOURCE_DIR}/webrtc-audio-processing)
    set(WEBRTC_AP_BUILD ${WEBRTC_AP_ROOT}/build)

    # The webrtc-audio-processing submodule is a separate meson build that fresh
    # clones / git worktrees don't auto-populate.  If it isn't built, degrade
    # gracefully to a no-AEC build with a warning instead of failing configure —
    # this keeps unrelated / worktree / CI builds working while AEC defaults ON
    # (presets) wherever the lib exists.  NOTE: this also drops the phone-call APM
    # (echo cancel + call AGC/NS), which shares this backend.
    if(NOT EXISTS "${WEBRTC_AP_ROOT}/webrtc/modules/audio_processing/include/audio_processing.h"
       OR NOT EXISTS "${WEBRTC_AP_BUILD}/webrtc/modules/audio_processing/libwebrtc-audio-processing-1.a")
        message(WARNING
            "ENABLE_AEC=ON but webrtc-audio-processing is not built — building WITHOUT "
            "echo cancellation and the phone-call APM.\n"
            "To enable: git submodule update --init --recursive && "
            "cd webrtc-audio-processing && meson setup build && ninja -C build")
        set(AEC_ENABLED FALSE)
    else()
        set(AEC_ENABLED TRUE)

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

# Common library added here (after whisper.cpp) so dawn_common_asr can link whisper
add_subdirectory(common)

link_directories(${CMAKE_SOURCE_DIR})

# Base source files (always compiled)
set(DAWN_SOURCES
    # Core files
    src/dawn.c
    src/core/pending_system_msg.c
    src/input_queue.c
    src/mosquitto_comms.c
    src/text_to_command_nuevo.c
    src/word_to_number.c

    # Multi-threaded core (command routing always needed)
    src/core/command_router.c
    src/core/command_executor.c
    src/core/component_status.c
    src/core/device_types.c
    src/core/hash_util.c
    src/core/ocp_helpers.c
    src/core/path_utils.c
    src/core/strbuf.c
    src/core/wake_word.c
    src/core/utterance_dedup.c

    # ASR subsystem
    src/asr/asr_interface.c
    src/asr/chunking_manager.c

    # LLM subsystem
    src/llm/llm_interface.c
    src/llm/llm_silent_observe.c
    src/llm/llm_openai.c
    src/llm/llm_openai_cache.c
    src/llm/llm_openai_chat_completions.c
    src/llm/llm_openai_history.c
    src/llm/llm_openai_responses.c
    src/llm/llm_claude.c
    src/llm/llm_claude_format.c
    src/llm/llm_streaming.c
    src/llm/llm_command_parser.c
    src/llm/llm_tools.c
    src/llm/llm_tool_loop.c
    src/llm/llm_context.c
    src/llm/llm_local_provider.c
    src/llm/sse_parser.c
    src/llm/llm_rate_limit.c

    # TTS subsystem
    src/tts/text_to_speech.cpp
    common/src/tts/tts_preprocessing.cpp
    common/src/tts/number_to_words.c

    # Audio subsystem
    src/audio/audio_capture_thread.c
    src/audio/audio_decoder.c
    src/audio/flac_decoder.c
    src/audio/flac_playback.c
    src/audio/mic_passthrough.c
    src/audio/resampler.c
    src/audio/audio_backend.c
    src/audio/audio_alsa.c
    src/audio/audio_pulse.c
    src/audio/ringtone.c
    # phone_apm.cpp is compiled UNCONDITIONALLY (base sources, NOT AEC_SOURCES):
    # its WebRTC use is fenced behind #ifdef AEC_BACKEND_WEBRTC and it stubs to
    # phone_apm_create()->NULL on AEC-off builds, so the phone bridge links and
    # degrades to raw gain rather than failing at link time.
    src/audio/phone_apm.cpp
    src/audio/audio_converter.c
    src/audio/music_db.c
    src/audio/music_scanner.c
    src/audio/music_source.c
    src/audio/plex_client.c
    src/audio/plex_db.c
    src/audio/http_download.c

    # UI subsystem (TUI metrics)
    src/ui/metrics.c

    # Tools subsystem (web search, weather, calculator, etc.)
    src/tools/tool_registry.c
    src/tools/tools_init.c
    src/tools/hud_discovery.c
    src/tools/web_search.c
    src/tools/web_search_tavily.c
    src/tools/tavily_rate_limit.c
    src/tools/weather_service.c
    src/tools/calculator.c
    src/tools/calculator_bignum.c
    src/tools/tinyexpr.c
    src/tools/search_summarizer.c
    src/tools/tfidf_summarizer.c
    src/tools/url_fetcher.c
    src/tools/url_fetcher_detect.c
    src/tools/url_fetcher_fallback.c
    src/tools/url_fetch_tavily.c
    src/tools/url_snippet_cache.c
    src/tools/html_parser.c
    src/core/time_query_parser.c
    src/core/iso8601.c
    src/core/str_fuzzy.c
    src/core/scheduled_context.c
    src/tools/toml.c

    # Config subsystem
    src/config/config_defaults.c
    src/config/config_parser.c
    src/config/config_validate.c
    src/config/config_env.c
)

# Add conditionally-compiled tool sources from DawnTools.cmake
list(APPEND DAWN_SOURCES ${TOOL_SOURCES})

# Conditionally add TUI source file
if(ENABLE_TUI)
    list(APPEND DAWN_SOURCES src/ui/tui.c)
endif()

# Conditionally add WebUI source files
if(ENABLE_WEBUI)
    list(APPEND DAWN_SOURCES
        src/webui/webui_server.c
        src/webui/webui_text_processing.c
        src/webui/webui_vision_validate.c
        src/webui/webui_auth_helpers.c
        src/webui/webui_broadcasts.c
        src/webui/webui_message_dispatch.c
        src/webui/webui_send.c
        src/webui/build_focus_block.c
        src/webui/webui_http.c
        src/webui/webui_admin.c
        src/webui/webui_messaging.c
        src/webui/webui_phone.c
        src/webui/webui_history.c
        src/webui/webui_memory.c
        src/webui/webui_config.c
        src/webui/webui_session.c
        src/webui/webui_settings.c
        src/webui/webui_tools.c
        src/webui/webui_images.c
        src/webui/webui_image_rehydrate.c
        src/webui/webui_documents.c
        src/webui/webui_doc_library.c
        src/webui/webui_satellite.c
        src/webui/webui_admin_satellite.c
        src/webui/webui_ota.c
        src/webui/webui_contacts.c
        src/webui/webui_scheduler.c
        src/webui/webui_attention.c
    )
    if(DAWN_ENABLE_CALENDAR_TOOL)
        list(APPEND DAWN_SOURCES src/webui/webui_calendar.c)
    endif()
    if(DAWN_ENABLE_HOMEASSISTANT_TOOL)
        list(APPEND DAWN_SOURCES src/webui/webui_homeassistant.c)
    endif()
    if(DAWN_ENABLE_PHONE_TOOL)
        list(APPEND DAWN_SOURCES src/webui/webui_phone_config.c)
    endif()
    if(OPUS_FOUND)
        list(APPEND DAWN_SOURCES src/webui/webui_audio.c)
        list(APPEND DAWN_SOURCES src/webui/webui_always_on.c)
        list(APPEND DAWN_SOURCES src/webui/webui_music.c)
        list(APPEND DAWN_SOURCES src/webui/webui_music_handlers.c)
        list(APPEND DAWN_SOURCES src/webui/webui_music_tool.c)
        list(APPEND DAWN_SOURCES src/webui/webui_music_queue_db.c)
        list(APPEND DAWN_SOURCES src/webui/webui_music_server.c)
    endif()
endif()

# Conditionally add session manager, LLM call orchestration, prompt
# composition, worker pool, and related multi-client support sources.
if(ENABLE_WEBUI)
    list(APPEND DAWN_SOURCES
        src/core/session_manager.c
        src/core/session_manager_llm.c
        src/core/text_input_dispatch.c
        src/core/prompt_compose.c
        src/core/session_dedup.c
        src/core/worker_pool.c
        src/core/text_filter.c
        src/core/rate_limiter.c
        # Messaging channels (Telegram, SMS, Discord, Slack)
        src/messaging/messaging_engine.c
        src/messaging/messaging_engine_session.c
        src/messaging/messaging_engine_channels.c
        src/messaging/messaging_engine_read.c
        src/messaging/messaging_engine_link.c
        src/messaging/messaging_engine_inbound.c
        src/messaging/messaging_telegram.c
        src/messaging/messaging_sms.c
        src/messaging/messaging_discord.c
        src/messaging/messaging_discord_read.c
        src/messaging/messaging_slack.c
        src/messaging/messaging_split.c
        src/messaging/messaging_format.c
        src/messaging/messaging_format_telegram.c
        src/messaging/ws_reconnect.c
        src/tools/messaging_tool.c
    )
endif()

# Shared crypto store (libsodium encryption, used by calendar + OAuth)
list(APPEND DAWN_SOURCES
    src/core/crypto_store.c
)

# Scheduler (depends on auth_db for storage, always compiled when DB is available)
list(APPEND DAWN_SOURCES
    src/core/scheduler_db.c
    src/core/scheduler.c
    src/core/missed_notifications_db.c
    src/core/ota_db.c
    src/core/ota_manifest.c
    src/core/ota.c
    src/core/ota_rollout.c
)

# SAGE proactive-attention subsystem (Layer 1 core, always compiled; master
# switch is [attention] enabled, default OFF). Ingest polls the STAT/suit
# service caches, so it degrades gracefully when those tools are compiled out.
list(APPEND DAWN_SOURCES
    src/core/attention/attention_core.c
    src/core/attention/attention_catalog.c
    src/core/attention/attention_ingest.c
    src/core/attention/attention_gate.c
    src/core/attention/attention_policy.c
    src/core/attention/attention_db.c
    src/tools/attention_tool.c
)

# Document DB, chunker, extraction, and indexing pipeline (RAG document search)
list(APPEND DAWN_SOURCES
    src/tools/document_db.c
    src/tools/document_chunker.c
    src/tools/document_extract.c
    src/tools/document_index_pipeline.c
)

# External focus-source adapters — Phase 1d (document chunks, calendar
# events).  Email adapter deferred to v2 — no cache layer.  Compiled
# into the daemon binary so 1e's wiring lands as a one-line caller-side
# change; no production caller invokes external_focus_adapters_register_all()
# until 1e ships.  Until then, these symbols are dead-stripped from the
# daemon binary by the linker (no callers ⇒ DCE).  The test binary
# (tests/CMakeLists.txt::test_external_focus_adapters) pulls these in
# directly.
list(APPEND DAWN_SOURCES
    src/tools/document_focus_adapter.c
    src/tools/calendar_focus_adapter.c
    src/tools/external_focus_adapters_init.c
)

# Database core + memory subsystem (always compiled — memory works in all modes).
# embedding_engine.c lives here, not in the ENABLE_WEBUI block: the memory
# subsystem (memory_embeddings.c) depends on it in every mode (e.g. the local /
# ci presets with WEBUI off).
list(APPEND DAWN_SOURCES
    src/core/embedding_engine.c
    src/auth/auth_db_core.c
    src/auth/auth_db_schema.c
    src/auth/auth_db_backup.c
    src/auth/auth_db_migrations.c
    # v64/v65/v66 migration helpers: unconditional (schema versioning is global,
    # not gated on the coding-harness feature flags). Called by
    # auth_db_apply_migrations() in auth_db_migrations.c.
    src/auth/auth_db_migrations_v64.c
    src/auth/auth_db_migrations_v65.c
    src/auth/auth_db_migrations_v66.c
    src/auth/auth_db_migrations_v67.c
    src/auth/auth_db_migrations_v68.c
    src/auth/auth_db_migrations_v69.c
    src/auth/auth_db_migrations_v70.c
    src/auth/auth_db_migrations_v71.c
    src/auth/auth_db_attention.c
    src/auth/auth_db_statements.c
    # DB-layer tables depended on by the always-compiled memory / OTA / tool
    # subsystems (extraction, summaries, history, satellite OTA, image tools), so
    # they can't live in the ENABLE_AUTH-gated block below (breaks local / ci with
    # WEBUI off).  The admin-socket transports that USE these stay AUTH-gated.
    src/auth/auth_db_conv.c
    src/auth/auth_db_user.c
    src/auth/auth_db_settings.c
    src/auth/auth_db_satellite.c
    src/blob_store.c
    src/blob_store_util.c
    src/document_original_store.c
    src/image_store.c
    src/memory/memory_db.c
    src/memory/memory_db_facts.c
    src/memory/memory_db_summaries.c
    src/memory/memory_db_prefs.c
    src/memory/memory_db_entities.c
    src/memory/memory_db_relations.c
    src/memory/memory_db_admin.c
    src/memory/memory_db_alias.c
    src/memory/memory_db_alias_scorer.c
    src/memory/memory_db_alias_cascade.c
    src/memory/memory_db_alias_writes.c
    src/memory/memory_db_provenance.c
    src/core/focus/focus_source.c
    src/core/focus/focus_recency.c
    src/core/focus/focus_candidate_helpers.c
    src/core/focus/focus_dominant_token.c
    src/memory/memory_fact_search.c
    src/memory/memory_focus_adapters.c
    src/memory/memory_source_dedup.c
    src/memory/memory_score_floor.c
    src/memory/memory_graph_retrieval.c
    src/memory/memory_predicate_dedup.c
    src/memory/memory_bm25.c
    src/memory/memory_stem.c
    src/memory/memory_note_guard.c
    src/memory/memory_note_bridge.c
    src/memory/memory_context.c
    src/memory/memory_callback.c
    src/memory/memory_extraction.c
    src/memory/memory_similarity.c
    src/core/memory_filter.c
    src/memory/memory_maintenance.c
    src/memory/memory_recovery.c
    src/memory/memory_history_loader.c
    src/memory/memory_summarize_missing.c
    src/memory/memory_embeddings.c
    src/memory/memory_recategorize.c
    src/memory/memory_embed_tokenizer.c
    src/memory/memory_embed_onnx.c
    src/memory/memory_embed_ollama.c
    src/memory/memory_embed_openai.c
    src/memory/memory_embed_recompute.c
)

# Auth subsystem (admin socket, user management, crypto — requires WebUI)
if(ENABLE_AUTH)
    list(APPEND DAWN_SOURCES
        src/auth/admin_socket.c
        src/auth/admin_socket_memory.c
        src/auth/admin_socket_memory_entity.c
        src/auth/admin_socket_messaging.c
        src/auth/admin_socket_music.c
        src/auth/admin_socket_ota.c
        src/auth/auth_db_session.c
        src/auth/auth_db_rate_limit.c
        src/auth/auth_db_audit.c
        src/auth/auth_db_maint.c
        src/auth/auth_db_metrics.c
        src/auth/auth_crypto.c
        src/auth/auth_maintenance.c
    )
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
    # Whisper ASR comes from dawn_common_asr (linked below)
    message(STATUS "ASR Engine: Whisper (via dawn_common_asr)")
endif()

# Conditionally add MP3 decoder source file
if(DAWN_ENABLE_MP3)
    list(APPEND DAWN_SOURCES src/audio/mp3_decoder.c)
endif()

# Conditionally add Ogg Vorbis decoder source file
if(DAWN_ENABLE_OGG)
    list(APPEND DAWN_SOURCES src/audio/ogg_decoder.c)
endif()

# Conditionally add AEC source files
if(AEC_ENABLED)
    list(APPEND DAWN_SOURCES ${AEC_SOURCES})
endif()

add_executable(dawn ${DAWN_SOURCES})

# Coding-harness CI invariant: forbid process-management primitives in the MCP
# bridge / code-project sources (bridge is socket-only; git is in-process via
# libgit2). Runs on every dawn build; passes trivially until harness files
# exist. Scope is harness files only -- see docs/CODING_HARNESS_DESIGN.md.
add_custom_target(no_process_mgmt_check
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_no_process_mgmt.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking coding-harness invariant: no process-management calls")
add_dependencies(dawn no_process_mgmt_check)

# WebUI send-path CI invariant: forbid direct lws_write() of WS data frames
# outside the sanctioned response-queue drain (webui_send.c) and the separate
# music-streaming context.  A direct write interleaves with large queued frames
# and corrupts the WebSocket framing, looping the client.  See webui_send.c.
add_custom_target(no_ws_direct_write_check
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_no_ws_direct_write.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking WebUI invariant: no direct WS-frame lws_write()")
add_dependencies(dawn no_ws_direct_write_check)

# Blob-marker CI invariant: the chat-attachment "blob:<id>]" marker is encoded
# in three places (JS producer, JS parser, orphan-sweep SQL) that must agree, or
# the maintenance sweep silently reclaims still-attached original files (data
# loss).  Fails the build if the marker drifts out of any site.
add_custom_target(blob_marker_sync_check
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_blob_marker_sync.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking blob invariant: chat-attachment marker in sync")
add_dependencies(dawn blob_marker_sync_check)

# libgit2 link for the code-projects subsystem (detected above).
if(DAWN_ENABLE_CODE_PROJECTS)
    target_link_libraries(dawn ${LIBGIT2_LIBRARIES})
endif()

# AEC compile definitions (target-specific to avoid rebuilding whisper/ggml)
if(AEC_ENABLED)
    target_compile_definitions(dawn PRIVATE ENABLE_AEC)
    target_compile_definitions(dawn PRIVATE AEC_BACKEND_WEBRTC)
endif()

target_link_libraries(dawn
                      dawn_common
                      dawn_common_vad
                      dawn_common_asr
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
                      OpenSSL::Crypto
                      stemmer
                      m)

# Link uuid for Plex client identifier generation
target_link_libraries(dawn uuid)

# Link audio libraries for runtime backend selection
# Both backends are statically linked; runtime selection via vtable
if(HAVE_LOCAL_AUDIO)
    target_link_libraries(dawn asound pulse-simple pulse)
elseif(NOT SERVER_ONLY)
    message(FATAL_ERROR
        "Audio libraries (ALSA, PulseAudio) not found.\n"
        "Install with: sudo apt install libasound2-dev libpulse-dev\n"
        "Or use -DSERVER_ONLY=ON for server deployments without local audio.")
endif()

# Conditionally link libmpg123 for MP3 decoding
if(DAWN_ENABLE_MP3)
    target_link_libraries(dawn ${MPG123_LIBRARIES})
    message(STATUS "Linking MP3 library (libmpg123)")
endif()

# Conditionally link libvorbis + libvorbisfile for Ogg Vorbis decoding
if(DAWN_ENABLE_OGG)
    target_link_libraries(dawn ${VORBISFILE_LIBRARIES} ${VORBIS_LIBRARIES})
    message(STATUS "Linking Ogg libraries (libvorbisfile, libvorbis)")
endif()

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

# Conditionally link document extraction libraries (MuPDF, libzip, libxml2)
if(MUPDF_FOUND)
    # MuPDF ships only static libs on Ubuntu 22.04. All transitive deps must be linked.
    # Deps from: pkg-config --libs --static mupdf (plus harfbuzz for text shaping)
    pkg_check_modules(FREETYPE2 REQUIRED freetype2)
    pkg_check_modules(HARFBUZZ harfbuzz)
    if(HARFBUZZ_FOUND)
        set(HARFBUZZ_LIB ${HARFBUZZ_LIBRARIES})
    else()
        # Fallback: find_library without hardcoded arch paths
        find_library(HARFBUZZ_LIB NAMES harfbuzz)
    endif()
    target_link_libraries(dawn
        ${MUPDF_LIB} ${MUPDF_THIRD_LIB}
        mujs gumbo openjp2 jbig2dec jpeg
        ${HARFBUZZ_LIB} ${FREETYPE2_LIBRARIES} z)
    message(STATUS "Linking MuPDF (PDF extraction)")
    message(STATUS "  harfbuzz: ${HARFBUZZ_LIB}")
endif()
if(LIBZIP_FOUND AND LIBXML2_FOUND)
    target_link_libraries(dawn ${LIBZIP_LIBRARIES} ${LIBXML2_LIBRARIES})
    message(STATUS "Linking libzip + libxml2 (DOCX extraction)")
endif()

# Conditionally link CalDAV dependencies (libical + libxml2)
if(DAWN_ENABLE_CALENDAR_TOOL)
    target_link_libraries(dawn ${LIBICAL_LIBRARIES} ${LIBXML2_LIBRARIES})
    message(STATUS "Linking libical + libxml2 (CalDAV calendar)")
endif()

# Always link SQLite3 (music metadata database)
target_link_libraries(dawn ${SQLITE3_LIBRARIES})

# libsodium is linked unconditionally — crypto_store, the memory subsystem, and
# the calendar tool all use it regardless of the auth/WebUI subsystem.
target_link_libraries(dawn ${SODIUM_LIBRARIES})

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

# =============================================================================
# ota-keytool — offline OTA signing CLI (build on demand: `make ota-keytool`)
# =============================================================================
# Links the pure manifest core; never talks to the daemon (the signing key stays
# off the server).  EXCLUDE_FROM_ALL so it doesn't bloat the daemon build.
# See docs/OTA_DESIGN.md §Trust model.
add_executable(ota-keytool EXCLUDE_FROM_ALL
    tools/ota_keytool.c
    src/core/ota_manifest.c)
target_link_libraries(ota-keytool ${SODIUM_LIBRARIES})

# =============================================================================
# Install rules (used by: cmake --install build --prefix /usr/local)
# =============================================================================
# Binary
install(TARGETS dawn RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Shared libraries built as subprojects (whisper, ggml, ggml-base, ggml-cpu)
# whisper.cpp and ggml already define their own install() rules via add_subdirectory

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
# enable_testing() must be here (not inside tests/) so CTestTestfile.cmake is
# generated at the build root — EXCLUDE_FROM_ALL subdirs only propagate to their
# own build directory.
enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)

# =============================================================================
# Admin CLI Tool (dawn-admin)
# =============================================================================
# Only built when authentication is enabled (WebUI)
if(ENABLE_AUTH)
    add_subdirectory(dawn-admin)
endif()

