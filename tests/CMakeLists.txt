# =============================================================================
# Dawn Test Suite
# =============================================================================
# Build all tests:        make tests-all     (or `make tests` — alias kept for
#                                              backwards compatibility)
# Build CI-safe subset:   make tests-ci
# Build a single test:    make test_<name>
#
# Note: This file is included from the main CMakeLists.txt with EXCLUDE_FROM_ALL,
# so tests are not built by default. `enable_testing()` lives in the root
# CMakeLists.txt (BEFORE add_subdirectory) so that ctest discovers tests
# correctly even though the subdirectory is excluded.

set(CMAKE_C_STANDARD 11)

# Unity test framework (vendored, matches STAT/ECHO repos)
# NOTE: DAWN uses EXCLUDE_FROM_ALL for tests because the Jetson target has
# limited resources. This means ctest is a test-runner only — `make tests-ci`
# must be run first to build the binaries.
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

# Source root directory (parent of tests/)
set(SRC_ROOT ${CMAKE_SOURCE_DIR})

# Include directories (inherited from parent, but be explicit for clarity)
include_directories(${SRC_ROOT}/include)

# Aggregate targets — populated by dawn_add_unit_test() below.
add_custom_target(tests-ci  COMMENT "Building CI-safe unit tests")
add_custom_target(tests-all COMMENT "Building all tests")
# Benchmark drivers (NO_TEST).  Not part of tests-ci because GitHub CI lacks
# onnxruntime; the pre-commit hook builds this on the dev box so the benches
# cannot silently stop compiling or linking.
add_custom_target(benches COMMENT "Building benchmark drivers")
# Backwards-compatibility alias for `make tests`.
add_custom_target(tests DEPENDS tests-all)

# =============================================================================
# Helper: dawn_add_unit_test
# =============================================================================
# Defines a test executable, registers it with ctest, applies a CTest label
# (ci or hardware), and adds it to the appropriate aggregate target(s).
#
# Single source of truth per test — replaces three previously separate manual
# lists (add_executable / set_tests_properties / aggregate DEPENDS), so a
# forgotten entry can no longer cause CI to silently skip a test.
#
# Usage:
#   dawn_add_unit_test(test_name
#       SOURCES   <source files...>
#       LIBS      <libraries...>           # passed to target_link_libraries
#       [INCLUDES <dirs...>]               # target_include_directories
#       [DEFINES  <defs...>]               # target_compile_definitions
#       [LABEL    ci|hardware]             # CTest label + aggregate membership
#       [NO_TEST])                         # Skip add_test (for benchmarks)
#
# - Tests with `LABEL ci` are added to both `tests-ci` and `tests-all`.
# - Tests with `LABEL hardware` are added to `tests-all` only.
# - Tests with no label are added to `tests-all` (and registered with ctest
#   without a label).
# - NO_TEST skips ctest registration entirely (e.g., bench_retrieval, which is
#   driven by an external Python harness) and joins the `benches` aggregate.
function(dawn_add_unit_test NAME)
   cmake_parse_arguments(T "NO_TEST" "LABEL" "SOURCES;LIBS;INCLUDES;DEFINES" ${ARGN})

   add_executable(${NAME} ${T_SOURCES})
   if(T_LIBS)
      target_link_libraries(${NAME} PRIVATE ${T_LIBS})
   endif()
   # The auth-DB chain hashes stored prompts (hash_util.c, OpenSSL SHA-256), so
   # every test built from it links libcrypto without listing it.
   if("${SRC_ROOT}/src/core/hash_util.c" IN_LIST T_SOURCES)
      target_link_libraries(${NAME} PRIVATE OpenSSL::Crypto)
   endif()
   if(T_INCLUDES)
      target_include_directories(${NAME} PRIVATE ${T_INCLUDES})
   endif()
   if(T_DEFINES)
      target_compile_definitions(${NAME} PRIVATE ${T_DEFINES})
   endif()

   if(NOT T_NO_TEST)
      add_test(NAME ${NAME} COMMAND ${NAME})
      if(T_LABEL STREQUAL "ci")
         # CI runners have no onnxruntime: a ci test that links it builds on a
         # dev box with the headers installed and fails only on GitHub.
         if("onnxruntime" IN_LIST T_LIBS OR "${SRC_ROOT}/src/memory/memory_embed_onnx.c" IN_LIST
                                            T_SOURCES)
            message(FATAL_ERROR "${NAME}: a ci test can't use onnxruntime (stub the provider)")
         endif()
         set_tests_properties(${NAME} PROPERTIES LABELS "ci")
         add_dependencies(tests-ci ${NAME})
      elseif(T_LABEL STREQUAL "hardware")
         set_tests_properties(${NAME} PROPERTIES LABELS "hardware")
      endif()
   else()
      add_dependencies(benches ${NAME})
   endif()
   add_dependencies(tests-all ${NAME})
endfunction()

# =============================================================================
# Shared source / library groups
# =============================================================================
# Auth-DB chain — three DB-backed tests (calendar, auth, email) link the full
# prepared-statement layer rather than stubbing it.
set(AUTH_DB_SOURCES
    ${SRC_ROOT}/src/auth/auth_db_core.c
    ${SRC_ROOT}/src/auth/auth_db_ids.c
    ${SRC_ROOT}/src/auth/auth_db_schema.c
    ${SRC_ROOT}/src/auth/auth_db_backup.c
    ${SRC_ROOT}/src/auth/auth_db_migrations.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v64.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v65.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v66.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v67.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v68.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v69.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v70.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v71.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v72.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v73.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v74.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v75.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v92.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v93.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v94.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v95.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v96.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v97.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v98.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v99.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v100.c
    ${SRC_ROOT}/src/auth/auth_db_migrations_v101.c
    ${SRC_ROOT}/src/auth/auth_db_messages.c
    ${SRC_ROOT}/src/auth/auth_db_conv_prefix.c
    ${SRC_ROOT}/src/auth/auth_db_focus_handles.c
    ${SRC_ROOT}/src/auth/auth_db_tool_results.c
    ${SRC_ROOT}/src/auth/auth_db_storage.c
    ${SRC_ROOT}/src/auth/auth_db_withdraw.c
    ${SRC_ROOT}/src/llm/llm_context_text.c
    ${SRC_ROOT}/src/core/hash_util.c
    ${SRC_ROOT}/src/auth/auth_db_statements.c
    ${SRC_ROOT}/src/auth/auth_db_user.c
    ${SRC_ROOT}/src/auth/auth_db_session.c
    ${SRC_ROOT}/src/auth/auth_db_conv.c
    ${SRC_ROOT}/src/auth/auth_db_jobs.c
    ${SRC_ROOT}/src/auth/auth_db_research.c
    ${SRC_ROOT}/src/auth/auth_db_events.c
    ${SRC_ROOT}/src/auth/auth_db_settings.c
    ${SRC_ROOT}/src/auth/auth_db_satellite.c
    ${SRC_ROOT}/src/auth/auth_db_metrics.c
    ${SRC_ROOT}/src/auth/auth_db_maint.c
    ${SRC_ROOT}/src/auth/auth_db_audit.c
    ${SRC_ROOT}/src/auth/auth_db_rate_limit.c
    # v48: auth_db_core.c calls memory_stem_init() during the v48
    # FTS5 backfill migration; targets linking the full auth chain need
    # the stemmer wrapper too.  memory_bm25.c is included for symmetry —
    # any future call from auth_db_core into the BM25 helpers links
    # cleanly without revisiting this list.
    ${SRC_ROOT}/src/memory/memory_stem.c
    ${SRC_ROOT}/src/memory/memory_bm25.c
    ${SRC_ROOT}/src/core/path_utils.c)

# CUDA libraries — required by Whisper-backed hardware tests.
set(CUDA_LIBS
    ${CUDA_LIBRARIES}
    ${CUDART_LIBRARIES}
    ${CUSPARSE_LIBRARIES}
    ${CUBLAS_LIBRARIES}
    ${CUSOLVER_LIBRARIES}
    ${CURAND_LIBRARIES})

# =============================================================================
# CI-safe tests (no ONNX, CUDA, hardware, or external API dependencies)
# =============================================================================

dawn_add_unit_test(test_sse_parser
   SOURCES test_sse_parser.c
           ${SRC_ROOT}/src/llm/sse_parser.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Schwab price-history analytics — pure math, no network/DB.
dawn_add_unit_test(test_schwab_stats
   SOURCES test_schwab_stats.c
           ${SRC_ROOT}/src/tools/schwab_stats.c
   LIBS    unity m
   LABEL   ci)

# Schwab transaction classifier — pure (json-c parse only, no network/DB),
# exercised against a scrubbed fixture pinned from a live capture.
dawn_add_unit_test(test_schwab_txn
   SOURCES test_schwab_txn.c
           ${SRC_ROOT}/src/tools/schwab_txn.c
   LIBS    unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Schwab portfolio snapshot parser + JSON serializer — pure (json-c only),
# exercised against a synthetic /accounts fixture (multi-account, options, short).
dawn_add_unit_test(test_schwab_portfolio
   SOURCES test_schwab_portfolio.c
           ${SRC_ROOT}/src/tools/schwab_portfolio.c
           ${SRC_ROOT}/src/tools/schwab_quotes.c
   LIBS    unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Schwab /quotes reference-name parser — synthetic /quotes fixture (reference
# blocks, a missing-reference symbol, the errors object).
dawn_add_unit_test(test_schwab_quotes
   SOURCES test_schwab_quotes.c
           ${SRC_ROOT}/src/tools/schwab_quotes.c
           ${SRC_ROOT}/src/tools/schwab_portfolio.c
   LIBS    unity ${JSONC_LIBRARIES}
   LABEL   ci)

# MCP HTTP+SSE transport — exercised against an in-test loopback HTTP+SSE
# server (no external network), so it is CI-safe.
dawn_add_unit_test(test_mcp_transport
   SOURCES test_mcp_transport.c
           ${SRC_ROOT}/src/tools/mcp_transport_http_sse.c
           ${SRC_ROOT}/src/llm/sse_parser.c
   LIBS    dawn_common unity ${CURL_LIBRARIES} pthread
   LABEL   ci)

# MCP JSON-RPC client — exercised against an in-test auto-responding mock MCP
# server (HTTP+SSE, no external network), so it is CI-safe.
dawn_add_unit_test(test_mcp_client
   SOURCES test_mcp_client.c
           ${SRC_ROOT}/src/tools/mcp_client.c
           ${SRC_ROOT}/src/tools/mcp_result.c
           ${SRC_ROOT}/src/tools/mcp_transport_http_sse.c
           ${SRC_ROOT}/src/llm/sse_parser.c
   LIBS    dawn_common unity ${CURL_LIBRARIES} ${JSONC_LIBRARIES} pthread
   LABEL   ci)

# A view of a tool result too big to send whole (pure).
dawn_add_unit_test(test_llm_tool_view
   SOURCES test_llm_tool_view.c
           ${SRC_ROOT}/src/llm/llm_tool_view.c
           ${SRC_ROOT}/src/llm/llm_tool_view_path.c
   LIBS    unity ${JSONC_LIBRARIES}
   LABEL   ci)

# What result_read does with a stored result (pure).
dawn_add_unit_test(test_result_read_ops
   SOURCES test_result_read_ops.c
           ${SRC_ROOT}/src/tools/result_read_ops.c
           ${SRC_ROOT}/src/llm/llm_tool_view.c
           ${SRC_ROOT}/src/llm/llm_tool_view_path.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# An MCP tools/call result as the model reads it (pure, no network).
dawn_add_unit_test(test_mcp_result
   SOURCES test_mcp_result.c
           ${SRC_ROOT}/src/tools/mcp_result.c
   LIBS    unity ${JSONC_LIBRARIES}
   LABEL   ci)

# MCP JSON Schema -> treg_param_t translator (pure functions, no network).
dawn_add_unit_test(test_mcp_bridge_schema
   SOURCES test_mcp_bridge_schema.c
           ${SRC_ROOT}/src/tools/mcp_bridge_schema.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Schema v64 mcp_user_access migration + per-user grant/revoke/check API.
dawn_add_unit_test(test_auth_db_mcp
   SOURCES test_auth_db_mcp.c
           ${AUTH_DB_SOURCES}
           ${SRC_ROOT}/src/auth/auth_db_mcp.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# Schema v56 code_projects migration + code_project_db CRUD/visibility/CASCADE.
dawn_add_unit_test(test_code_project_db
   SOURCES test_code_project_db.c
           ${AUTH_DB_SOURCES}
           ${SRC_ROOT}/src/tools/code_project_db.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# libgit2 clone wrapper — only when code projects (and thus libgit2) are enabled.
if(DAWN_ENABLE_CODE_PROJECTS)
   dawn_add_unit_test(test_code_project_git
      SOURCES test_code_project_git.c
              ${SRC_ROOT}/src/tools/code_project_git.c
      LIBS     dawn_common unity ${LIBGIT2_LIBRARIES} pthread
      INCLUDES ${LIBGIT2_INCLUDE_DIRS}
      LABEL    ci)
endif()

# MCP bridge: registration + dispatch + auth + dangerous-tool denylist.
# Reuses test_tool_registry_stub.c for tool_registry.c's config/toml deps and an
# in-test mock MCP server; no external network.
dawn_add_unit_test(test_mcp_bridge
   SOURCES test_mcp_bridge.c
           test_tool_registry_stub.c
           ${SRC_ROOT}/src/tools/tool_registry.c
           ${SRC_ROOT}/src/tools/mcp_bridge_tool.c
           ${SRC_ROOT}/src/tools/mcp_bridge_schema.c
           ${SRC_ROOT}/src/tools/mcp_client.c
           ${SRC_ROOT}/src/tools/mcp_result.c
           ${SRC_ROOT}/src/tools/mcp_transport_http_sse.c
           ${SRC_ROOT}/src/llm/sse_parser.c
           ${SRC_ROOT}/src/auth/auth_db_mcp.c
           # code_project_db + code_project_namemap: the bridge's cbm path
           # (#ifdef DAWN_ENABLE_CODE_PROJECTS, active via the global define)
           # references code_project_db_check_visible and the namemap translation.
           ${SRC_ROOT}/src/tools/code_project_db.c
           ${SRC_ROOT}/src/tools/code_project_namemap.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
           ${CURL_LIBRARIES} ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_sentence_buffer
   SOURCES test_sentence_buffer.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_session_commands
   SOURCES test_session_commands.c
   LIBS    unity pthread
   LABEL   ci)

# Blob store pure helpers (id gen, validation, mime->ext) — no DB, CI-safe.
dawn_add_unit_test(test_blob_store
   SOURCES test_blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
   LIBS    unity
   LABEL   ci)

# Image store as a thin wrapper over the generic blob engine — full delegation
# against real SQLite + temp file dir (Phase 2 live-path regression gate).
dawn_add_unit_test(test_image_store
   SOURCES test_image_store.c
           ${AUTH_DB_SOURCES}
           ${SRC_ROOT}/src/image_store.c
           ${SRC_ROOT}/src/blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# An image question's history message: what a reload rebuilds, or refused
# (missing / another user's / inline data: / too many), never degraded.
dawn_add_unit_test(test_image_question
   SOURCES test_image_question.c
           ${AUTH_DB_SOURCES}
           ${SRC_ROOT}/src/core/image_rehydrate.c
           ${SRC_ROOT}/src/core/ocp_helpers.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/image_store.c
           ${SRC_ROOT}/src/blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# core_text_input_dispatch's gate on a turn's words: an image-only turn runs,
# an empty one doesn't (everything it calls stubbed in the test).
dawn_add_unit_test(test_text_input_dispatch
   SOURCES test_text_input_dispatch.c
           ${SRC_ROOT}/src/core/text_input_dispatch.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES}
   LABEL   ci)

# A turn's attached documents built into its text (bodies defused, boundaries kept).
dawn_add_unit_test(test_webui_attachments
   SOURCES test_webui_attachments.c
           ${SRC_ROOT}/src/webui/webui_attachments.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/blob_store_util.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# A text turn's client_ref: validation and the per-thread turn ref.
dawn_add_unit_test(test_webui_turn_ref
   SOURCES test_webui_turn_ref.c
           ${SRC_ROOT}/src/webui/webui_turn_ref.c
   LIBS    unity pthread
   LABEL   ci)

# Document-original store + blob engine dedup/orphan/caps + v68 stmt_blob_*.
dawn_add_unit_test(test_document_original_store
   SOURCES test_document_original_store.c
           ${AUTH_DB_SOURCES}
           ${SRC_ROOT}/src/document_original_store.c
           ${SRC_ROOT}/src/blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# v68 migration applied to an EXISTING pre-v68 DB (base-schema-before-migration
# ordering regression — the field failure "no such column: original_blob_id").
dawn_add_unit_test(test_blob_migration
   SOURCES test_blob_migration.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# v69: conversations.is_pinned upgrade (seeds a real v68 DB → verifies the
# base-schema-vs-migration ordering — the field failure "no such column:
# is_pinned").
dawn_add_unit_test(test_v69_migration
   SOURCES test_v69_migration.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# v70: code_projects.name NOCASE unique index upgrade (seeds a real v69 DB with a
# code_projects row → verifies the index exists and enforces case-uniqueness).
dawn_add_unit_test(test_v70_migration
   SOURCES test_v70_migration.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_ota_manifest
   SOURCES test_ota_manifest.c
           ${SRC_ROOT}/src/core/ota_manifest.c
   LIBS    unity ${SODIUM_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_ota
   SOURCES test_ota.c
           ${SRC_ROOT}/src/core/ota.c
           ${SRC_ROOT}/src/core/ota_db.c
           ${SRC_ROOT}/src/core/ota_manifest.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${SODIUM_LIBRARIES} ${JSONC_LIBRARIES}
   LABEL   ci)

# Fleet rollout orchestrator (canary-then-fan-out state machine + slot-claim
# invariants).  Externals (satellite_db_list / ota_db_get / sessions / transport)
# are stubbed IN the test TU (not a sibling *_stub.c) — deliberate: ota_rollout.c
# is the sole consumer here, so there's no stub to share, and a future new external
# dep should surface as a link error in this TU.
dawn_add_unit_test(test_ota_rollout
   SOURCES test_ota_rollout.c
           ${SRC_ROOT}/src/core/ota_rollout.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

# Satellite-side OTA host tests.  These compile dawn_satellite/ sources directly
# (libc / libsodium / json-c only — no SDL/curl/ws) and run natively, so the
# device-apply decision logic regression-tests in CI without a real RPi.
#
# Guarded on the satellite tree being present: the Docker server image excludes
# dawn_satellite/ (.dockerignore) while add_subdirectory(tests) is unconditional,
# so without this guard `cmake --preset server` fails its configure step on the
# missing sources.  The daemon-side OTA tests above never reference dawn_satellite/
# and stay enabled everywhere.
if(EXISTS ${SRC_ROOT}/dawn_satellite/src/ota_apply_decide.c)
   dawn_add_unit_test(test_ota_marker
      SOURCES test_ota_marker.c
              ${SRC_ROOT}/dawn_satellite/src/ota_marker.c
      INCLUDES ${SRC_ROOT}/dawn_satellite/include
      LIBS    unity
      LABEL   ci)

   dawn_add_unit_test(test_ota_launch
      SOURCES test_ota_launch.c
              ${SRC_ROOT}/dawn_satellite/src/ota_launch_core.c
              ${SRC_ROOT}/dawn_satellite/src/ota_marker.c
      INCLUDES ${SRC_ROOT}/dawn_satellite/include
      DEFINES OTA_DATA_DIR="/tmp/dawn_test_ota_launch"
      LIBS    unity
      LABEL   ci)

   dawn_add_unit_test(test_ota_apply
      SOURCES test_ota_apply.c
              ${SRC_ROOT}/dawn_satellite/src/ota_apply_decide.c
              ${SRC_ROOT}/src/core/ota_manifest.c
      INCLUDES ${SRC_ROOT}/dawn_satellite/include
      DEFINES OTA_KEYRING_PATH="/tmp/dawn_test_ota_keyring"
      LIBS    unity ${SODIUM_LIBRARIES} ${JSONC_LIBRARIES}
      LABEL   ci)
else()
   message(STATUS "DAWN tests: dawn_satellite/ absent — skipping satellite-side OTA host tests")
endif()

dawn_add_unit_test(test_scheduler
   SOURCES test_scheduler.c test_scheduler_stub.c
           ${SRC_ROOT}/src/core/scheduler_db.c
   LIBS    dawn_common unity sqlite3 pthread ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_briefing_prompt
   SOURCES test_briefing_prompt.c
           ${SRC_ROOT}/src/core/briefing_prompt.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity
   LABEL   ci)

# One IMAP connection per account: the lease's FIFO handover, cancel, timeout
# and release hook.  NDEBUG so a re-take by the holder returns instead of asserting.
dawn_add_unit_test(test_email_account_lease
   SOURCES test_email_account_lease.c
           ${SRC_ROOT}/src/tools/email_account_lease.c
   LIBS    dawn_common unity pthread
   DEFINES NDEBUG
   LABEL   ci)

# The WebUI email executor: admission per slot, and the executor under load
# with the real lease (stub account work and sender).
dawn_add_unit_test(test_webui_email_exec_policy
   SOURCES test_webui_email_exec_policy.c
           ${SRC_ROOT}/src/webui/webui_email_exec_policy.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_webui_email_exec
   SOURCES test_webui_email_exec.c
           ${SRC_ROOT}/src/webui/webui_email_exec.c
           ${SRC_ROOT}/src/webui/webui_email_exec_policy.c
           ${SRC_ROOT}/src/tools/email_account_lease.c
           ${SRC_ROOT}/src/tools/email_transfer.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES} ${CURL_LIBRARIES}
   LABEL   ci)

# Email failure codes on the wire, curl results as codes, the per-thread cancel.
dawn_add_unit_test(test_email_transfer
   SOURCES test_email_transfer.c
           ${SRC_ROOT}/src/tools/email_transfer.c
   LIBS    unity pthread ${CURL_LIBRARIES}
   LABEL   ci)

# An IMAP account's capabilities and Trash/Archive folders, from its replies.
dawn_add_unit_test(test_email_imap_roles
   SOURCES test_email_imap_roles.c
           ${SRC_ROOT}/src/tools/email_imap_roles.c
   LIBS    unity
   LABEL   ci)

# Reading a message: GMime over synthetic and hostile RFC 822, plus the Gmail
# part-list path.  GMime is only found when the email tool is on.
if(DAWN_ENABLE_EMAIL_TOOL)
   dawn_add_unit_test(test_email_mime
      SOURCES  test_email_mime.c
               ${SRC_ROOT}/src/tools/email_mime.c
               ${SRC_ROOT}/src/tools/email_display.c
               ${SRC_ROOT}/src/tools/gmail_parts.c
               ${SRC_ROOT}/src/tools/html_parser.c
      LIBS     dawn_common unity ${GMIME_LINK_LIBRARIES} ${JSONC_LIBRARIES}
      INCLUDES ${GMIME_INCLUDE_DIRS}
      LABEL    ci)
endif()

# The confirm rule: the same session, the next turn (any later one by code).
dawn_add_unit_test(test_turn_origin
   SOURCES test_turn_origin.c
   LIBS    unity
   LABEL   ci)

# Who may make a tool call: the caller's kind of turn against the action's kind.
dawn_add_unit_test(test_tool_call_policy
   SOURCES test_tool_call_policy.c test_tool_registry_stub.c
           ${SRC_ROOT}/src/core/tool_call_policy.c
           ${SRC_ROOT}/src/core/tool_call_challenge.c
           ${SRC_ROOT}/src/core/reply_code.c
           ${SRC_ROOT}/src/core/pending_slots.c
           ${SRC_ROOT}/src/tools/tool_registry.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES} ${SODIUM_LIBRARIES}
   LABEL   ci)

# Reply codes and the actions from a text that wait for them.
dawn_add_unit_test(test_tool_call_challenge
   SOURCES test_tool_call_challenge.c
           ${SRC_ROOT}/src/core/tool_call_challenge.c
           ${SRC_ROOT}/src/core/reply_code.c
           ${SRC_ROOT}/src/core/pending_slots.c
   LIBS    dawn_common unity pthread ${SODIUM_LIBRARIES}
   LABEL   ci)

# A call held for a reply code: its description, and the binding that covers it.
dawn_add_unit_test(test_llm_tools_reply_code
   SOURCES test_llm_tools_reply_code.c
           ${SRC_ROOT}/src/llm/llm_tools_reply_code.c
           ${SRC_ROOT}/src/core/tool_call_challenge.c
           ${SRC_ROOT}/src/core/reply_code.c
           ${SRC_ROOT}/src/core/pending_slots.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES} ${SODIUM_LIBRARIES}
   LABEL   ci)

# What a tool staged for the user's confirm: one item per session and kind.
dawn_add_unit_test(test_pending_slots
   SOURCES test_pending_slots.c
           ${SRC_ROOT}/src/core/pending_slots.c
           ${SRC_ROOT}/src/tools/tool_pending.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_email_parse
   SOURCES test_email_parse.c
           ${SRC_ROOT}/src/tools/email_parse.c
   LIBS    unity
   LABEL   ci)

# The mail panel's cursor and page merge across accounts.
dawn_add_unit_test(test_email_cursor
   SOURCES test_email_cursor.c
           ${SRC_ROOT}/src/webui/email_cursor.c
   LIBS    unity sodium ${JSONC_LIBRARIES}
   LABEL   ci)

# The mail panel's wire shapes and the read frame budget.
dawn_add_unit_test(test_email_wire
   SOURCES test_email_wire.c
           ${SRC_ROOT}/src/webui/email_wire.c
           ${SRC_ROOT}/src/tools/email_parse.c
   LIBS    unity ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_email_imap_state
   SOURCES test_email_imap_state.c
           ${SRC_ROOT}/src/tools/email_imap_state.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_email_digest_step
   SOURCES test_email_digest_step.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_music_db
   SOURCES test_music_db.c
           ${SRC_ROOT}/src/audio/music_db.c
           ${SRC_ROOT}/src/audio/music_db_query.c
           ${SRC_ROOT}/src/audio/music_rank.c
           ${SRC_ROOT}/src/audio/music_source.c
           ${SRC_ROOT}/src/core/path_utils.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_audio_metadata
   SOURCES test_audio_metadata.c
           ${SRC_ROOT}/src/audio/audio_metadata_util.c
   LIBS    unity
   INCLUDES ${SRC_ROOT}/common/include
   LABEL   ci)

dawn_add_unit_test(test_music_search
   SOURCES test_music_search.c
           ${SRC_ROOT}/src/tools/music_search.c
           ${SRC_ROOT}/src/audio/music_db.c
           ${SRC_ROOT}/src/audio/music_db_query.c
           ${SRC_ROOT}/src/audio/music_rank.c
           ${SRC_ROOT}/src/audio/music_source.c
           ${SRC_ROOT}/src/core/path_utils.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_music_rank
   SOURCES test_music_rank.c
           ${SRC_ROOT}/src/audio/music_rank.c
   LIBS    unity
   INCLUDES ${SRC_ROOT}/common/include
   LABEL   ci)

dawn_add_unit_test(test_stat_service
   SOURCES test_stat_service.c
           ${SRC_ROOT}/src/core/stat_service.c
           ${SRC_ROOT}/src/core/stat_db.c
           ${SRC_ROOT}/src/core/stat_net_interpret.c
           ${SRC_ROOT}/src/core/path_utils.c
           ${SRC_ROOT}/src/tools/stat_render.c
   LIBS    dawn_common unity sqlite3 pthread ${JSONC_LIBRARIES}
   LABEL   ci)
# Pin the DAWN-side Network parser to STAT's canonical wire fixture (sibling repo),
# so a wire-contract change on either side fails a test. The test skips gracefully
# when the STAT checkout isn't present (e.g. DAWN-only CI).
if(TARGET test_stat_service)
   target_compile_definitions(test_stat_service PRIVATE
      STAT_NET_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../stat/tests/fixtures/network_v1.json")
endif()

dawn_add_unit_test(test_suit_service
   SOURCES test_suit_service.c
           ${SRC_ROOT}/src/core/suit_service.c
   LIBS    dawn_common unity pthread m ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_session_history
   SOURCES test_session_history.c
           ${SRC_ROOT}/src/core/session_history.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
           ${SRC_ROOT}/src/core/hash_util.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES}
   LABEL   ci)

# DAWN's added text: the tag, the operator-note label, defused markers.
dawn_add_unit_test(test_llm_context_text
   SOURCES test_llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
   LIBS    unity
   LABEL   ci)

# The append-only request: the first turn freezes the system prompt, changes
# are appended after the question, the turn's context goes in front of it.
dawn_add_unit_test(test_session_prefix
   SOURCES test_session_prefix.c
           ${SRC_ROOT}/src/core/session_prefix.c
           ${SRC_ROOT}/src/core/session_focus.c
           ${SRC_ROOT}/src/core/focus/focus_incremental.c
           ${SRC_ROOT}/src/core/session_compaction.c
           ${SRC_ROOT}/src/llm/llm_compaction.c
           ${SRC_ROOT}/src/llm/llm_compaction_range.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/memory/memory_history_loader.c
           ${SRC_ROOT}/src/core/image_rehydrate.c
           ${SRC_ROOT}/src/core/ocp_helpers.c
           ${SRC_ROOT}/src/image_store.c
           ${SRC_ROOT}/src/blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
           ${SRC_ROOT}/src/core/prefix_message.c
           ${SRC_ROOT}/src/core/prefix_in_force.c
           ${SRC_ROOT}/src/core/prompt_sections.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_history_rows.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${AUTH_DB_SOURCES}
           ${SRC_ROOT}/src/core/prefix_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_satellite_remap
   SOURCES test_satellite_remap.c
           ${SRC_ROOT}/src/webui/satellite_remap.c
           ${SRC_ROOT}/src/core/turn_queue.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_turn_queue
   SOURCES test_turn_queue.c
           ${SRC_ROOT}/src/core/turn_queue.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

dawn_add_unit_test(test_memory_embeddings
   SOURCES test_memory_embeddings.c test_memory_embeddings_stub.c
           ${SRC_ROOT}/src/memory/memory_embeddings.c
           ${SRC_ROOT}/src/memory/memory_embeddings_entity.c
           ${SRC_ROOT}/src/memory/memory_terms.c
           ${SRC_ROOT}/src/memory/memory_stem.c
           ${SRC_ROOT}/src/memory/memory_embed_backfill.c
           ${SRC_ROOT}/src/core/time_query_parser.c
           ${SRC_ROOT}/src/core/embedding_engine.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer
   LABEL   ci)

dawn_add_unit_test(test_context_compaction
   SOURCES test_context_compaction.c
           ${SRC_ROOT}/src/llm/llm_compaction_range.c
           ${SRC_ROOT}/src/llm/llm_compaction.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_claude_blocks.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/llm_reasoning_details.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_time_query_parser
   SOURCES test_time_query_parser.c
           ${SRC_ROOT}/src/core/time_query_parser.c
   LIBS    unity m
   LABEL   ci)

# US-equity market-session calendar — NYSE holidays, observed shifts, half-days.
dawn_add_unit_test(test_market_hours
   SOURCES test_market_hours.c
           ${SRC_ROOT}/src/core/market_hours.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_attention_gate
   SOURCES test_attention_gate.c
           ${SRC_ROOT}/src/core/attention/attention_gate.c
           ${SRC_ROOT}/src/core/attention/attention_policy.c
           ${SRC_ROOT}/src/core/attention/attention_catalog.c
   LIBS    unity m
   LABEL   ci)

dawn_add_unit_test(test_endpointer
   SOURCES test_endpointer.c
           ${SRC_ROOT}/src/core/endpointer.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_spec_slot
   SOURCES test_spec_slot.c
           ${SRC_ROOT}/src/core/spec_slot.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_worker_pool_select
   SOURCES test_worker_pool_select.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_v49_migration
   SOURCES test_v49_migration.c
   LIBS    unity sqlite3
   LABEL   ci)

dawn_add_unit_test(test_v52_migration
   SOURCES test_v52_migration.c
   LIBS    unity sqlite3
   LABEL   ci)

dawn_add_unit_test(test_temporal_relations
   SOURCES test_temporal_relations.c test_temporal_relations_stub.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_db_facts.c
           ${SRC_ROOT}/src/memory/memory_db_summaries.c
           ${SRC_ROOT}/src/memory/memory_db_prefs.c
           ${SRC_ROOT}/src/memory/memory_db_entities.c
           ${SRC_ROOT}/src/auth/auth_db_ids.c
           ${SRC_ROOT}/src/memory/memory_db_relations.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
           # v48: memory_db_facts.c calls memory_bm25_* + memory_stem_* helpers
           # in the FTS5 sync path (fact_create / fact_delete) + search.
           ${SRC_ROOT}/src/memory/memory_bm25.c
           ${SRC_ROOT}/src/memory/memory_stem.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer
   LABEL   ci)

dawn_add_unit_test(test_relation_supersede
   SOURCES test_relation_supersede.c test_relation_supersede_stub.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_db_facts.c
           ${SRC_ROOT}/src/memory/memory_db_summaries.c
           ${SRC_ROOT}/src/memory/memory_db_prefs.c
           ${SRC_ROOT}/src/memory/memory_db_entities.c
           ${SRC_ROOT}/src/auth/auth_db_ids.c
           ${SRC_ROOT}/src/memory/memory_db_relations.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
           ${SRC_ROOT}/src/memory/memory_bm25.c
           ${SRC_ROOT}/src/memory/memory_stem.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer
   LABEL   ci)

# Entity-merge alias surface (v43).  Links the full auth_db chain so the v43
# schema and prepared statements are available out of the box; stubs the
# embedding engine so Stage 4 returns "no engine" gracefully (composite
# correctly forfeits the embedding-cosine term in tests).
dawn_add_unit_test(test_memory_db_alias
   SOURCES test_memory_db_alias.c test_memory_db_alias_stub.c
           ${SRC_ROOT}/src/memory/memory_db_alias.c
           ${SRC_ROOT}/src/memory/memory_db_alias_scorer.c
           ${SRC_ROOT}/src/memory/memory_db_alias_cascade.c
           ${SRC_ROOT}/src/memory/memory_db_alias_writes.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_db_facts.c
           ${SRC_ROOT}/src/memory/memory_db_summaries.c
           ${SRC_ROOT}/src/memory/memory_db_prefs.c
           ${SRC_ROOT}/src/memory/memory_db_entities.c
           ${SRC_ROOT}/src/memory/memory_db_relations.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
           # memory_bm25.c + memory_stem.c arrive via AUTH_DB_SOURCES below.
           # WARNING: keep config_defaults.c a leaf module — it is linked
           # into this test target so test setUp can call config_set_defaults
           # to seed g_config with real runtime values (otherwise the
           # entity_merge_*_threshold reads at runtime route every >0
           # composite to PROPOSED).  If config_defaults ever grows a call
           # into the LLM / audio / scheduler subsystems, those will pull
           # in as link-time deps here and this test target will need new
           # stubs.  Watch for transitive-include creep on every config
           # surface change.
           ${SRC_ROOT}/src/config/config_defaults.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# Document extract has conditional MuPDF / libzip+libxml2 linking, so the
# optional libs and defines are appended to the target after creation.
dawn_add_unit_test(test_document_extract
   SOURCES test_document_extract.c
           ${SRC_ROOT}/src/tools/document_extract.c
           ${SRC_ROOT}/src/tools/html_parser.c
   LIBS    dawn_common unity
   INCLUDES ${SRC_ROOT}/include
   LABEL   ci)
if(MUPDF_FOUND)
   target_link_libraries(test_document_extract PRIVATE
       ${MUPDF_LIB} ${MUPDF_THIRD_LIB}
       mujs gumbo openjp2 jbig2dec jpeg
       ${HARFBUZZ_LIB} ${FREETYPE2_LIBRARIES} z m)
   target_compile_definitions(test_document_extract PRIVATE HAVE_MUPDF)
else()
   target_link_libraries(test_document_extract PRIVATE m)
endif()
if(LIBZIP_FOUND AND LIBXML2_FOUND)
   target_link_libraries(test_document_extract PRIVATE
       ${LIBZIP_LIBRARIES} ${LIBXML2_LIBRARIES})
   target_compile_definitions(test_document_extract PRIVATE HAVE_LIBZIP HAVE_LIBXML2)
endif()

dawn_add_unit_test(test_document_chunker
   SOURCES test_document_chunker.c
           ${SRC_ROOT}/src/tools/document_chunker.c
   LIBS    unity m
   LABEL   ci)

dawn_add_unit_test(test_document_db
   SOURCES test_document_db.c test_document_db_stub.c
           ${SRC_ROOT}/src/tools/document_db.c
           ${SRC_ROOT}/src/auth/auth_db_ids.c
           ${SRC_ROOT}/src/tools/document_db_embeddings.c
           # v61: document_db.c now stems label/body for the FTS sync helpers and
           # runs the BM25 lexical search (build_match_expr + normalize).
           ${SRC_ROOT}/src/memory/memory_stem.c
           ${SRC_ROOT}/src/memory/memory_bm25.c
   LIBS    dawn_common unity sqlite3 pthread stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# Which stretch of the calendar a message asks about (pure; pinned to UTC).
dawn_add_unit_test(test_calendar_query_window
   SOURCES test_calendar_query_window.c
           ${SRC_ROOT}/src/tools/calendar_query_window.c
           ${SRC_ROOT}/src/core/iso8601.c
           ${SRC_ROOT}/src/core/time_query_parser.c
           ${SRC_ROOT}/src/memory/memory_terms.c
           ${SRC_ROOT}/src/memory/memory_stem.c
   LIBS    dawn_common unity pthread m stemmer
   LABEL   ci)

# v61: the "bios" search-quality test — exact-label lexical ranking (conv 809).
# Uses the REAL auth_db (v61 schema + prepared statements) via AUTH_DB_SOURCES,
# which already bundles memory_stem.c + memory_bm25.c.
dawn_add_unit_test(test_document_search_bm25
   SOURCES test_document_search_bm25.c
           ${SRC_ROOT}/src/tools/document_db.c
           ${SRC_ROOT}/src/tools/document_db_embeddings.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# document_manage edit/append pure text ops (B1a) — no storage deps
dawn_add_unit_test(test_document_manage
   SOURCES test_document_manage.c
           ${SRC_ROOT}/src/tools/document_edit_ops.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_embedding_engine
   SOURCES test_embedding_engine.c test_embedding_engine_stub.c
           ${SRC_ROOT}/src/core/embedding_engine.c
   LIBS    dawn_common unity pthread m
   LABEL   ci)

dawn_add_unit_test(test_plan_executor
   SOURCES test_plan_executor.c test_plan_executor_stub.c
           ${SRC_ROOT}/src/tools/plan_executor.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES} pthread
   LABEL   ci)

dawn_add_unit_test(test_wake_word
   SOURCES test_wake_word.c
           ${SRC_ROOT}/src/core/wake_word.c
   LIBS    dawn_common unity
   LABEL   ci)

# Messaging outbound splitter (paragraph / sentence / reject) — pure
# function with no DAWN deps, links only the .c file under test.
dawn_add_unit_test(test_messaging_split
   SOURCES test_messaging_split.c
           ${SRC_ROOT}/src/messaging/messaging_split.c
   LIBS    dawn_common unity
   LABEL   ci)

# Who may speak for a messaging channel: the sender rule + Telegram message
# reading.  Pure; links only the two .c files under test.
dawn_add_unit_test(test_messaging_sender
   SOURCES test_messaging_sender.c
           ${SRC_ROOT}/src/messaging/messaging_sender_gate.c
           ${SRC_ROOT}/src/messaging/messaging_telegram_parse.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Schema v101: messaging channel owners + SMS verification backfill on an
# existing database (the migration re-run over rows shaped like before it).
dawn_add_unit_test(test_messaging_channels_v101
   SOURCES test_messaging_channels_v101.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# Messaging channels on a real database: resolve by sender, /link, SMS
# verification.  The engine's channel + link files as they are; the driver
# registry and session slots are stand-ins in the test.
dawn_add_unit_test(test_messaging_channels_db
   SOURCES test_messaging_channels_db.c
           ${AUTH_DB_SOURCES}
           ${SRC_ROOT}/src/messaging/messaging_engine_channels.c
           ${SRC_ROOT}/src/messaging/messaging_engine_link.c
           ${SRC_ROOT}/src/core/reply_code.c
           ${SRC_ROOT}/src/core/tool_call_challenge.c
           ${SRC_ROOT}/src/core/pending_slots.c
           ${SRC_ROOT}/src/messaging/messaging_sender_gate.c
           ${SRC_ROOT}/src/messaging/messaging_format.c
           ${SRC_ROOT}/src/messaging/messaging_format_telegram.c
           ${SRC_ROOT}/src/messaging/messaging_split.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/core/rate_limiter.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer sodium ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_messaging_format
   SOURCES test_messaging_format.c
           ${SRC_ROOT}/src/messaging/messaging_format.c
           ${SRC_ROOT}/src/messaging/messaging_format_telegram.c
           ${SRC_ROOT}/src/messaging/messaging_split.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_claude_format
   SOURCES test_claude_format.c
           ${SRC_ROOT}/src/llm/llm_claude_format.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_capabilities.c
           ${SRC_ROOT}/src/llm/llm_model_family.c
           ${SRC_ROOT}/src/llm/llm_model_version.c
           ${SRC_ROOT}/src/tools/toml.c
           ${SRC_ROOT}/src/llm/llm_claude_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
           ${SRC_ROOT}/src/core/hash_util.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   DEFINES MODELS_TOML_PATH="${SRC_ROOT}/models.toml"
   LABEL   ci)

# Request layout for prompt caching: frozen system prompt, context in front of
# its question, directions in place, the conversation breakpoint, and each
# request's messages a prefix of the next one's.
dawn_add_unit_test(test_prompt_cache_layout
   SOURCES test_prompt_cache_layout.c
           ${SRC_ROOT}/src/llm/llm_claude_format.c
           ${SRC_ROOT}/src/llm/llm_openai_history.c
           ${SRC_ROOT}/src/llm/llm_openai_responses_input.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_capabilities.c
           ${SRC_ROOT}/src/llm/llm_model_family.c
           ${SRC_ROOT}/src/llm/llm_model_version.c
           ${SRC_ROOT}/src/tools/toml.c
           ${SRC_ROOT}/src/llm/llm_claude_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
           ${SRC_ROOT}/src/core/hash_util.c
           ${SRC_ROOT}/src/core/focus/focus_incremental.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   DEFINES MODELS_TOML_PATH="${SRC_ROOT}/models.toml"
   LABEL   ci)

# A chat-completions request's leading system messages merged (copy-on-write).
dawn_add_unit_test(test_openai_system_merge
   SOURCES test_openai_system_merge.c
           ${SRC_ROOT}/src/llm/llm_openai_cache.c
   LIBS    unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Duplicate-tool-call turn-scoping.  Links the real llm_tools.c against a thin
# stub TU that provides its wide dep surface (the dup-check path reaches almost
# none of it).
dawn_add_unit_test(test_llm_dup_check
   SOURCES test_llm_dup_check.c test_llm_dup_check_stub.c test_tool_call_policy_stub.c
           ${SRC_ROOT}/src/llm/llm_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_tools_filter.c
           ${SRC_ROOT}/src/llm/llm_tools_results.c
           ${SRC_ROOT}/src/llm/llm_tools_dup.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/core/hash_util.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES} pthread
   LABEL   ci)

# The batch finish contract: results reach the finish step raw and leave
# finished (neutralized, announced).  Same link as test_llm_dup_check.
dawn_add_unit_test(test_llm_tools_finish
   SOURCES test_llm_tools_finish.c test_llm_dup_check_stub.c test_tool_call_policy_stub.c
           ${SRC_ROOT}/src/llm/llm_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_tools_filter.c
           ${SRC_ROOT}/src/llm/llm_tools_results.c
           ${SRC_ROOT}/src/llm/llm_tools_dup.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/core/hash_util.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES} pthread
   LABEL   ci)

# Tool-call arguments that don't fit are flagged on every path (and refused).
dawn_add_unit_test(test_llm_tools_args
   SOURCES test_llm_tools_args.c test_llm_dup_check_stub.c test_tool_call_policy_stub.c
           ${SRC_ROOT}/src/llm/llm_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_tools_filter.c
           ${SRC_ROOT}/src/llm/llm_tools_results.c
           ${SRC_ROOT}/src/llm/llm_tools_dup.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/core/hash_util.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES} pthread
   LABEL   ci)

# A conversation's tools are decided by registration, not enable flags; a
# tool a session may not use is never offered to it, frozen or added later.
dawn_add_unit_test(test_llm_tools_frozen
   SOURCES test_llm_tools_frozen.c test_llm_dup_check_stub.c test_tool_call_policy_stub.c
           ${SRC_ROOT}/src/llm/llm_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_tools_filter.c
           ${SRC_ROOT}/src/llm/llm_tools_results.c
           ${SRC_ROOT}/src/llm/llm_tools_dup.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/core/hash_util.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES} pthread
   LABEL   ci)

# A tool's images inside its result: counted (image budget, token estimate)
# and stripped for a model without vision, on every history shape.  Links the
# real llm_tools.c against the dup-check stub, as test_llm_dup_check does.
dawn_add_unit_test(test_tool_images_render
   SOURCES test_tool_images_render.c test_llm_dup_check_stub.c test_tool_call_policy_stub.c
           ${SRC_ROOT}/src/llm/llm_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_compaction.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_tools_filter.c
           ${SRC_ROOT}/src/llm/llm_tools_results.c
           ${SRC_ROOT}/src/llm/llm_tools_dup.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/core/hash_util.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES} pthread
   LABEL   ci)

dawn_add_unit_test(test_instruction_loader
   SOURCES test_instruction_loader.c
           ${SRC_ROOT}/src/tools/instruction_loader.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_phone_db
   SOURCES test_phone_db.c test_scheduler_stub.c
           ${SRC_ROOT}/src/tools/phone_db.c
           ${SRC_ROOT}/src/tools/phone_number.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_missed_notifications_db
   SOURCES test_missed_notifications_db.c test_scheduler_stub.c
           ${SRC_ROOT}/src/core/missed_notifications_db.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_memory_filter
   SOURCES test_memory_filter.c
           ${SRC_ROOT}/src/core/memory_filter.c
   LIBS    dawn_common unity
   LABEL   ci)

# Focus-source framework (Phase 1b of dynamic context injection).  Links
# the real memory_filter so filter-on-retrieval is exercised end-to-end;
# the test provides g_config inline.
dawn_add_unit_test(test_focus_source
   SOURCES test_focus_source.c
           ${SRC_ROOT}/src/core/focus/focus_source.c
           ${SRC_ROOT}/src/core/focus/focus_dominant_token.c
           ${SRC_ROOT}/src/core/focus/focus_candidate_helpers.c
           ${SRC_ROOT}/src/core/memory_filter.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

# Phase B-ii: dominant-token over-inclusion heuristic.  Standalone unit
# tests for the penalty module — no DB, no embedding engine, no LLM.
# Covers no-op contracts, dominance detection, penalty math, clamps,
# FOCUS_SCORE_NA sentinel protection, and a Phase-A severe-pathology
# integration smoke.
dawn_add_unit_test(test_focus_dominant_token
   SOURCES test_focus_dominant_token.c
           ${SRC_ROOT}/src/core/focus/focus_dominant_token.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

# Recall result formatter — pure rendering of a focus_compose result into the
# grouped, source-tagged "here's what we know" block.  No DB / no engine.
dawn_add_unit_test(test_recall_format
   SOURCES test_recall_format.c
           ${SRC_ROOT}/src/tools/recall_format.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

# Memory-citation <cited> tokenizer + two-set validation (Option B).  Links the
# pure parse core only — no session/DB/config.
dawn_add_unit_test(test_memory_citation_parse
   SOURCES test_memory_citation_parse.c
           ${SRC_ROOT}/src/memory/memory_citation_parse.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

# Phase 1c: extracted fact-search helper.  Links the production helper
# against programmable memory_db_*/memory_embeddings_* stubs so the
# pipeline's tokenize → multi-token-keyword → hybrid re-rank flow can
# be driven without a real DB.
dawn_add_unit_test(test_memory_fact_search
   SOURCES test_memory_fact_search.c test_memory_focus_adapters_stub.c
           ${SRC_ROOT}/src/memory/memory_fact_search.c
   LIBS    dawn_common unity pthread m
   LABEL   ci)

# Phase 1c: focus-source memory adapters (fact, entity, relation, summary).
# Links the four adapters + the framework + the extracted fact-search
# helper + the Phase-1d shared scaffolding (focus_recency,
# focus_candidate_helpers) against the same programmable stubs.
dawn_add_unit_test(test_memory_focus_adapters
   SOURCES test_memory_focus_adapters.c test_memory_focus_adapters_stub.c
           ${SRC_ROOT}/src/memory/memory_focus_adapters.c
           ${SRC_ROOT}/src/memory/memory_fact_search.c
           ${SRC_ROOT}/src/memory/memory_embeddings_entity.c
           ${SRC_ROOT}/src/memory/memory_terms.c
           ${SRC_ROOT}/src/core/focus/focus_source.c
           ${SRC_ROOT}/src/core/focus/focus_recency.c
           ${SRC_ROOT}/src/core/focus/focus_candidate_helpers.c
           ${SRC_ROOT}/src/core/focus/focus_dominant_token.c
   LIBS    dawn_common unity pthread m
   LABEL   ci)

# build_focus_block (the turn's retrieved items, as data) without the full
# session-manager runtime: focus_compose / memory_embeddings are programmable
# stubs.
dawn_add_unit_test(test_prompt_builder
   SOURCES test_prompt_builder.c test_prompt_builder_stub.c
           ${SRC_ROOT}/src/core/focus/focus_candidate_helpers.c
           ${SRC_ROOT}/src/core/focus/focus_incremental.c
           ${SRC_ROOT}/src/core/prompt_sections.c
           ${SRC_ROOT}/src/core/build_focus_block.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES}
   LABEL   ci)

# Incremental focus: which items a history already shows, which a turn sends,
# and how it says so.  Pure (json-c and the LLM text helpers).
dawn_add_unit_test(test_focus_incremental
   SOURCES test_focus_incremental.c
           ${SRC_ROOT}/src/core/focus/focus_incremental.c
           ${SRC_ROOT}/src/core/prompt_sections.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Phase 1i.B: focus-injection probe (Components 4 + 5).  Probe-style
# coverage for the ranker (recency / importance / source-weight), dedup
# (suppress / uplift), and budget force-keep-first behaviors via a
# fake adapter that emits a synthetic candidate pool.  Differs from
# test_prompt_builder by using the REAL focus_compose + framework
# (not stubs) so the ranker is exercised end-to-end against the same
# code path production runs.  Stub TU covers only memory_embeddings
# + webui_broadcast.
dawn_add_unit_test(test_focus_probe
   SOURCES test_focus_probe.c test_focus_probe_stub.c
           ${SRC_ROOT}/src/core/focus/focus_source.c
           ${SRC_ROOT}/src/core/focus/focus_recency.c
           ${SRC_ROOT}/src/core/focus/focus_candidate_helpers.c
           ${SRC_ROOT}/src/core/focus/focus_dominant_token.c
           ${SRC_ROOT}/src/core/memory_filter.c
           ${SRC_ROOT}/src/core/focus/focus_incremental.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/core/build_focus_block.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
   LIBS    dawn_common unity pthread m ${JSONC_LIBRARIES}
   LABEL   ci)

# Phase 1d: external focus-source adapters (document chunks + calendar
# events; email deferred to v2 — see design doc §"API Audit (Phase 1d)").
# Links the two adapters + their init wrapper + the framework + the
# Phase-1d shared scaffolding against programmable document_db /
# calendar_db / memory_embeddings stubs.  Network-call invariant
# enforced at link level: no service-layer symbols are stubbed; if an
# adapter ever regresses to call them the link breaks.
dawn_add_unit_test(test_external_focus_adapters
   SOURCES test_external_focus_adapters.c test_external_focus_adapters_stub.c
           ${SRC_ROOT}/src/tools/document_focus_adapter.c
           ${SRC_ROOT}/src/tools/document_embed_cache.c
           ${SRC_ROOT}/src/tools/document_rank.c
           ${SRC_ROOT}/src/tools/calendar_focus_adapter.c
           ${SRC_ROOT}/src/tools/calendar_query_window.c
           ${SRC_ROOT}/src/core/iso8601.c
           ${SRC_ROOT}/src/tools/external_focus_adapters_init.c
           ${SRC_ROOT}/src/core/focus/focus_source.c
           ${SRC_ROOT}/src/core/focus/focus_recency.c
           ${SRC_ROOT}/src/core/focus/focus_candidate_helpers.c
           ${SRC_ROOT}/src/core/focus/focus_dominant_token.c
           ${SRC_ROOT}/src/core/time_query_parser.c
           ${SRC_ROOT}/src/memory/memory_stem.c
           ${SRC_ROOT}/src/memory/memory_terms.c
   LIBS    dawn_common unity pthread m stemmer
   LABEL   ci)

# Silent-observe primitive (Phase 0 of Dynamic Context Injection).
# Stubs llm_chat_completion_with_config + memory_filter_check via
# strong-symbol replacement in the test TU.
dawn_add_unit_test(test_silent_observe
   SOURCES test_silent_observe.c
           ${SRC_ROOT}/src/llm/llm_silent_observe.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES} pthread
   LABEL   ci)

# Per-call prompt-cache telemetry: prefix hash of a noted request, expected-read
# bookkeeping per cache key.  Session lookup and provider names are stubbed.
dawn_add_unit_test(test_llm_cache_monitor
   SOURCES test_llm_cache_monitor.c
           ${SRC_ROOT}/src/llm/llm_cache_monitor.c
           ${SRC_ROOT}/src/llm/llm_model_family.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES} pthread
   LABEL   ci)

# Per-model reasoning capabilities + the mode/effort resolver, against the
# shipped models.toml.
dawn_add_unit_test(test_llm_capabilities
   SOURCES test_llm_capabilities.c
           ${SRC_ROOT}/src/llm/llm_capabilities.c
           ${SRC_ROOT}/src/llm/llm_model_family.c
           ${SRC_ROOT}/src/tools/toml.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   DEFINES MODELS_TOML_PATH="${SRC_ROOT}/models.toml"
   LABEL   ci)

# Loading models.toml: the disk copy over the built-in one, table by table.
dawn_add_unit_test(test_llm_models_toml
   SOURCES test_llm_models_toml.c
           ${SRC_ROOT}/src/llm/llm_models_toml.c
           ${SRC_ROOT}/src/tools/toml.c
   LIBS    dawn_common unity
   LABEL   ci)

# Claude content-block capture + provider-neutral turn blocks (replay fidelity).
dawn_add_unit_test(test_llm_turn_blocks
   SOURCES test_llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_claude_blocks.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/llm_reasoning_details.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# The rows a history message saves as: canonical rows pinned by fixture, the
# turn's blocks alongside, no vendor data in client-visible fields.
dawn_add_unit_test(test_llm_history_rows
   SOURCES test_llm_history_rows.c
           ${SRC_ROOT}/src/llm/llm_history_rows.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Request context on a history: fold on load, the rows it saves as, and the
# copies that must never carry it (extraction, summaries) or its marks (wire).
dawn_add_unit_test(test_llm_history_kind
   SOURCES test_llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_history_rows.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Chat-completions history preparation: no internal keys or foreign reasoning.
dawn_add_unit_test(test_openai_history
   SOURCES test_openai_history.c
           ${SRC_ROOT}/src/llm/llm_openai_history.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Per-model prompt-cache pricing, against the shipped models.toml.
dawn_add_unit_test(test_llm_pricing
   SOURCES test_llm_pricing.c
           ${SRC_ROOT}/src/llm/llm_pricing.c
           ${SRC_ROOT}/src/llm/llm_model_family.c
           ${SRC_ROOT}/src/tools/toml.c
   LIBS    dawn_common unity m
   DEFINES MODELS_TOML_PATH="${SRC_ROOT}/models.toml"
   LABEL   ci)

dawn_add_unit_test(test_llm_claude_binding
   SOURCES test_llm_claude_binding.c
           ${SRC_ROOT}/src/llm/llm_claude_binding.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# The HUD tools' schemas stay as registered through a helmet discovery cycle
# (a conversation freezes them); live elements and modes are described and
# checked at the call instead.
dawn_add_unit_test(test_hud_tools
   SOURCES test_hud_tools.c
           ${SRC_ROOT}/src/tools/hud_tools.c
           ${SRC_ROOT}/src/tools/hud_discovery.c
   LIBS    dawn_common unity mosquitto ${JSONC_LIBRARIES} pthread
   LABEL   ci)

# Anthropic betas: request fields, header, and which 400s turn one off.
dawn_add_unit_test(test_llm_claude_betas
   SOURCES test_llm_claude_betas.c
           ${SRC_ROOT}/src/llm/llm_claude_betas.c
           ${SRC_ROOT}/src/llm/llm_model_family.c
           ${SRC_ROOT}/src/llm/llm_claude_route.c
           ${SRC_ROOT}/src/llm/llm_turn_result.c
   LIBS    dawn_common unity pthread ${CURL_LIBRARIES} ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_word_to_number
   SOURCES test_word_to_number.c
           ${SRC_ROOT}/src/word_to_number.c
   LIBS    unity m
   LABEL   ci)

dawn_add_unit_test(test_text_filter
   SOURCES test_text_filter.c
           ${SRC_ROOT}/src/core/text_filter.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_llm_response_finalize
   SOURCES test_llm_response_finalize.c
           ${SRC_ROOT}/src/core/llm_response_finalize.c
           ${SRC_ROOT}/src/core/text_filter.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_llm_model_version
   SOURCES test_llm_model_version.c
           ${SRC_ROOT}/src/llm/llm_model_version.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_responses_input
   SOURCES test_responses_input.c
           ${SRC_ROOT}/src/llm/llm_openai_responses_input.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_iso8601
   SOURCES test_iso8601.c
           ${SRC_ROOT}/src/core/iso8601.c
   LIBS    unity
   LABEL   ci)

# sanitize_utf8_for_json — the guard that keeps invalid UTF-8 out of WebSocket
# text frames (string_utils.c lives in dawn_common).
dawn_add_unit_test(test_string_utils
   SOURCES test_string_utils.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_str_fuzzy
   SOURCES test_str_fuzzy.c
           ${SRC_ROOT}/src/core/str_fuzzy.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_homeassistant_match
   SOURCES test_homeassistant_match.c
           ${SRC_ROOT}/src/tools/homeassistant_match.c
           ${SRC_ROOT}/src/core/str_fuzzy.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_calendar_pick
   SOURCES test_calendar_pick.c
           ${SRC_ROOT}/src/tools/calendar_pick.c
           ${SRC_ROOT}/src/core/str_fuzzy.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_memory_extraction_input
   SOURCES test_memory_extraction_input.c
           ${SRC_ROOT}/src/memory/memory_extraction_input.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
   LIBS    dawn_common unity json-c
   LABEL   ci)

# The dawn-admin cache stats table.
dawn_add_unit_test(test_admin_cache_stats
   SOURCES test_admin_cache_stats.c
           ${SRC_ROOT}/src/auth/admin_socket_cache.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_scheduled_context
   SOURCES test_scheduled_context.c
           ${SRC_ROOT}/src/core/scheduled_context.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_strbuf
   SOURCES test_strbuf.c
           ${SRC_ROOT}/src/core/strbuf.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_memory_provenance
   SOURCES test_memory_provenance.c test_memory_provenance_stub.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_db_forget.c
           ${SRC_ROOT}/src/memory/memory_db_facts.c
           ${SRC_ROOT}/src/memory/memory_db_summaries.c
           ${SRC_ROOT}/src/memory/memory_db_prefs.c
           ${SRC_ROOT}/src/memory/memory_db_entities.c
           ${SRC_ROOT}/src/auth/auth_db_ids.c
           ${SRC_ROOT}/src/memory/memory_db_relations.c
           ${SRC_ROOT}/src/memory/memory_db_provenance.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
           ${SRC_ROOT}/src/memory/memory_bm25.c
           ${SRC_ROOT}/src/memory/memory_stem.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer
   LABEL   ci)

dawn_add_unit_test(test_memory_summarize_missing
   SOURCES test_memory_summarize_missing.c test_memory_summarize_missing_stub.c
           ${SRC_ROOT}/src/memory/memory_summarize_missing.c
   LIBS    dawn_common unity sqlite3 pthread m ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_memory_db_admin
   SOURCES test_memory_db_admin.c test_memory_db_admin_stub.c
           ${SRC_ROOT}/src/memory/memory_db_admin.c
           # memory_db.c linked so memory_db_undo_extraction_attempt is
           # exercised directly (transient-failure rollback path).
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_bm25.c
           ${SRC_ROOT}/src/memory/memory_stem.c
   LIBS    dawn_common unity sqlite3 pthread m stemmer
   LABEL   ci)

dawn_add_unit_test(test_memory_source_dedup
   SOURCES test_memory_source_dedup.c
           ${SRC_ROOT}/src/memory/memory_source_dedup.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_memory_score_floor
   SOURCES test_memory_score_floor.c
           ${SRC_ROOT}/src/memory/memory_score_floor.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_memory_bm25
   SOURCES test_memory_bm25.c
           ${SRC_ROOT}/src/memory/memory_bm25.c
   # dawn_common supplies log_message (memory_bm25_build_match_expr logs on truncation).
   LIBS    dawn_common unity m
   LABEL   ci)

# Phase 9 note-extraction guard.  Standalone — json-c + an inline g_config;
# no DB, no embeddings.  Drives both provider history shapes through
# collect + redact.
dawn_add_unit_test(test_memory_note_guard
   SOURCES test_memory_note_guard.c
           ${SRC_ROOT}/src/memory/memory_note_guard.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Phase 9 memory→note bridge lifecycle.  Real v61 schema via auth_db_init
# (:memory:); the embedding engine is stubbed in the test TU so this stays a
# DB-only fact/note_doc_id lifecycle test (cosine retrieval is covered by the
# live 809 re-run).
dawn_add_unit_test(test_memory_note_bridge
   SOURCES test_memory_note_bridge.c
           ${SRC_ROOT}/src/memory/memory_note_bridge.c
           ${SRC_ROOT}/src/core/memory_filter.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_db_facts.c
           ${SRC_ROOT}/src/memory/memory_db_summaries.c
           ${SRC_ROOT}/src/memory/memory_db_prefs.c
           ${SRC_ROOT}/src/memory/memory_db_entities.c
           ${SRC_ROOT}/src/memory/memory_db_relations.c
           ${SRC_ROOT}/src/memory/memory_db_provenance.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# Merged facts are pruned a window after the merge, not after creation (v95).
dawn_add_unit_test(test_memory_fact_prune
   SOURCES test_memory_fact_prune.c
           ${SRC_ROOT}/src/core/memory_filter.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_db_facts.c
           ${SRC_ROOT}/src/memory/memory_db_summaries.c
           ${SRC_ROOT}/src/memory/memory_db_prefs.c
           ${SRC_ROOT}/src/memory/memory_db_entities.c
           ${SRC_ROOT}/src/memory/memory_db_relations.c
           ${SRC_ROOT}/src/memory/memory_db_provenance.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_memory_stem
   SOURCES test_memory_stem.c
           ${SRC_ROOT}/src/memory/memory_stem.c
   LIBS    dawn_common unity stemmer pthread
   LABEL   ci)

dawn_add_unit_test(test_memory_predicate_dedup
   SOURCES test_memory_predicate_dedup.c
           test_memory_predicate_dedup_stub.c
           ${SRC_ROOT}/src/memory/memory_predicate_dedup.c
   LIBS    unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_memory_similarity
   SOURCES test_memory_similarity.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
   LIBS    unity m
   LABEL   ci)

dawn_add_unit_test(test_memory_extraction
   SOURCES test_memory_extraction.c
   LIBS    unity ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_embed_recompute
   SOURCES test_embed_recompute.c test_embed_recompute_stub.c
           ${SRC_ROOT}/src/memory/memory_embed_recompute.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_embed_tokenizer
   SOURCES test_embed_tokenizer.c
           ${SRC_ROOT}/src/memory/memory_embed_tokenizer.c
           ${SRC_ROOT}/src/memory/memory_embed_unicode.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

dawn_add_unit_test(test_rate_limiter
   SOURCES test_rate_limiter.c
           ${SRC_ROOT}/src/core/rate_limiter.c
   LIBS    unity pthread
   LABEL   ci)

dawn_add_unit_test(test_llm_interrupt_ctx
   SOURCES test_llm_interrupt_ctx.c
           ${SRC_ROOT}/src/llm/llm_rate_limit.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

dawn_add_unit_test(test_utterance_dedup
   SOURCES test_utterance_dedup.c
           ${SRC_ROOT}/src/core/utterance_dedup.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

dawn_add_unit_test(test_tavily_rate_limit
   SOURCES test_tavily_rate_limit.c
           ${SRC_ROOT}/src/tools/tavily_rate_limit.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

dawn_add_unit_test(test_flaresolverr_error_detection
   SOURCES test_flaresolverr_error_detection.c
           ${SRC_ROOT}/src/tools/url_fetcher_detect.c
           ${SRC_ROOT}/src/tools/url_snippet_cache.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

dawn_add_unit_test(test_crypto_store
   SOURCES test_crypto_store.c test_crypto_store_stub.c
           ${SRC_ROOT}/src/core/crypto_store.c
           ${SRC_ROOT}/src/llm/llm_key_tag.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
   LIBS    dawn_common unity sodium pthread ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_input_queue
   SOURCES test_input_queue.c
           ${SRC_ROOT}/src/input_queue.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

dawn_add_unit_test(test_auth_db_migrations_v72
   SOURCES test_auth_db_migrations_v72.c
           ${SRC_ROOT}/src/auth/auth_db_migrations_v72.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_auth_db_migrations_v73
   SOURCES test_auth_db_migrations_v73.c
           ${SRC_ROOT}/src/auth/auth_db_migrations_v72.c
           ${SRC_ROOT}/src/auth/auth_db_migrations_v73.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_auth_db_migrations_v75
   SOURCES test_auth_db_migrations_v75.c
           ${SRC_ROOT}/src/auth/auth_db_migrations_v72.c
           ${SRC_ROOT}/src/auth/auth_db_migrations_v75.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_conv_db_events
   SOURCES test_conv_db_events.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# messages.llm_blocks: replay-only read, assistant-only CHECK, watermark GC,
# edit trigger, and the v92 re-render of legacy voice rows.
dawn_add_unit_test(test_conv_db_llm_blocks
   SOURCES test_conv_db_llm_blocks.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# messages.kind (request-context rows only the replay read returns), the frozen
# prefix and tool set, the reasoning floor, and stable citation handles.
dawn_add_unit_test(test_conv_db_kind_prefix
   SOURCES test_conv_db_kind_prefix.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# Images a tool returns (real auth_db, real image store): stored unbound and
# owner-only, bound with their tool row, reclaimed otherwise, reloaded as the
# live turn built them, saved by a voice save, deleted with their conversation.
dawn_add_unit_test(test_tool_images
   SOURCES test_tool_images.c
           ${SRC_ROOT}/src/llm/llm_tool_images.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/core/conv_images.c
           ${SRC_ROOT}/src/core/session_image_hold.c
           ${SRC_ROOT}/src/llm/llm_model_family.c
           ${SRC_ROOT}/src/core/session_voice_save.c
           ${SRC_ROOT}/src/memory/memory_history_loader.c
           ${SRC_ROOT}/src/core/image_rehydrate.c
           ${SRC_ROOT}/src/core/ocp_helpers.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/image_store.c
           ${SRC_ROOT}/src/blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
           ${SRC_ROOT}/src/document_original_store.c
           ${SRC_ROOT}/src/core/prefix_message.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_history_rows.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_compaction_range.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# A session's stable memory citation handles (real auth_db): numbered before
# the conversation exists, saved with it, the same after a reload.
dawn_add_unit_test(test_focus_handles
   SOURCES test_focus_handles.c
           ${SRC_ROOT}/src/core/focus/focus_handles.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# auth.db's storage: checkpoints off the global lock, auto-vacuum (real auth_db).
dawn_add_unit_test(test_auth_db_storage
   SOURCES test_auth_db_storage.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# The session reaper: a destroyed session is finished on its last reference.
dawn_add_unit_test(test_session_reaper
   SOURCES test_session_reaper.c
           ${SRC_ROOT}/src/core/session_reaper.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES}
   LABEL   ci)

# A Referer's origin for the same-origin (CSRF) check.
dawn_add_unit_test(test_webui_origin
   SOURCES test_webui_origin.c
           ${SRC_ROOT}/src/webui/webui_origin.c
   LIBS    unity
   LABEL   ci)

# One login cookie per app (the WebUI and Aurora keep separate logins).
dawn_add_unit_test(test_webui_login_cookie
   SOURCES test_webui_login_cookie.c
           ${SRC_ROOT}/src/webui/webui_login_cookie.c
   LIBS    unity
   LABEL   ci)

# The tool loop's view stage: the batch budget, its split, the header.
dawn_add_unit_test(test_llm_tool_views
   SOURCES test_llm_tool_views.c
           ${SRC_ROOT}/src/llm/llm_tool_views_plan.c
   LIBS    unity m
   LABEL   ci)

# The WebUI protocol's version and feature flags.
dawn_add_unit_test(test_webui_protocol
   SOURCES test_webui_protocol.c
           ${SRC_ROOT}/src/webui/webui_protocol.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

# Stored tool results: who may read one, binding, cascades (real auth_db).
dawn_add_unit_test(test_tool_result_store
   SOURCES test_tool_result_store.c
           ${SRC_ROOT}/src/core/tool_result_store.c
           ${SRC_ROOT}/src/llm/llm_tool_view.c
           ${SRC_ROOT}/src/llm/llm_tool_view_path.c
           ${SRC_ROOT}/src/blob_store_util.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_conv_db_jobs
   SOURCES test_conv_db_jobs.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_auth_db_research
   SOURCES test_auth_db_research.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_research_run
   SOURCES test_research_run.c
           ${SRC_ROOT}/src/tools/research_run.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# Header-only predicate (the read-only research allowlist, HIGH-1): no link deps.
dawn_add_unit_test(test_research_allowlist
   SOURCES test_research_allowlist.c
   LIBS    unity
   LABEL   ci)

# A compaction point recorded inside a tool exchange reloads cleanly (real
# auth_db, in memory).
dawn_add_unit_test(test_history_reload_watermark
   SOURCES test_history_reload_watermark.c
           ${SRC_ROOT}/src/memory/memory_history_loader.c
           ${SRC_ROOT}/src/core/prefix_message.c
           ${SRC_ROOT}/src/core/image_rehydrate.c
           ${SRC_ROOT}/src/core/ocp_helpers.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/image_store.c
           ${SRC_ROOT}/src/blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_compaction_range.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# Pure string transform shared by six history-hydration callers  (json-c is for
# the rest of the TU, not the helper under test) (resume,
# reinvoke, messaging drift reload, ...).  Links the loader against the real
# auth_db sources because the TU needs them, though only the pure helper is
# exercised.
dawn_add_unit_test(test_history_strip_markers
   SOURCES test_history_strip_markers.c
           ${SRC_ROOT}/src/memory/memory_history_loader.c
           ${SRC_ROOT}/src/core/prefix_message.c
           ${SRC_ROOT}/src/core/image_rehydrate.c
           ${SRC_ROOT}/src/core/ocp_helpers.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/image_store.c
           ${SRC_ROOT}/src/blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_compaction_range.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# Background-job session pool.
dawn_add_unit_test(test_job_manager
   SOURCES test_job_manager.c
           ${SRC_ROOT}/src/core/job_manager.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

dawn_add_unit_test(test_conv_stream
   SOURCES test_conv_stream.c
           ${SRC_ROOT}/src/core/conv_stream.c
   LIBS    unity pthread
   LABEL   ci)

dawn_add_unit_test(test_config_validate
   SOURCES test_config_validate.c
           ${SRC_ROOT}/src/config/config_validate.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_config_roundtrip
   SOURCES test_config_roundtrip.c test_config_roundtrip_stub.c
           ${SRC_ROOT}/src/config/config_parser.c
           ${SRC_ROOT}/src/config/config_env.c
           ${SRC_ROOT}/src/config/config_defaults.c
           ${SRC_ROOT}/src/tools/toml.c
   LIBS    unity json-c m pthread
   LABEL   ci)

dawn_add_unit_test(test_calculator
   SOURCES test_calculator.c
           ${SRC_ROOT}/src/tools/calculator.c
           ${SRC_ROOT}/src/tools/calculator_bignum.c
           ${SRC_ROOT}/src/tools/tinyexpr.c
   LIBS    dawn_common unity m pthread
   LABEL   ci)

dawn_add_unit_test(test_event_payload
   SOURCES test_event_payload.c
           ${SRC_ROOT}/src/core/event_payload.c
           ${SRC_ROOT}/common/src/utils/string_utils.c
   LIBS    unity json-c m pthread
   LABEL   ci)

# Who an action goes to: names, numbers and addresses resolved without guessing.
dawn_add_unit_test(test_contact_resolve
   SOURCES test_contact_resolve.c test_contacts_db_stub.c
           ${SRC_ROOT}/src/tools/contact_resolve.c
           ${SRC_ROOT}/src/tools/phone_number.c
           ${SRC_ROOT}/src/memory/contacts_db.c
           ${SRC_ROOT}/src/core/str_fuzzy.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_contacts_db
   SOURCES test_contacts_db.c test_contacts_db_stub.c
           ${SRC_ROOT}/src/memory/contacts_db.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_calendar_db
   SOURCES test_calendar_db.c
           ${SRC_ROOT}/src/tools/calendar_db.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_auth_db
   SOURCES test_auth_db.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_tool_registry
   SOURCES test_tool_registry.c test_tool_registry_stub.c
           ${SRC_ROOT}/src/tools/tool_registry.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES}
   LABEL   ci)

# Round-trip symmetry gate for the [phone] audio config (parse<->write). Links
# the dependency-light audio-config module + toml + phone_apm's pure default
# helper (phone_apm.cpp compiles as a stub without AEC_BACKEND_WEBRTC).
dawn_add_unit_test(test_phone_audio_config
   SOURCES test_phone_audio_config.c
           ${SRC_ROOT}/src/tools/phone_audio_config.c
           ${SRC_ROOT}/src/tools/toml.c
           ${SRC_ROOT}/src/audio/phone_apm.cpp
           ${SRC_ROOT}/common/src/logging.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_device_types
   SOURCES test_device_types.c
           ${SRC_ROOT}/src/core/device_types.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_html_parser
   SOURCES test_html_parser.c
           ${SRC_ROOT}/src/tools/html_parser.c
   LIBS    dawn_common unity
   LABEL   ci)

# tts_preprocessing.cpp is compiled directly (rather than linking dawn_common_tts)
# to avoid the Piper / ONNX runtime dependency.
dawn_add_unit_test(test_tts_preprocessing
   SOURCES test_tts_preprocessing.c
           ${SRC_ROOT}/common/src/tts/tts_preprocessing.cpp
           ${SRC_ROOT}/common/src/tts/number_to_words.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_number_to_words
   SOURCES test_number_to_words.c
           ${SRC_ROOT}/common/src/tts/number_to_words.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_email_db
   SOURCES test_email_db.c test_email_db_stub.c
           ${SRC_ROOT}/src/tools/email_db.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m stemmer ${JSONC_LIBRARIES}
   LABEL   ci)

# =============================================================================
# Hardware-dependent tests (require ONNX, CUDA, or external APIs)
# =============================================================================

# NOTE: test_streaming (manual live-API streaming smoke, no assertions, LABEL
# hardware) was dropped + removed 2026-08-25 — bit-rotted uncompilable, needs live
# keys so never in CI, and its testable surface is covered by test_sse_parser (SSE
# parse) + test_responses_input (Responses request shaping) + live usage.

dawn_add_unit_test(asr_benchmark
   SOURCES asr_benchmark.c
           ${SRC_ROOT}/src/asr/asr_interface.c
           ${SRC_ROOT}/src/asr/asr_vosk.c
   INCLUDES ${SRC_ROOT}/whisper.cpp/include
   LIBS    dawn_common_asr vosk whisper sndfile ${JSONC_LIBRARIES} ${CUDA_LIBS}
   LABEL   hardware)

dawn_add_unit_test(test_vad_models
   SOURCES test_vad_models.c
   INCLUDES /usr/local/include/onnxruntime
   LIBS    onnxruntime m
   LABEL   hardware)

dawn_add_unit_test(test_vad_api
   SOURCES test_vad_api.c
   INCLUDES /usr/local/include/onnxruntime
   LIBS    dawn_common_vad onnxruntime m ${JSONC_LIBRARIES}
   LABEL   hardware)

dawn_add_unit_test(test_asr_concurrent
   SOURCES test_asr_concurrent.c
           ${SRC_ROOT}/src/asr/asr_interface.c
           ${SRC_ROOT}/src/ui/metrics.c
   INCLUDES ${SRC_ROOT}/whisper.cpp/include
   LIBS    dawn_common_asr whisper pthread m ${JSONC_LIBRARIES} ${CUDA_LIBS}
   LABEL   hardware)

dawn_add_unit_test(test_summarizer_quality
   SOURCES test_summarizer_quality.c
           ${SRC_ROOT}/src/tools/tfidf_summarizer.c
           ${SRC_ROOT}/src/tools/url_fetcher.c
           ${SRC_ROOT}/src/tools/html_parser.c
           ${SRC_ROOT}/src/config/config_parser.c
           ${SRC_ROOT}/src/config/config_defaults.c
           ${SRC_ROOT}/src/config/config_env.c
           ${SRC_ROOT}/src/tools/toml.c
   LIBS    dawn_common ${CURL_LIBRARIES} ${JSONC_LIBRARIES} m
   LABEL   hardware)

# =============================================================================
# Benchmarks (not run via ctest — driven by external Python harnesses)
# =============================================================================

dawn_add_unit_test(bench_retrieval
   NO_TEST
   SOURCES ${SRC_ROOT}/benchmarks/bench_retrieval.c
           ${SRC_ROOT}/benchmarks/bench_retrieval_stub.c
           ${SRC_ROOT}/benchmarks/bench_memory_pipeline.c
           ${SRC_ROOT}/src/tools/document_db.c
           ${SRC_ROOT}/src/auth/auth_db_ids.c
           ${SRC_ROOT}/src/tools/document_db_embeddings.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/core/time_query_parser.c
           ${SRC_ROOT}/src/core/embedding_engine.c
           ${SRC_ROOT}/src/memory/memory_embed_tokenizer.c
           ${SRC_ROOT}/src/memory/memory_embed_unicode.c
           ${SRC_ROOT}/src/memory/memory_embed_onnx.c
           ${SRC_ROOT}/src/memory/memory_embed_ollama.c
           ${SRC_ROOT}/src/memory/memory_embed_openai.c
           # Memory-pipeline mode TUs (dormant unless --memory-pipeline is passed)
           ${AUTH_DB_SOURCES}
           ${SRC_ROOT}/src/memory/memory_callback.c
           ${SRC_ROOT}/src/memory/contacts_db.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_db_facts.c
           ${SRC_ROOT}/src/memory/memory_db_summaries.c
           ${SRC_ROOT}/src/memory/memory_db_prefs.c
           ${SRC_ROOT}/src/memory/memory_db_entities.c
           ${SRC_ROOT}/src/memory/memory_db_relations.c
           ${SRC_ROOT}/src/memory/memory_db_provenance.c
           # memory_bm25.c + memory_stem.c arrive via AUTH_DB_SOURCES below.
           ${SRC_ROOT}/src/memory/memory_source_dedup.c
           ${SRC_ROOT}/src/memory/memory_score_floor.c
           ${SRC_ROOT}/src/memory/memory_graph_retrieval.c
           ${SRC_ROOT}/src/memory/memory_predicate_dedup.c
           ${SRC_ROOT}/src/memory/memory_embeddings.c
           ${SRC_ROOT}/src/memory/memory_embeddings_entity.c
           ${SRC_ROOT}/src/memory/memory_terms.c
           ${SRC_ROOT}/src/memory/memory_embed_backfill.c
           ${SRC_ROOT}/src/memory/memory_extraction.c
           ${SRC_ROOT}/src/memory/memory_extraction_input.c
           ${SRC_ROOT}/src/memory/memory_note_guard.c
           ${SRC_ROOT}/src/memory/memory_fact_search.c
           ${SRC_ROOT}/src/core/memory_filter.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
           ${SRC_ROOT}/src/llm/llm_interface.c
           ${SRC_ROOT}/src/llm/llm_turn_result.c
           ${SRC_ROOT}/src/llm/llm_local_provider.c
           ${SRC_ROOT}/src/llm/llm_claude.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_claude_blocks.c
           ${SRC_ROOT}/src/llm/llm_claude_betas.c
           ${SRC_ROOT}/src/llm/llm_claude_route.c
           ${SRC_ROOT}/src/llm/llm_claude_binding.c
           ${SRC_ROOT}/src/llm/llm_claude_format.c
           ${SRC_ROOT}/src/llm/llm_claude_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
           ${SRC_ROOT}/src/llm/llm_capabilities.c
           ${SRC_ROOT}/src/llm/llm_model_family.c
           ${SRC_ROOT}/src/llm/llm_model_version.c
           ${SRC_ROOT}/src/llm/llm_openai.c
           ${SRC_ROOT}/src/llm/llm_openai_cache.c
           ${SRC_ROOT}/src/llm/llm_openai_chat_completions.c
           ${SRC_ROOT}/src/llm/llm_key_tag.c
           ${SRC_ROOT}/src/core/crypto_store.c
           ${SRC_ROOT}/src/llm/llm_openai_history.c
           ${SRC_ROOT}/src/llm/llm_streaming.c
           ${SRC_ROOT}/src/llm/llm_reasoning_details.c
           ${SRC_ROOT}/src/llm/llm_tools.c
           ${SRC_ROOT}/src/core/tool_call_policy.c
           ${SRC_ROOT}/src/core/tool_call_challenge.c
           ${SRC_ROOT}/src/llm/llm_tools_reply_code.c
           ${SRC_ROOT}/src/core/reply_code.c
           ${SRC_ROOT}/src/core/pending_slots.c
           ${SRC_ROOT}/src/llm/llm_tools_filter.c
           ${SRC_ROOT}/src/llm/llm_tools_results.c
           ${SRC_ROOT}/src/llm/llm_tool_images.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/core/image_rehydrate.c
           ${SRC_ROOT}/src/image_store.c
           ${SRC_ROOT}/src/blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
           ${SRC_ROOT}/src/llm/llm_tools_dup.c
           ${SRC_ROOT}/src/llm/llm_rate_limit.c
           ${SRC_ROOT}/src/llm/llm_openai_responses.c
           ${SRC_ROOT}/src/llm/llm_openai_responses_input.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_context.c
           ${SRC_ROOT}/src/llm/llm_compaction_range.c
           ${SRC_ROOT}/src/llm/llm_compaction.c
           ${SRC_ROOT}/src/llm/llm_models_toml.c
           ${SRC_ROOT}/src/llm/llm_cache_monitor.c
           ${SRC_ROOT}/src/llm/llm_pricing.c
           ${SRC_ROOT}/src/llm/llm_command_parser.c
           ${SRC_ROOT}/src/llm/llm_tool_loop.c
           ${SRC_ROOT}/src/llm/llm_tool_views_plan.c
           ${SRC_ROOT}/src/llm/llm_tool_views_apply.c
           ${SRC_ROOT}/src/llm/llm_tool_view.c
           ${SRC_ROOT}/src/llm/llm_tool_view_path.c
           ${SRC_ROOT}/src/llm/llm_history_rows.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/sse_parser.c
           ${SRC_ROOT}/src/core/iso8601.c
           ${SRC_ROOT}/src/core/ocp_helpers.c
           ${SRC_ROOT}/src/tools/tool_registry.c
           ${SRC_ROOT}/src/config/config_parser.c
           ${SRC_ROOT}/src/config/config_defaults.c
           ${SRC_ROOT}/src/config/config_validate.c
           ${SRC_ROOT}/src/config/config_env.c
           ${SRC_ROOT}/src/tools/toml.c
   INCLUDES /usr/local/include/onnxruntime
   LIBS    dawn_common sqlite3 pthread m onnxruntime ssl crypto stemmer ${JSONC_LIBRARIES} sodium
           ${CURL_LIBRARIES} ${JSONC_LIBRARIES})

# The remember tool end to end (memory_callback.c on a real schema), linked like
# the retrieval bench: where a remembered fact's source comes from, and the
# refusal when a turn has more facts waiting than it can track.  It never
# embeds, so the ONNX provider is a stub: CI runners have no onnxruntime.
dawn_add_unit_test(test_memory_remember
   SOURCES test_memory_remember.c test_memory_remember_stub.c
           ${SRC_ROOT}/benchmarks/bench_retrieval_stub.c
           ${SRC_ROOT}/src/tools/document_db.c
           ${SRC_ROOT}/src/auth/auth_db_ids.c
           ${SRC_ROOT}/src/tools/document_db_embeddings.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/core/time_query_parser.c
           ${SRC_ROOT}/src/core/embedding_engine.c
           ${SRC_ROOT}/src/memory/memory_embed_ollama.c
           ${SRC_ROOT}/src/memory/memory_embed_openai.c
           # Memory-pipeline mode TUs (dormant unless --memory-pipeline is passed)
           ${AUTH_DB_SOURCES}
           ${SRC_ROOT}/src/memory/memory_callback.c
           ${SRC_ROOT}/src/memory/contacts_db.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_db_facts.c
           ${SRC_ROOT}/src/memory/memory_db_summaries.c
           ${SRC_ROOT}/src/memory/memory_db_prefs.c
           ${SRC_ROOT}/src/memory/memory_db_entities.c
           ${SRC_ROOT}/src/memory/memory_db_relations.c
           ${SRC_ROOT}/src/memory/memory_db_provenance.c
           # memory_bm25.c + memory_stem.c arrive via AUTH_DB_SOURCES below.
           ${SRC_ROOT}/src/memory/memory_source_dedup.c
           ${SRC_ROOT}/src/memory/memory_score_floor.c
           ${SRC_ROOT}/src/memory/memory_graph_retrieval.c
           ${SRC_ROOT}/src/memory/memory_predicate_dedup.c
           ${SRC_ROOT}/src/memory/memory_embeddings.c
           ${SRC_ROOT}/src/memory/memory_embeddings_entity.c
           ${SRC_ROOT}/src/memory/memory_terms.c
           ${SRC_ROOT}/src/memory/memory_embed_backfill.c
           ${SRC_ROOT}/src/memory/memory_extraction.c
           ${SRC_ROOT}/src/memory/memory_extraction_input.c
           ${SRC_ROOT}/src/memory/memory_note_guard.c
           ${SRC_ROOT}/src/memory/memory_fact_search.c
           ${SRC_ROOT}/src/core/memory_filter.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
           ${SRC_ROOT}/src/llm/llm_interface.c
           ${SRC_ROOT}/src/llm/llm_turn_result.c
           ${SRC_ROOT}/src/llm/llm_local_provider.c
           ${SRC_ROOT}/src/llm/llm_claude.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_claude_blocks.c
           ${SRC_ROOT}/src/llm/llm_claude_betas.c
           ${SRC_ROOT}/src/llm/llm_claude_route.c
           ${SRC_ROOT}/src/llm/llm_claude_binding.c
           ${SRC_ROOT}/src/llm/llm_claude_format.c
           ${SRC_ROOT}/src/llm/llm_claude_tools.c
           ${SRC_ROOT}/src/llm/llm_tool_defs.c
           ${SRC_ROOT}/src/llm/llm_capabilities.c
           ${SRC_ROOT}/src/llm/llm_model_family.c
           ${SRC_ROOT}/src/llm/llm_model_version.c
           ${SRC_ROOT}/src/llm/llm_openai.c
           ${SRC_ROOT}/src/llm/llm_openai_cache.c
           ${SRC_ROOT}/src/llm/llm_openai_chat_completions.c
           ${SRC_ROOT}/src/llm/llm_key_tag.c
           ${SRC_ROOT}/src/core/crypto_store.c
           ${SRC_ROOT}/src/llm/llm_openai_history.c
           ${SRC_ROOT}/src/llm/llm_streaming.c
           ${SRC_ROOT}/src/llm/llm_reasoning_details.c
           ${SRC_ROOT}/src/llm/llm_tools.c
           ${SRC_ROOT}/src/core/tool_call_policy.c
           ${SRC_ROOT}/src/core/tool_call_challenge.c
           ${SRC_ROOT}/src/llm/llm_tools_reply_code.c
           ${SRC_ROOT}/src/core/reply_code.c
           ${SRC_ROOT}/src/core/pending_slots.c
           ${SRC_ROOT}/src/llm/llm_tools_filter.c
           ${SRC_ROOT}/src/llm/llm_tools_results.c
           ${SRC_ROOT}/src/llm/llm_tool_images.c
           ${SRC_ROOT}/src/llm/llm_tool_images_render.c
           ${SRC_ROOT}/src/core/image_rehydrate.c
           ${SRC_ROOT}/src/image_store.c
           ${SRC_ROOT}/src/blob_store.c
           ${SRC_ROOT}/src/blob_store_util.c
           ${SRC_ROOT}/src/llm/llm_tools_dup.c
           ${SRC_ROOT}/src/llm/llm_rate_limit.c
           ${SRC_ROOT}/src/llm/llm_openai_responses.c
           ${SRC_ROOT}/src/llm/llm_openai_responses_input.c
           ${SRC_ROOT}/src/llm/llm_claude_parts.c
           ${SRC_ROOT}/src/llm/llm_context.c
           ${SRC_ROOT}/src/llm/llm_compaction_range.c
           ${SRC_ROOT}/src/llm/llm_compaction.c
           ${SRC_ROOT}/src/llm/llm_models_toml.c
           ${SRC_ROOT}/src/llm/llm_cache_monitor.c
           ${SRC_ROOT}/src/llm/llm_pricing.c
           ${SRC_ROOT}/src/llm/llm_command_parser.c
           ${SRC_ROOT}/src/llm/llm_tool_loop.c
           ${SRC_ROOT}/src/llm/llm_tool_views_plan.c
           ${SRC_ROOT}/src/llm/llm_tool_views_apply.c
           ${SRC_ROOT}/src/llm/llm_tool_view.c
           ${SRC_ROOT}/src/llm/llm_tool_view_path.c
           ${SRC_ROOT}/src/llm/llm_history_rows.c
           ${SRC_ROOT}/src/llm/llm_turn_blocks_stored.c
           ${SRC_ROOT}/src/llm/sse_parser.c
           ${SRC_ROOT}/src/core/iso8601.c
           ${SRC_ROOT}/src/core/ocp_helpers.c
           ${SRC_ROOT}/src/tools/tool_registry.c
           ${SRC_ROOT}/src/config/config_parser.c
           ${SRC_ROOT}/src/config/config_defaults.c
           ${SRC_ROOT}/src/config/config_validate.c
           ${SRC_ROOT}/src/config/config_env.c
           ${SRC_ROOT}/src/tools/toml.c
   LIBS    dawn_common sqlite3 pthread m ssl crypto stemmer ${JSONC_LIBRARIES} sodium
           unity ${CURL_LIBRARIES} ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(bench_contradiction
   NO_TEST
   SOURCES ${SRC_ROOT}/benchmarks/bench_contradiction.c
           ${SRC_ROOT}/benchmarks/bench_contradiction_stub.c
           ${SRC_ROOT}/src/core/embedding_engine.c
           ${SRC_ROOT}/src/memory/memory_embed_tokenizer.c
           ${SRC_ROOT}/src/memory/memory_embed_unicode.c
           ${SRC_ROOT}/src/memory/memory_embed_onnx.c
           ${SRC_ROOT}/src/memory/memory_embed_ollama.c
           ${SRC_ROOT}/src/memory/memory_embed_openai.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
   INCLUDES /usr/local/include/onnxruntime
   LIBS    dawn_common pthread m onnxruntime
           ${CURL_LIBRARIES} ${JSONC_LIBRARIES})

# Phase 1i.C focus-pipeline bench binary.  Standalone driver that wraps
# build_focus_block end-to-end against a fake-adapter synthetic memory
# pool, marshals the result to JSON stdout for the multi-model fix-rate
# probe (benchmarks/bench_focus_fix_rate.py).  No SQLite linkage per
# coordinator pin C2 — keeps the link surface lean and consistent with
# 1i.B's test_focus_probe shape.  Embedded synth_embed stub provides
# deterministic FNV-derived float vectors; webui_broadcast hook captures
# the result for marshalling before build_focus_block frees it.
dawn_add_unit_test(bench_focus_pipeline
   NO_TEST
   SOURCES ${SRC_ROOT}/benchmarks/bench_focus_pipeline.c
           ${SRC_ROOT}/src/core/focus/focus_source.c
           ${SRC_ROOT}/src/core/focus/focus_recency.c
           ${SRC_ROOT}/src/core/focus/focus_candidate_helpers.c
           ${SRC_ROOT}/src/core/focus/focus_dominant_token.c
           ${SRC_ROOT}/src/core/memory_filter.c
           ${SRC_ROOT}/src/core/focus/focus_incremental.c
           ${SRC_ROOT}/src/core/strbuf.c
           ${SRC_ROOT}/src/core/build_focus_block.c
           ${SRC_ROOT}/src/llm/llm_context_text.c
           ${SRC_ROOT}/src/llm/llm_history_kind.c
   LIBS    dawn_common pthread m stemmer
           ${JSONC_LIBRARIES})

# =============================================================================
# Drift tests — shell-script based (no compile target)
# =============================================================================
# Cross-source-language invariants that can't be enforced by C unit tests.
# Architecture-reviewer flagged the swap-eligible entity-type list as a
# drift risk on the entity-merge Path B + Path C ship — same predicate
# expressed in both C (memory_db_alias_writes.c) and JS (memory_aliases.js).
# The drift script extracts both lists, compares, and fails the build if
# they diverge.
add_test(NAME check_swap_eligible_types_mirror
         COMMAND bash ${SRC_ROOT}/tests/check_swap_eligible_types_mirror.sh
         WORKING_DIRECTORY ${SRC_ROOT})
set_tests_properties(check_swap_eligible_types_mirror PROPERTIES LABELS "ci")

# SECTION_CATEGORIES in schema.js is the settings render gate — a section absent
# from it is invisible in the UI no matter how correctly it is wired everywhere
# else.  `[attention]` and `[jobs]` both shipped that way before this guard.
add_test(NAME check_settings_sections_rendered
         COMMAND bash ${SRC_ROOT}/tests/check_settings_sections_rendered.sh
         WORKING_DIRECTORY ${SRC_ROOT})
set_tests_properties(check_settings_sections_rendered PROPERTIES LABELS "ci")

# The tests that load models.toml read the copy compiled in (models_toml_builtin.h).
add_dependencies(bench_retrieval models_toml_builtin)
add_dependencies(test_memory_remember models_toml_builtin)
add_dependencies(test_llm_models_toml models_toml_builtin)
