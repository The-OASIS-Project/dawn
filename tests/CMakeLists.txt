# =============================================================================
# Dawn Test Suite
# =============================================================================
# Build all tests: make tests
# Build individual test: make test_<name>
#
# Note: This file is included from the main CMakeLists.txt with EXCLUDE_FROM_ALL,
# so tests are not built by default.

set(CMAKE_C_STANDARD 11)

# Source root directory (parent of tests/)
set(SRC_ROOT ${CMAKE_SOURCE_DIR})

# Include directories (inherited from parent, but be explicit for clarity)
include_directories(${SRC_ROOT}/include)

# =============================================================================
# Test Executables
# =============================================================================

# Test: SSE Parser
add_executable(test_sse_parser
               test_sse_parser.c
               ${SRC_ROOT}/src/llm/sse_parser.c
)

target_link_libraries(test_sse_parser
                      dawn_common
                      ${JSONC_LIBRARIES})

# Test: Sentence Buffer
add_executable(test_sentence_buffer
               test_sentence_buffer.c
               ${SRC_ROOT}/src/llm/sentence_buffer.c
)

target_link_libraries(test_sentence_buffer
                      dawn_common
                      ${JSONC_LIBRARIES})

# Test: End-to-End Streaming
add_executable(test_streaming
               test_streaming.c
               ${SRC_ROOT}/src/llm/llm_interface.c
               ${SRC_ROOT}/src/llm/llm_openai.c
               ${SRC_ROOT}/src/llm/llm_claude.c
               ${SRC_ROOT}/src/llm/llm_streaming.c
               ${SRC_ROOT}/src/llm/sentence_buffer.c
               ${SRC_ROOT}/src/llm/sse_parser.c
               ${SRC_ROOT}/src/config/config_parser.c
               ${SRC_ROOT}/src/config/config_defaults.c
               ${SRC_ROOT}/src/config/config_env.c
               ${SRC_ROOT}/src/tools/toml.c
               ${SRC_ROOT}/src/ui/metrics.c
)

target_link_libraries(test_streaming
                      dawn_common
                      ${CURL_LIBRARIES}
                      ${JSONC_LIBRARIES}
                      OpenSSL::SSL
                      OpenSSL::Crypto)

# Test: ASR Benchmark (requires both Vosk and Whisper)
add_executable(asr_benchmark
               asr_benchmark.c
               ${SRC_ROOT}/src/asr/asr_interface.c
               ${SRC_ROOT}/src/asr/asr_vosk.c
               ${SRC_ROOT}/src/asr/asr_whisper.c
)

target_include_directories(asr_benchmark PRIVATE
                           ${SRC_ROOT}/whisper.cpp/include)

target_link_libraries(asr_benchmark
                      dawn_common
                      vosk
                      whisper
                      sndfile
                      ${JSONC_LIBRARIES}
                      ${CUDA_LIBRARIES}
                      ${CUDART_LIBRARIES}
                      ${CUSPARSE_LIBRARIES}
                      ${CUBLAS_LIBRARIES}
                      ${CUSOLVER_LIBRARIES}
                      ${CURAND_LIBRARIES})

# Test: VAD Model Comparison
add_executable(test_vad_models test_vad_models.c)
target_include_directories(test_vad_models PRIVATE /usr/local/include/onnxruntime)
target_link_libraries(test_vad_models onnxruntime m)

# Test: VAD API Unit Test
add_executable(test_vad_api
               test_vad_api.c
               ${SRC_ROOT}/src/asr/vad_silero.c
)

target_include_directories(test_vad_api PRIVATE /usr/local/include/onnxruntime)

target_link_libraries(test_vad_api
                      dawn_common
                      onnxruntime
                      m
                      ${JSONC_LIBRARIES})

# Test: ASR Concurrent Context
# Verifies multi-context support for multi-client architecture
add_executable(test_asr_concurrent
               test_asr_concurrent.c
               ${SRC_ROOT}/src/asr/asr_interface.c
               ${SRC_ROOT}/src/asr/asr_whisper.c
               ${SRC_ROOT}/src/ui/metrics.c
)

target_include_directories(test_asr_concurrent PRIVATE
                           ${SRC_ROOT}/whisper.cpp/include)

target_link_libraries(test_asr_concurrent
                      dawn_common
                      whisper
                      pthread
                      m
                      ${JSONC_LIBRARIES}
                      ${CUDA_LIBRARIES}
                      ${CUDART_LIBRARIES}
                      ${CUSPARSE_LIBRARIES}
                      ${CUBLAS_LIBRARIES}
                      ${CUSOLVER_LIBRARIES}
                      ${CURAND_LIBRARIES})

# Test: Per-Session LLM Commands
# Tests thread-local storage mechanism for command context (session pointer passing)
# This is a self-contained test that validates TLS without needing full infrastructure
add_executable(test_session_commands
               test_session_commands.c)

target_link_libraries(test_session_commands
                      pthread)

# Test: TF-IDF Summarizer Quality
# Fetches articles from diverse outlets and analyzes summarizer output quality
add_executable(test_summarizer_quality
               test_summarizer_quality.c
               ${SRC_ROOT}/src/tools/tfidf_summarizer.c
               ${SRC_ROOT}/src/tools/url_fetcher.c
               ${SRC_ROOT}/src/tools/html_parser.c
               ${SRC_ROOT}/src/tools/string_utils.c
               ${SRC_ROOT}/src/config/config_parser.c
               ${SRC_ROOT}/src/config/config_defaults.c
               ${SRC_ROOT}/src/config/config_env.c
               ${SRC_ROOT}/src/tools/toml.c
)

target_link_libraries(test_summarizer_quality
                      dawn_common
                      ${CURL_LIBRARIES}
                      ${JSONC_LIBRARIES}
                      m)

# =============================================================================
# Aggregate "tests" target
# =============================================================================
# This creates a target that builds all tests when you run "make tests"
add_custom_target(tests
                  DEPENDS test_sse_parser
                          test_sentence_buffer
                          test_streaming
                          asr_benchmark
                          test_vad_models
                          test_vad_api
                          test_asr_concurrent
                          test_session_commands
                          test_summarizer_quality
                  COMMENT "Building all tests")
