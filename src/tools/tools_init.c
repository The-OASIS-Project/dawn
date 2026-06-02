/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Tools Initialization - Central registration point for all modular tools
 */

#include "tools/tools_init.h"

#include "dawn_error.h"
#include "logging.h"

#ifdef DAWN_ENABLE_MCP_BRIDGE_TOOL
#include "tools/mcp_bridge.h"
#endif

#ifdef DAWN_ENABLE_CODE_PROJECTS
#include "tools/code_project_service.h"
#include "tools/code_project_tool.h"
#endif

/* ========== Tool Headers (conditionally included) ========== */

#ifdef DAWN_ENABLE_SHUTDOWN_TOOL
#include "tools/shutdown_tool.h"
#endif
#ifdef DAWN_ENABLE_MUSIC_TOOL
#include "tools/music_tool.h"
#endif
#ifdef DAWN_ENABLE_CALCULATOR_TOOL
#include "tools/calculator_tool.h"
#endif
#ifdef DAWN_ENABLE_WEATHER_TOOL
#include "tools/weather_tool.h"
#endif
#ifdef DAWN_ENABLE_SEARCH_TOOL
#include "tools/search_tool.h"
#endif
#ifdef DAWN_ENABLE_URL_TOOL
#include "tools/url_tool.h"
#endif
#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
#include "tools/homeassistant_tool.h"
#endif
#ifdef DAWN_ENABLE_MEMORY_TOOL
#include "tools/memory_tool.h"
#endif
#ifdef DAWN_ENABLE_DATETIME_TOOL
#include "tools/datetime_tool.h"
#endif
#ifdef DAWN_ENABLE_VOLUME_TOOL
#include "tools/volume_tool.h"
#endif
#ifdef DAWN_ENABLE_LLM_STATUS_TOOL
#include "tools/llm_status_tool.h"
#endif
#ifdef DAWN_ENABLE_SWITCH_LLM_TOOL
#include "tools/switch_llm_tool.h"
#endif
#ifdef DAWN_ENABLE_RESET_CONVERSATION_TOOL
#include "tools/reset_conversation_tool.h"
#endif
#ifdef DAWN_ENABLE_VIEWING_TOOL
#include "tools/viewing_tool.h"
#endif
#ifdef DAWN_ENABLE_HUD_TOOLS
#include "tools/hud_tools.h"
#endif
#ifdef DAWN_ENABLE_AUDIO_TOOLS
#include "tools/audio_tools.h"
#endif
#ifdef DAWN_ENABLE_SCHEDULER_TOOL
#include "tools/scheduler_tool.h"
#endif
#ifdef DAWN_ENABLE_TTS_TOOL
#include "tools/tts_tool.h"
#endif
#ifdef DAWN_ENABLE_SFX_TOOL
#include "tools/sfx_tool.h"
#endif
#ifdef DAWN_ENABLE_DOCUMENT_SEARCH_TOOL
#include "tools/document_grep.h"
#include "tools/document_index_tool.h"
#include "tools/document_manage.h"
#include "tools/document_read.h"
#include "tools/document_search.h"
#endif
#ifdef DAWN_ENABLE_CALENDAR_TOOL
#include "tools/calendar_tool.h"
#endif
#ifdef DAWN_ENABLE_EMAIL_TOOL
#include "tools/email_tool.h"
#endif

#ifdef DAWN_ENABLE_RENDER_VISUAL_TOOL
#include "tools/render_visual_tool.h"
#endif
#ifdef DAWN_ENABLE_PHONE_TOOL
#include "tools/phone_tool.h"
#endif
#ifdef DAWN_ENABLE_IMAGE_SEARCH_TOOL
#include "tools/image_search_tool.h"
#endif
#include "tools/messaging_tool.h"
#ifdef DAWN_ENABLE_CONTEXT_EXPAND_TOOL
#include "tools/context_expand_tool.h"
#endif

#include "tools/plan_executor.h"

/* ========== Registration ========== */

int tools_register_all(void) {
   OLOG_INFO("Registering modular tools...");

#ifdef DAWN_ENABLE_SHUTDOWN_TOOL
   if (shutdown_tool_register() != 0) {
      OLOG_WARNING("Failed to register shutdown tool");
   }
#endif

#ifdef DAWN_ENABLE_MUSIC_TOOL
   if (music_tool_register() != 0) {
      OLOG_WARNING("Failed to register music tool");
   }
#endif

#ifdef DAWN_ENABLE_CALCULATOR_TOOL
   if (calculator_tool_register() != 0) {
      OLOG_WARNING("Failed to register calculator tool");
   }
#endif

#ifdef DAWN_ENABLE_WEATHER_TOOL
   if (weather_tool_register() != 0) {
      OLOG_WARNING("Failed to register weather tool");
   }
#endif

#ifdef DAWN_ENABLE_SEARCH_TOOL
   if (search_tool_register() != 0) {
      OLOG_WARNING("Failed to register search tool");
   }
#endif

#ifdef DAWN_ENABLE_URL_TOOL
   if (url_tool_register() != 0) {
      OLOG_WARNING("Failed to register url_fetch tool");
   }
#endif


#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
   if (homeassistant_tool_register() != 0) {
      OLOG_WARNING("Failed to register home_assistant tool");
   }
#endif

#ifdef DAWN_ENABLE_MEMORY_TOOL
   if (memory_tool_register() != 0) {
      OLOG_WARNING("Failed to register memory tool");
   }
#endif

#ifdef DAWN_ENABLE_DATETIME_TOOL
   if (date_tool_register() != 0) {
      OLOG_WARNING("Failed to register date tool");
   }
   if (time_tool_register() != 0) {
      OLOG_WARNING("Failed to register time tool");
   }
#endif

#ifdef DAWN_ENABLE_VOLUME_TOOL
   if (volume_tool_register() != 0) {
      OLOG_WARNING("Failed to register volume tool");
   }
#endif

#ifdef DAWN_ENABLE_LLM_STATUS_TOOL
   if (llm_status_tool_register() != 0) {
      OLOG_WARNING("Failed to register llm_status tool");
   }
#endif

#ifdef DAWN_ENABLE_SWITCH_LLM_TOOL
   if (switch_llm_tool_register() != 0) {
      OLOG_WARNING("Failed to register switch_llm tool");
   }
#endif

#ifdef DAWN_ENABLE_RESET_CONVERSATION_TOOL
   if (reset_conversation_tool_register() != 0) {
      OLOG_WARNING("Failed to register reset_conversation tool");
   }
#endif

#ifdef DAWN_ENABLE_VIEWING_TOOL
   if (viewing_tool_register() != 0) {
      OLOG_WARNING("Failed to register viewing tool");
   }
#endif

#ifdef DAWN_ENABLE_HUD_TOOLS
   if (hud_control_tool_register() != 0) {
      OLOG_WARNING("Failed to register hud_control tool");
   }
   if (hud_mode_tool_register() != 0) {
      OLOG_WARNING("Failed to register hud_mode tool");
   }
   if (faceplate_tool_register() != 0) {
      OLOG_WARNING("Failed to register faceplate tool");
   }
   if (recording_tool_register() != 0) {
      OLOG_WARNING("Failed to register recording tool");
   }
   if (visual_offset_tool_register() != 0) {
      OLOG_WARNING("Failed to register visual_offset tool");
   }
#endif

#ifdef DAWN_ENABLE_AUDIO_TOOLS
   if (voice_amplifier_tool_register() != 0) {
      OLOG_WARNING("Failed to register voice_amplifier tool");
   }
   if (audio_device_tool_register() != 0) {
      OLOG_WARNING("Failed to register audio_device tool");
   }
#endif

#ifdef DAWN_ENABLE_SCHEDULER_TOOL
   if (scheduler_tool_register() != 0) {
      OLOG_WARNING("Failed to register scheduler tool");
   }
#endif

#ifdef DAWN_ENABLE_TTS_TOOL
   if (tts_tool_register() != 0) {
      OLOG_WARNING("Failed to register tts tool");
   }
#endif

#ifdef DAWN_ENABLE_SFX_TOOL
   if (sfx_tool_register() != 0) {
      OLOG_WARNING("Failed to register sfx tool");
   }
#endif

#ifdef DAWN_ENABLE_DOCUMENT_SEARCH_TOOL
   if (document_search_tool_register() != 0) {
      OLOG_WARNING("Failed to register document_search tool");
   }
   if (document_grep_tool_register() != 0) {
      OLOG_WARNING("Failed to register document_grep tool");
   }
   if (document_read_tool_register() != 0) {
      OLOG_WARNING("Failed to register document_read tool");
   }
   if (document_index_tool_register() != 0) {
      OLOG_WARNING("Failed to register document_index tool");
   }
   if (document_manage_tool_register() != 0) {
      OLOG_WARNING("Failed to register document_manage tool");
   }
#endif

#ifdef DAWN_ENABLE_CALENDAR_TOOL
   if (calendar_tool_register() != 0) {
      OLOG_WARNING("Failed to register calendar tool");
   }
#endif

#ifdef DAWN_ENABLE_EMAIL_TOOL
   if (email_tool_register() != 0) {
      OLOG_WARNING("Failed to register email tool");
   }
#endif

#ifdef DAWN_ENABLE_RENDER_VISUAL_TOOL
   if (render_visual_tool_register() != 0) {
      OLOG_WARNING("Failed to register render_visual tool");
   }
#endif

#ifdef DAWN_ENABLE_PHONE_TOOL
   if (phone_tool_register() != 0) {
      OLOG_WARNING("Failed to register phone tool");
   }
#endif

#ifdef DAWN_ENABLE_IMAGE_SEARCH_TOOL
   if (image_search_tool_register() != 0) {
      OLOG_WARNING("Failed to register image_search tool");
   }
#endif

#ifdef DAWN_ENABLE_CONTEXT_EXPAND_TOOL
   if (context_expand_tool_register() != 0) {
      OLOG_WARNING("Failed to register context_expand tool");
   }
#endif

   /* Plan executor (always available — meta-tool, no external deps) */
   if (plan_executor_tool_register() != 0) {
      OLOG_WARNING("Failed to register plan_executor tool");
   }

   /* Messaging channels (Telegram / future Discord, Slack, SMS).
    * Tool's .init hook spawns the engine + driver; .cleanup tears
    * them down.  Skips driver registration when no token is set.
    * The messaging engine is part of the WebUI build (sessions + drivers), so
    * the tool is only available there. */
#ifdef ENABLE_WEBUI
   if (messaging_tool_register() != 0) {
      OLOG_WARNING("Failed to register messaging tool");
   }
#endif

#ifdef DAWN_ENABLE_MCP_BRIDGE_TOOL
   /* Connect to configured MCP servers and register their tools. Non-fatal:
    * a missing/unreachable server is logged and skipped. */
   if (mcp_bridge_init() != 0) {
      OLOG_WARNING("MCP bridge init failed; continuing without bridged tools");
   }
#endif

#ifdef DAWN_ENABLE_CODE_PROJECTS
   if (code_project_service_init() != 0) {
      OLOG_WARNING("Code project service init failed");
   }
   if (code_project_tool_register() != 0) {
      OLOG_WARNING("Failed to register code_project tool");
   }
#endif

   OLOG_INFO("Tool registration complete");
   return SUCCESS;
}
