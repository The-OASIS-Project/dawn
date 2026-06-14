# DawnTools.cmake - Tool Configuration and Compile-time Exclusion
#
# This module provides CMake options for enabling/disabling individual tools.
# When a tool is disabled, its source files are not compiled and the
# DAWN_ENABLE_<TOOL>_TOOL define is not set, allowing compile-time exclusion.
#
# Usage in CMakeLists.txt:
#   include(cmake/DawnTools.cmake)
#   # TOOL_SOURCES variable will contain enabled tool source files
#
# To disable a tool at configure time:
#   cmake -DDAWN_ENABLE_SHUTDOWN_TOOL=OFF ..

# =============================================================================
# Tool Enable/Disable Options
# =============================================================================

# Plugin tools (can be disabled)
option(DAWN_ENABLE_SHUTDOWN_TOOL "Enable shutdown command tool" ON)
option(DAWN_ENABLE_MUSIC_TOOL "Enable music playback tool" ON)
option(DAWN_ENABLE_CALCULATOR_TOOL "Enable calculator tool" ON)
option(DAWN_ENABLE_WEATHER_TOOL "Enable weather service tool" ON)
option(DAWN_ENABLE_SEARCH_TOOL "Enable web search tool" ON)
option(DAWN_ENABLE_URL_TOOL "Enable URL fetcher tool" ON)
option(DAWN_ENABLE_HOMEASSISTANT_TOOL "Enable Home Assistant integration" ON)
option(DAWN_ENABLE_SMARTTHINGS_TOOL "Enable SmartThings integration" OFF)
option(DAWN_ENABLE_MEMORY_TOOL "Enable memory/recall system" ON)
option(DAWN_ENABLE_DATETIME_TOOL "Enable date/time tools" ON)
option(DAWN_ENABLE_VOLUME_TOOL "Enable volume control tool" ON)
option(DAWN_ENABLE_LLM_STATUS_TOOL "Enable LLM status/switch tool" ON)
option(DAWN_ENABLE_SWITCH_LLM_TOOL "Enable switch_llm meta-tool" ON)
option(DAWN_ENABLE_RESET_CONVERSATION_TOOL "Enable reset_conversation tool" ON)
option(DAWN_ENABLE_VIEWING_TOOL "Enable viewing/vision tool (MQTT)" ON)
option(DAWN_ENABLE_HUD_TOOLS "Enable HUD control tools (MQTT)" ON)
option(DAWN_ENABLE_AUDIO_TOOLS "Enable voice amplifier and audio device tools" ON)
option(DAWN_ENABLE_SCHEDULER_TOOL "Enable scheduler/timer/alarm/reminder tool" ON)
option(DAWN_ENABLE_TTS_TOOL "Enable text-to-speech command tool" ON)
option(DAWN_ENABLE_DOCUMENT_SEARCH_TOOL "Enable RAG document search tool" ON)
option(DAWN_ENABLE_CALENDAR_TOOL "Enable CalDAV calendar integration" ON)
option(DAWN_ENABLE_EMAIL_TOOL "Enable IMAP/SMTP email integration" ON)
option(DAWN_ENABLE_SFX_TOOL "Enable sound effect playback tool" ON)
option(DAWN_ENABLE_RENDER_VISUAL_TOOL "Enable visual rendering tool (SVG/HTML diagrams)" ON)
option(DAWN_ENABLE_PHONE_TOOL "Enable phone call and SMS tool (requires ECHO daemon)" ON)
option(DAWN_ENABLE_IMAGE_SEARCH_TOOL "Enable image search tool (requires SearXNG + image store)" ON)
option(DAWN_ENABLE_CONTEXT_EXPAND_TOOL "Enable context expansion tool (LCM Phase 3)" ON)

# =============================================================================
# Mutual Exclusion: Home Assistant and SmartThings
# =============================================================================
if(DAWN_ENABLE_HOMEASSISTANT_TOOL AND DAWN_ENABLE_SMARTTHINGS_TOOL)
    message(FATAL_ERROR "Cannot enable both Home Assistant and SmartThings tools. They are mutually exclusive.")
endif()

# =============================================================================
# Tool Registry (always included)
# =============================================================================

set(TOOL_REGISTRY_SOURCES
    src/tools/tool_registry.c
    src/tools/instruction_loader.c
    src/tools/plan_executor.c
    src/tools/plan_executor_tool.c
)

# =============================================================================
# Conditional Source Files and Definitions
# =============================================================================

set(TOOL_SOURCES ${TOOL_REGISTRY_SOURCES})

# Shutdown Tool
if(DAWN_ENABLE_SHUTDOWN_TOOL)
    add_definitions(-DDAWN_ENABLE_SHUTDOWN_TOOL)
    list(APPEND TOOL_SOURCES src/tools/shutdown_tool.c)
    message(STATUS "DAWN: Shutdown tool ENABLED")
else()
    message(STATUS "DAWN: Shutdown tool DISABLED")
endif()

# Music Tool
if(DAWN_ENABLE_MUSIC_TOOL)
    add_definitions(-DDAWN_ENABLE_MUSIC_TOOL)
    list(APPEND TOOL_SOURCES src/tools/music_tool.c)
    message(STATUS "DAWN: Music tool ENABLED")
else()
    message(STATUS "DAWN: Music tool DISABLED")
endif()

# Calculator Tool
if(DAWN_ENABLE_CALCULATOR_TOOL)
    add_definitions(-DDAWN_ENABLE_CALCULATOR_TOOL)
    # Note: calculator.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/calculator_tool.c)
    message(STATUS "DAWN: Calculator tool ENABLED")
else()
    message(STATUS "DAWN: Calculator tool DISABLED")
endif()

# Weather Tool
if(DAWN_ENABLE_WEATHER_TOOL)
    add_definitions(-DDAWN_ENABLE_WEATHER_TOOL)
    # Note: weather_service.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/weather_tool.c)
    message(STATUS "DAWN: Weather tool ENABLED")
else()
    message(STATUS "DAWN: Weather tool DISABLED")
endif()

# Search Tool
if(DAWN_ENABLE_SEARCH_TOOL)
    add_definitions(-DDAWN_ENABLE_SEARCH_TOOL)
    # Note: web_search.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/search_tool.c)
    message(STATUS "DAWN: Search tool ENABLED")
else()
    message(STATUS "DAWN: Search tool DISABLED")
endif()

# URL Fetcher Tool
if(DAWN_ENABLE_URL_TOOL)
    add_definitions(-DDAWN_ENABLE_URL_TOOL)
    # Note: url_fetcher.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/url_tool.c)
    message(STATUS "DAWN: URL fetcher tool ENABLED")
else()
    message(STATUS "DAWN: URL fetcher tool DISABLED")
endif()

# SmartThings Tool
if(DAWN_ENABLE_SMARTTHINGS_TOOL)
    add_definitions(-DDAWN_ENABLE_SMARTTHINGS_TOOL)
    # Note: smartthings_service.c already exists, we add a wrapper
    list(APPEND TOOL_SOURCES src/tools/smartthings_tool.c src/tools/smartthings_service.c)
    message(STATUS "DAWN: SmartThings tool ENABLED")
else()
    message(STATUS "DAWN: SmartThings tool DISABLED")
endif()

# Home Assistant Tool (mutually exclusive with SmartThings)
if(DAWN_ENABLE_HOMEASSISTANT_TOOL)
    add_definitions(-DDAWN_ENABLE_HOMEASSISTANT_TOOL)
    list(APPEND TOOL_SOURCES src/tools/homeassistant_tool.c src/tools/homeassistant_service.c)
    message(STATUS "DAWN: Home Assistant tool ENABLED")
else()
    message(STATUS "DAWN: Home Assistant tool DISABLED")
endif()

# Memory Tool
if(DAWN_ENABLE_MEMORY_TOOL)
    add_definitions(-DDAWN_ENABLE_MEMORY_TOOL)
    list(APPEND TOOL_SOURCES
        src/tools/memory_tool.c
        src/memory/contacts_db.c)
    message(STATUS "DAWN: Memory tool ENABLED")
else()
    message(STATUS "DAWN: Memory tool DISABLED")
endif()

# DateTime Tools (date and time)
if(DAWN_ENABLE_DATETIME_TOOL)
    add_definitions(-DDAWN_ENABLE_DATETIME_TOOL)
    list(APPEND TOOL_SOURCES src/tools/datetime_tool.c)
    message(STATUS "DAWN: DateTime tools ENABLED")
else()
    message(STATUS "DAWN: DateTime tools DISABLED")
endif()

# Volume Tool
if(DAWN_ENABLE_VOLUME_TOOL)
    add_definitions(-DDAWN_ENABLE_VOLUME_TOOL)
    list(APPEND TOOL_SOURCES src/tools/volume_tool.c)
    message(STATUS "DAWN: Volume tool ENABLED")
else()
    message(STATUS "DAWN: Volume tool DISABLED")
endif()

# LLM Status Tool
if(DAWN_ENABLE_LLM_STATUS_TOOL)
    add_definitions(-DDAWN_ENABLE_LLM_STATUS_TOOL)
    list(APPEND TOOL_SOURCES src/tools/llm_status_tool.c)
    message(STATUS "DAWN: LLM Status tool ENABLED")
else()
    message(STATUS "DAWN: LLM Status tool DISABLED")
endif()

# Switch LLM Tool
if(DAWN_ENABLE_SWITCH_LLM_TOOL)
    add_definitions(-DDAWN_ENABLE_SWITCH_LLM_TOOL)
    list(APPEND TOOL_SOURCES src/tools/switch_llm_tool.c)
    message(STATUS "DAWN: Switch LLM tool ENABLED")
else()
    message(STATUS "DAWN: Switch LLM tool DISABLED")
endif()

# Reset Conversation Tool
if(DAWN_ENABLE_RESET_CONVERSATION_TOOL)
    add_definitions(-DDAWN_ENABLE_RESET_CONVERSATION_TOOL)
    list(APPEND TOOL_SOURCES src/tools/reset_conversation_tool.c)
    message(STATUS "DAWN: Reset Conversation tool ENABLED")
else()
    message(STATUS "DAWN: Reset Conversation tool DISABLED")
endif()

# Viewing Tool (MQTT-based vision)
if(DAWN_ENABLE_VIEWING_TOOL)
    add_definitions(-DDAWN_ENABLE_VIEWING_TOOL)
    list(APPEND TOOL_SOURCES src/tools/viewing_tool.c)
    message(STATUS "DAWN: Viewing tool ENABLED")
else()
    message(STATUS "DAWN: Viewing tool DISABLED")
endif()

# HUD Tools (MQTT-based: hud_control, hud_mode, faceplate, recording, visual_offset)
if(DAWN_ENABLE_HUD_TOOLS)
    add_definitions(-DDAWN_ENABLE_HUD_TOOLS)
    list(APPEND TOOL_SOURCES src/tools/hud_tools.c)
    message(STATUS "DAWN: HUD tools ENABLED")
else()
    message(STATUS "DAWN: HUD tools DISABLED")
endif()

# Audio Tools (voice_amplifier, audio_device)
if(DAWN_ENABLE_AUDIO_TOOLS)
    add_definitions(-DDAWN_ENABLE_AUDIO_TOOLS)
    list(APPEND TOOL_SOURCES src/tools/audio_tools.c)
    message(STATUS "DAWN: Audio tools ENABLED")
else()
    message(STATUS "DAWN: Audio tools DISABLED")
endif()

# Scheduler Tool
if(DAWN_ENABLE_SCHEDULER_TOOL)
    add_definitions(-DDAWN_ENABLE_SCHEDULER_TOOL)
    list(APPEND TOOL_SOURCES src/tools/scheduler_tool.c)
    message(STATUS "DAWN: Scheduler tool ENABLED")
else()
    message(STATUS "DAWN: Scheduler tool DISABLED")
endif()

# TTS Tool
if(DAWN_ENABLE_TTS_TOOL)
    add_definitions(-DDAWN_ENABLE_TTS_TOOL)
    list(APPEND TOOL_SOURCES src/tools/tts_tool.c)
    message(STATUS "DAWN: TTS tool ENABLED")
else()
    message(STATUS "DAWN: TTS tool DISABLED")
endif()

# Document Search Tool (RAG)
if(DAWN_ENABLE_DOCUMENT_SEARCH_TOOL)
    add_definitions(-DDAWN_ENABLE_DOCUMENT_SEARCH_TOOL)
    list(APPEND TOOL_SOURCES src/tools/document_search.c src/tools/document_grep_tool.c src/tools/document_read.c src/tools/document_index_tool.c src/tools/document_manage_tool.c src/tools/document_edit_ops.c)
    message(STATUS "DAWN: Document Search tool ENABLED")
else()
    message(STATUS "DAWN: Document Search tool DISABLED")
endif()

# Context Expand Tool (LCM Phase 3 — retrieve compacted messages)
if(DAWN_ENABLE_CONTEXT_EXPAND_TOOL)
    add_definitions(-DDAWN_ENABLE_CONTEXT_EXPAND_TOOL)
    list(APPEND TOOL_SOURCES src/tools/context_expand_tool.c)
    message(STATUS "DAWN: Context Expand tool ENABLED")
else()
    message(STATUS "DAWN: Context Expand tool DISABLED")
endif()

# Calendar Tool (CalDAV) — requires libxml2 for XML parsing and libical for iCalendar
if(DAWN_ENABLE_CALENDAR_TOOL)
    if(NOT LIBXML2_FOUND)
        message(WARNING "DAWN: Calendar tool requires libxml2 — disabling (install libxml2-dev)")
        set(DAWN_ENABLE_CALENDAR_TOOL OFF)
    elseif(NOT LIBICAL_FOUND)
        message(WARNING "DAWN: Calendar tool requires libical — disabling (install libical-dev)")
        set(DAWN_ENABLE_CALENDAR_TOOL OFF)
    else()
        add_definitions(-DDAWN_ENABLE_CALENDAR_TOOL)
        list(APPEND TOOL_SOURCES
            src/tools/calendar_tool.c
            src/tools/calendar_service.c
            src/tools/calendar_db.c
            src/tools/caldav_client.c
            src/tools/oauth_client.c)
        message(STATUS "DAWN: Calendar tool ENABLED")
    endif()
else()
    message(STATUS "DAWN: Calendar tool DISABLED")
endif()

# Email Tool (IMAP/SMTP)
if(DAWN_ENABLE_EMAIL_TOOL)
    add_definitions(-DDAWN_ENABLE_EMAIL_TOOL)
    list(APPEND TOOL_SOURCES
        src/tools/email_tool.c
        src/tools/email_service.c
        src/tools/email_db.c
        src/tools/email_client.c
        src/tools/gmail_client.c
        src/webui/webui_email.c)
    # oauth_client.c may already be included by calendar tool
    if(NOT DAWN_ENABLE_CALENDAR_TOOL)
        list(APPEND TOOL_SOURCES src/tools/oauth_client.c)
    endif()
    message(STATUS "DAWN: Email tool ENABLED")
else()
    message(STATUS "DAWN: Email tool DISABLED")
endif()

# Shared OAuth WebUI handler (needed by calendar or email for Google OAuth).
# It's a WebUI surface (conn_require_auth / send_json_response), so only compile
# it when WebUI is on — otherwise the local / ci presets fail to link.
if((DAWN_ENABLE_CALENDAR_TOOL OR DAWN_ENABLE_EMAIL_TOOL) AND ENABLE_WEBUI)
    list(APPEND TOOL_SOURCES src/webui/webui_oauth.c)
endif()

# SFX Tool (sound effect playback)
if(DAWN_ENABLE_SFX_TOOL)
    add_definitions(-DDAWN_ENABLE_SFX_TOOL)
    list(APPEND TOOL_SOURCES src/tools/sfx_tool.c)
    message(STATUS "DAWN: SFX tool ENABLED")
else()
    message(STATUS "DAWN: SFX tool DISABLED")
endif()

# Render Visual Tool (SVG/HTML diagrams via two-step instruction loader)
if(DAWN_ENABLE_RENDER_VISUAL_TOOL)
    add_definitions(-DDAWN_ENABLE_RENDER_VISUAL_TOOL)
    list(APPEND TOOL_SOURCES src/tools/render_visual_tool.c)
    message(STATUS "DAWN: Render Visual tool ENABLED")
else()
    message(STATUS "DAWN: Render Visual tool DISABLED")
endif()

# Phone Tool (calls and SMS via ECHO modem daemon)
if(DAWN_ENABLE_PHONE_TOOL)
    add_definitions(-DDAWN_ENABLE_PHONE_TOOL)
    list(APPEND TOOL_SOURCES
        src/tools/phone_tool.c
        src/tools/phone_service.c
        src/tools/phone_db.c)
    message(STATUS "DAWN: Phone tool ENABLED")
else()
    message(STATUS "DAWN: Phone tool DISABLED")
endif()

# Image Search Tool (SearXNG image search + image store caching)
if(DAWN_ENABLE_IMAGE_SEARCH_TOOL)
    add_definitions(-DDAWN_ENABLE_IMAGE_SEARCH_TOOL)
    list(APPEND TOOL_SOURCES src/tools/image_search_tool.c)
    message(STATUS "DAWN: Image Search tool ENABLED")
else()
    message(STATUS "DAWN: Image Search tool DISABLED")
endif()

# MCP Bridge (coding harness — HTTP+SSE client to operator-launched MCP servers)
option(DAWN_ENABLE_MCP_BRIDGE_TOOL "Enable MCP bridge tool (Phase 1: HTTP+SSE only)" OFF)
if(DAWN_ENABLE_MCP_BRIDGE_TOOL)
    add_definitions(-DDAWN_ENABLE_MCP_BRIDGE_TOOL)
    list(APPEND TOOL_SOURCES
        src/tools/mcp_bridge_tool.c
        src/tools/mcp_client.c
        src/tools/mcp_transport_http_sse.c
        src/tools/mcp_bridge_schema.c
        src/auth/auth_db_mcp.c
        src/auth/admin_socket_mcp.c)
    message(STATUS "DAWN: MCP bridge tool ENABLED")
else()
    message(STATUS "DAWN: MCP bridge tool DISABLED")
endif()

# Code Projects (coding harness — clone + index repos via libgit2 + MCP bridge)
option(DAWN_ENABLE_CODE_PROJECTS "Enable code projects subsystem (requires MCP bridge + libgit2)" OFF)
if(DAWN_ENABLE_CODE_PROJECTS)
    if(NOT DAWN_ENABLE_MCP_BRIDGE_TOOL)
        message(FATAL_ERROR "DAWN_ENABLE_CODE_PROJECTS requires DAWN_ENABLE_MCP_BRIDGE_TOOL")
    endif()
    add_definitions(-DDAWN_ENABLE_CODE_PROJECTS)
    list(APPEND TOOL_SOURCES
        src/tools/code_graph_provider_cbm.c
        src/tools/code_project_db.c
        src/tools/code_project_git.c
        src/tools/code_project_namemap.c
        src/tools/code_project_service.c
        src/tools/code_project_tool.c
        src/auth/admin_socket_code_project.c)
    # WebUI handlers for the Coding popover (needs the WebUI layer). The strong
    # code_project_broadcast_status_changed override lives in webui_broadcasts.c
    # (always compiled with the WebUI), gated there by DAWN_ENABLE_CODE_PROJECTS.
    if(ENABLE_WEBUI)
        list(APPEND TOOL_SOURCES src/webui/webui_code_projects.c)
    endif()
    # CMakeLists.txt links ${LIBGIT2_LIBRARIES} when this is ON.
    message(STATUS "DAWN: Code projects subsystem ENABLED")
else()
    message(STATUS "DAWN: Code projects subsystem DISABLED")
endif()

# =============================================================================
# Summary
# =============================================================================

list(LENGTH TOOL_SOURCES TOOL_COUNT)
message(STATUS "DAWN: ${TOOL_COUNT} tool source files configured")
