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

cmake_minimum_required(VERSION 3.10)

project(dawn_common LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Source files
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

# Install targets (for potential standalone use)
install(TARGETS dawn_common
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
    DESTINATION include/dawn_common
    FILES_MATCHING PATTERN "*.h"
)
