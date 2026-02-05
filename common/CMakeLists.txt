# DAWN Common Library
#
# Shared code between the DAWN daemon and satellite applications.
# This library provides platform-independent utilities that can be used
# by both the main daemon and remote satellites (e.g., Raspberry Pi).
#
# Components:
#   - logging_common: Callback-based logging abstraction
#   - ring_buffer: Thread-safe circular buffer for audio
#   - string_utils: String manipulation utilities
#   - sentence_buffer: Sentence extraction for TTS streaming
#
# Optional Components (require ONNX Runtime):
#   - vad_silero: Silero VAD for voice activity detection

cmake_minimum_required(VERSION 3.10)

project(dawn_common LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options for optional components
option(DAWN_COMMON_WITH_VAD "Build with Silero VAD support (requires ONNX Runtime)" ON)
option(DAWN_COMMON_WITH_ASR "Build with Whisper ASR support (requires whisper.cpp)" ON)
option(DAWN_COMMON_WITH_TTS "Build with Piper TTS support (requires ONNX Runtime)" ON)

# Sentence buffer max size (bytes)
# Daemon (server): 10MB (10485760) - plenty of memory
# Satellite (Pi Zero 2 W): 256KB (262144) - prevent OOM on constrained devices
if(NOT DEFINED DAWN_SENTENCE_BUFFER_MAX_SIZE)
    set(DAWN_SENTENCE_BUFFER_MAX_SIZE 10485760 CACHE STRING "Maximum sentence buffer size in bytes")
endif()

# Core source files (no external dependencies beyond pthreads)
set(DAWN_COMMON_SOURCES
    src/logging_common.c
    src/audio/ring_buffer.c
    src/utils/string_utils.c
    src/utils/sentence_buffer.c
)

# Create static library
add_library(dawn_common STATIC ${DAWN_COMMON_SOURCES})

# Include directories
target_include_directories(dawn_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Required libraries
find_package(Threads REQUIRED)
target_link_libraries(dawn_common PUBLIC Threads::Threads)

# Compiler flags
target_compile_options(dawn_common PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -Werror=implicit-function-declaration
)

# Sentence buffer size limit
target_compile_definitions(dawn_common PRIVATE
    SENTENCE_BUFFER_MAX_SIZE=${DAWN_SENTENCE_BUFFER_MAX_SIZE}
)
message(STATUS "Sentence buffer max size: ${DAWN_SENTENCE_BUFFER_MAX_SIZE} bytes")

# --------------------------------------------------------------------------
# Optional: Silero VAD (requires ONNX Runtime)
# --------------------------------------------------------------------------
if(DAWN_COMMON_WITH_VAD)
    # Find ONNX Runtime
    find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime
        HINTS /usr/local/lib /usr/lib /opt/onnxruntime/lib
    )
    find_path(ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_c_api.h
        HINTS /usr/local/include /usr/include /opt/onnxruntime/include
              /usr/local/include/onnxruntime
    )

    if(ONNXRUNTIME_LIBRARY AND ONNXRUNTIME_INCLUDE_DIR)
        message(STATUS "Found ONNX Runtime: ${ONNXRUNTIME_LIBRARY}")
        message(STATUS "ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")

        # Create VAD library
        add_library(dawn_common_vad STATIC src/asr/vad_silero.c)
        target_include_directories(dawn_common_vad PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${ONNXRUNTIME_INCLUDE_DIR}
        )
        target_link_libraries(dawn_common_vad PUBLIC
            dawn_common
            ${ONNXRUNTIME_LIBRARY}
        )
        target_compile_definitions(dawn_common_vad PUBLIC DAWN_HAS_VAD)
    else()
        message(WARNING "ONNX Runtime not found - VAD support disabled")
        set(DAWN_COMMON_WITH_VAD OFF)
    endif()
endif()

# --------------------------------------------------------------------------
# Optional: Whisper ASR (requires whisper.cpp)
# --------------------------------------------------------------------------
if(DAWN_COMMON_WITH_ASR)
    # Check if whisper target already exists (from parent CMakeLists adding whisper.cpp)
    if(TARGET whisper)
        message(STATUS "Using whisper target from parent project")
        set(WHISPER_FOUND TRUE)
        set(WHISPER_TARGET whisper)
    else()
        # Try to find pre-installed whisper.cpp
        find_library(WHISPER_LIBRARY NAMES whisper
            HINTS /usr/local/lib /usr/lib
            PATH_SUFFIXES whisper
        )
        find_path(WHISPER_INCLUDE_DIR NAMES whisper.h
            HINTS /usr/local/include /usr/include
            PATH_SUFFIXES whisper
        )
        if(WHISPER_LIBRARY AND WHISPER_INCLUDE_DIR)
            message(STATUS "Found whisper.cpp: ${WHISPER_LIBRARY}")
            message(STATUS "Whisper include: ${WHISPER_INCLUDE_DIR}")
            set(WHISPER_FOUND TRUE)
        else()
            message(WARNING "whisper.cpp not found - ASR support disabled")
            set(WHISPER_FOUND FALSE)
            set(DAWN_COMMON_WITH_ASR OFF)
        endif()
    endif()

    if(WHISPER_FOUND)
        # Create ASR library
        add_library(dawn_common_asr STATIC src/asr/asr_whisper.c)
        target_include_directories(dawn_common_asr PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        target_link_libraries(dawn_common_asr PUBLIC dawn_common)

        if(TARGET whisper)
            # Use whisper target from parent
            target_link_libraries(dawn_common_asr PUBLIC whisper)
        else()
            # Use found library
            target_include_directories(dawn_common_asr PUBLIC ${WHISPER_INCLUDE_DIR})
            target_link_libraries(dawn_common_asr PUBLIC ${WHISPER_LIBRARY})
        endif()

        target_compile_definitions(dawn_common_asr PUBLIC DAWN_HAS_ASR)
    endif()
endif()

# --------------------------------------------------------------------------
# Optional: Piper TTS (requires ONNX Runtime + piper-phonemize)
# --------------------------------------------------------------------------
if(DAWN_COMMON_WITH_TTS)
    # Reuse ONNX Runtime from VAD or find it
    if(NOT ONNXRUNTIME_LIBRARY)
        find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime
            HINTS /usr/local/lib /usr/lib /opt/onnxruntime/lib
        )
        find_path(ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_c_api.h
            HINTS /usr/local/include /usr/include /opt/onnxruntime/include
                  /usr/local/include/onnxruntime
        )
    endif()

    # Find piper-phonemize library
    find_library(PIPER_PHONEMIZE_LIBRARY NAMES piper_phonemize
        HINTS /usr/local/lib /usr/lib
    )
    find_path(PIPER_PHONEMIZE_INCLUDE_DIR NAMES piper-phonemize/phonemize.hpp
        HINTS /usr/local/include /usr/include
    )

    # Find espeak-ng library
    find_library(ESPEAK_NG_LIBRARY NAMES espeak-ng
        HINTS /usr/local/lib /usr/lib
    )

    # Find spdlog (required by piper.cpp)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SPDLOG spdlog)
    endif()

    # Check if piper source exists in parent project
    set(PIPER_HEADER_DIR "")
    set(PIPER_SOURCE_FILE "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/include/tts/piper.hpp")
        set(PIPER_HEADER_DIR "${CMAKE_SOURCE_DIR}/include")
        set(PIPER_SOURCE_FILE "${CMAKE_SOURCE_DIR}/src/tts/piper.cpp")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../include/tts/piper.hpp")
        set(PIPER_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")
        set(PIPER_SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../src/tts/piper.cpp")
    endif()

    if(ONNXRUNTIME_LIBRARY AND PIPER_PHONEMIZE_LIBRARY AND PIPER_HEADER_DIR AND EXISTS "${PIPER_SOURCE_FILE}")
        message(STATUS "Found ONNX Runtime for TTS: ${ONNXRUNTIME_LIBRARY}")
        message(STATUS "Found piper-phonemize: ${PIPER_PHONEMIZE_LIBRARY}")
        message(STATUS "Found Piper source: ${PIPER_SOURCE_FILE}")

        # Check if piper target already exists (created by parent project)
        if(NOT TARGET piper)
            # Create piper library (same pattern as main build)
            add_library(piper STATIC ${PIPER_SOURCE_FILE})
            target_include_directories(piper PUBLIC
                ${PIPER_HEADER_DIR}
                ${ONNXRUNTIME_INCLUDE_DIR}
                ${PIPER_PHONEMIZE_INCLUDE_DIR}
                ${SPDLOG_INCLUDE_DIRS}
            )
            target_link_libraries(piper PUBLIC ${ONNXRUNTIME_LIBRARY})
        else()
            message(STATUS "Using piper target from parent project")
        endif()

        # Create TTS wrapper library (C interface + preprocessing)
        add_library(dawn_common_tts STATIC
            src/tts/tts_piper.cpp
            src/tts/tts_preprocessing.cpp
        )
        target_include_directories(dawn_common_tts PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${PIPER_HEADER_DIR}
            ${ONNXRUNTIME_INCLUDE_DIR}
            ${PIPER_PHONEMIZE_INCLUDE_DIR}
            ${SPDLOG_INCLUDE_DIRS}
        )
        target_link_libraries(dawn_common_tts PUBLIC
            dawn_common
            piper
            ${PIPER_PHONEMIZE_LIBRARY}
            ${SPDLOG_LIBRARIES}
        )
        if(ESPEAK_NG_LIBRARY)
            target_link_libraries(dawn_common_tts PUBLIC ${ESPEAK_NG_LIBRARY})
        endif()
        target_compile_definitions(dawn_common_tts PUBLIC DAWN_HAS_TTS)
    else()
        if(NOT ONNXRUNTIME_LIBRARY)
            message(WARNING "ONNX Runtime not found - TTS support disabled")
        endif()
        if(NOT PIPER_PHONEMIZE_LIBRARY)
            message(WARNING "piper-phonemize not found - TTS support disabled")
        endif()
        if(NOT PIPER_HEADER_DIR)
            message(WARNING "Piper headers not found - TTS support disabled")
        endif()
        if(NOT EXISTS "${PIPER_SOURCE_FILE}")
            message(WARNING "Piper source (piper.cpp) not found - TTS support disabled")
        endif()
        set(DAWN_COMMON_WITH_TTS OFF)
    endif()
endif()

# Note: No install targets - this is a subproject built as part of dawn or dawn_satellite
