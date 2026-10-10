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
 * the project author(s).
 *
 * The prompt builder (dawn_build_prompt) that runs per turn, on every
 * surface: the system prompt a conversation freezes, in named sections
 * (build_stable_sections), the surface's standing directions
 * (build_directives), what DAWN knows about the user, and the turn's context
 * (its time, retrieved items and notes).  session_prefix.c applies them to the
 * conversation, append-only.
 */

#include "core/prompt_builder.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_withdraw.h"
#include "config/dawn_config.h"
#include "core/buf_printf.h"
#include "core/build_focus_block.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "core/text_filter.h"
#include "dawn_error.h"
#include "llm/llm_claude_betas.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "memory/memory_context.h"
#include "prompts.h"
#include "tools/hud_discovery.h"
#include "utils/string_utils.h"
#include "webui/webui_server.h"

/**
 * @brief Build the v44 user-identity block.
 *
 * Composes real_name / preferred_address / identity_aliases into the
 * "## User Identity" block that's appended after the persona/settings
 * block in the system prompt.  Returns an allocated string (caller
 * frees) or NULL if the user has no real_name set.
 *
 * Aliases parsing: newline-separated input, strip whitespace, drop empty
 * lines, dedupe case-insensitive, emit comma-joined.  Total output size
 * bounded by the AUTH_REAL_NAME_MAX + AUTH_PREFERRED_ADDRESS_MAX +
 * AUTH_IDENTITY_ALIASES_MAX caps and a small fixed overhead.
 *
 * @param user_id User ID (0 returns NULL — block requires a known user)
 * @return Allocated string (caller frees) or NULL if real_name unset
 */
static char *build_identity_block(int user_id) {
   if (user_id <= 0)
      return NULL;

   auth_user_identity_t identity;
   if (auth_db_get_user_identity(user_id, &identity) != AUTH_DB_SUCCESS)
      return NULL;
   if (identity.real_name[0] == '\0')
      return NULL; /* no real_name → skip the entire block */

   /* Parse identity_aliases: split on \n, strip whitespace per token,
    * drop empties, dedupe case-insensitive.  Up to 16 aliases tracked. */
   char joined_aliases[AUTH_IDENTITY_ALIASES_MAX];
   joined_aliases[0] = '\0';
   if (identity.identity_aliases[0] != '\0') {
      char buf[AUTH_IDENTITY_ALIASES_MAX];
      safe_strscpy(buf, identity.identity_aliases);

      char *seen[16];
      int seen_count = 0;
      size_t out_off = 0;

      char *save = NULL;
      for (char *line = strtok_r(buf, "\n", &save); line != NULL && seen_count < 16;
           line = strtok_r(NULL, "\n", &save)) {
         /* Strip leading whitespace */
         while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
         /* Strip trailing whitespace */
         char *end = line + strlen(line);
         while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            end--;
         *end = '\0';
         if (*line == '\0')
            continue;
         /* Case-insensitive dedupe against already-emitted aliases. */
         bool dup = false;
         for (int i = 0; i < seen_count; i++) {
            if (strcasecmp(seen[i], line) == 0) {
               dup = true;
               break;
            }
         }
         if (dup)
            continue;
         seen[seen_count++] = line;
         /* Append to joined_aliases with comma separator. */
         size_t alias_len = strlen(line);
         size_t need = alias_len + (out_off > 0 ? 2 : 0); /* + ", " when not first */
         if (out_off + need + 1 >= sizeof(joined_aliases))
            break;
         if (out_off > 0) {
            joined_aliases[out_off++] = ',';
            joined_aliases[out_off++] = ' ';
         }
         memcpy(joined_aliases + out_off, line, alias_len);
         out_off += alias_len;
         joined_aliases[out_off] = '\0';
      }
   }

   /* Worst-case stack: ~1.5 KB.  Trip if any cap bumps push past 4 KB so we
    * notice before the LLM/refresh worker stack (8 MB) gets uncomfortable. */
   _Static_assert(
       AUTH_REAL_NAME_MAX + AUTH_PREFERRED_ADDRESS_MAX + AUTH_IDENTITY_ALIASES_MAX + 256 < 4096,
       "build_identity_block stack buffer exceeded 4 KB; revisit caps or heap-alloc");
   char block[AUTH_REAL_NAME_MAX + AUTH_PREFERRED_ADDRESS_MAX + AUTH_IDENTITY_ALIASES_MAX + 256];
   size_t off = 0;
   size_t rem = sizeof(block);
   BUF_PRINTF(block, off, rem, "\n\n## User Identity\nYou are speaking with %s.",
              identity.real_name);
   if (identity.preferred_address[0] != '\0') {
      BUF_PRINTF(block, off, rem, " They prefer to be addressed as %s.",
                 identity.preferred_address);
   }
   if (joined_aliases[0] != '\0') {
      BUF_PRINTF(block, off, rem, " They may also be referred to as: %s.", joined_aliases);
   }
   BUF_PRINTF(block, off, rem,
              " Use this information to recognize when memory facts or extracted entities refer "
              "to them.\n");
   return strdup(block);
}

/* The user's own context: location, timezone and units, and (append mode)
 * their persona traits.  Empty when they set none. */
static void user_context_text(const auth_user_settings_t *settings,
                              bool replace_persona,
                              char *buf,
                              size_t size) {
   size_t off = 0;
   size_t rem = size;
   buf[0] = '\0';
   const bool has_persona = !replace_persona && settings->persona_description[0] != '\0';
   const bool has_location = settings->location[0] != '\0';
   const bool has_timezone = settings->timezone[0] != '\0';
   const bool has_units = settings->units[0] != '\0';
   if (!has_persona && !has_location && !has_timezone && !has_units)
      return;
   BUF_PRINTF(buf, off, rem, replace_persona ? "## User Info\n" : "## User Context\n");
   if (has_persona)
      BUF_PRINTF(buf, off, rem, "Additional persona traits: %s\n", settings->persona_description);
   if (has_location)
      BUF_PRINTF(buf, off, rem, "Location: %s\n", settings->location);
   if (has_timezone)
      BUF_PRINTF(buf, off, rem, "Timezone: %s\n", settings->timezone);
   if (has_units)
      BUF_PRINTF(buf, off, rem, "Preferred units: %s\n", settings->units);
}

/**
 * @brief The system prompt a conversation starting now would freeze, in
 *        named sections, and joined (out->stable_prefix)
 *
 * The same base for every surface (get_command_prompt_parts): what differs by
 * surface is a standing direction (build_directives), so a conversation
 * continued from another surface keeps its prompt.  In order:
 *   - identity_override or persona: a replace-mode persona, else the base one
 *   - rules: the base prompt's rules
 *   - tool_defaults: the configured location / units / timezone, for a guest
 *     only (a user's own settings are their user_context)
 *   - user_context: the user's location / timezone / units / persona traits
 *   - user_identity: who the user is (real name, how to address them, aliases)
 *   - memory_rules, citation_rules: when memory is on for the user
 *   - tool_discipline, recall_routing, background_deliveries, context_rules
 * A running conversation gets a changed section alone, appended
 * (session_prefix.c), so each is a unit that reads on its own.
 *
 * @param user_id The turn's user (<= 0: a guest; nothing of any user's)
 * @return SUCCESS, or FAILURE on allocation failure (@p out's sections are
 *         then partial; the caller frees them)
 */
static int build_stable_sections(int user_id, composed_prompt_t *out) {
   command_prompt_parts_t base;
   if (get_command_prompt_parts(&base) != 0)
      return FAILURE;

   auth_user_settings_t settings;
   const bool have_settings = user_id > 0 &&
                              auth_db_get_user_settings(user_id, &settings) == AUTH_DB_SUCCESS;
   const bool replace_persona = have_settings && settings.persona_description[0] != '\0' &&
                                strcmp(settings.persona_mode, "replace") == 0;

   int err = 0;
   /* A replace-mode persona takes the base persona's place: the model gets
    * one identity, not two with an instruction to ignore one. */
   if (replace_persona) {
      char override[AUTH_PERSONA_DESC_MAX + 32];
      snprintf(override, sizeof(override), "## Your Identity\n%s", settings.persona_description);
      err |= prompt_sections_add(out, "identity_override", "who you are", override);
   } else {
      err |= prompt_sections_add(out, "persona", "your persona", base.persona);
   }
   err |= prompt_sections_add(out, "rules", "your operating rules", base.rules);
   if (user_id <= 0)
      err |= prompt_sections_add(out, "tool_defaults", "the tool defaults", base.tool_defaults);
   command_prompt_parts_free(&base);

   if (have_settings) {
      char context[AUTH_PERSONA_DESC_MAX + AUTH_LOCATION_MAX + AUTH_TIMEZONE_MAX + AUTH_UNITS_MAX +
                   128];
      user_context_text(&settings, replace_persona, context, sizeof(context));
      err |= prompt_sections_add(out, "user_context", "the user's context", context);
   }
   char *identity = build_identity_block(user_id);
   err |= prompt_sections_add(out, "user_identity", "who the user is", identity);
   free(identity);

   if (user_id > 0 && g_config.memory.enabled) {
      err |= prompt_sections_add(out, "memory_rules", "the memory instructions",
                                 SYSTEM_PROMPT_MEMORY_INSTRUCTIONS);
      if (g_config.memory.citation_enabled)
         err |= prompt_sections_add(out, "citation_rules", "the memory citation rules",
                                    SYSTEM_PROMPT_MEMORY_CITATIONS);
   }
   err |= prompt_sections_add(out, "tool_discipline", "tool-call discipline",
                              SYSTEM_PROMPT_TOOL_CALL_DISCIPLINE);
   err |= prompt_sections_add(out, "recall_routing", "context gathering",
                              SYSTEM_PROMPT_RECALL_ROUTING);
   err |= prompt_sections_add(out, "background_deliveries", "background deliveries",
                              SYSTEM_PROMPT_BACKGROUND_DELIVERIES);
   err |= prompt_sections_add(out, "context_rules", "context DAWN adds",
                              SYSTEM_PROMPT_CONTEXT_RULES);
   if (err)
      return FAILURE;
   out->stable_prefix = prompt_sections_join(out);
   return out->stable_prefix ? SUCCESS : FAILURE;
}

/**
 * @brief Append the messaging-channel context: the surface a messaging turn
 *        arrived on, and the channel to default scheduler deliveries to.
 *
 * One of the surface's standing directions (build_directives).  Transfers
 * ownership of @p base and frees it on success; returns @p base unchanged
 * when the dispatch session isn't messaging, has no provider, or on OOM.
 */
static char *append_messaging_context(char *base, session_t *dispatch) {
   if (base == NULL || dispatch == NULL)
      return base;
   if (dispatch->type != SESSION_TYPE_MESSAGING)
      return base;
   const char *provider = dispatch->messaging_identity.provider;
   const char *channel = dispatch->messaging_identity.channel_name;
   if (provider == NULL || provider[0] == '\0')
      return base;

   /* Explicit, actionable context block.  A bare "Key=value." shape was too
    * terse: the model saw the data but didn't connect it to the scheduler
    * tool's deliver_to default.  This says (a) which surface the user is on,
    * (b) the channel name usable as deliver_to, and (c) the rule inline, and
    * for SMS how a reply should read.  Stable across a session's turns, so it
    * caches cleanly. */
   const char *format = strcmp(provider, "sms") == 0 ? MESSAGING_SMS_FORMAT_DIRECTION : "";
   char ctx[768];
   ctx[0] = '\0';
   int len;
   if (channel && channel[0]) {
      len = snprintf(ctx, sizeof(ctx),
                     "\n\nYou're replying through the %s channel \"%s\"; the user reads your "
                     "replies there. When you schedule something for them (scheduler tool), set "
                     "deliver_to to \"%s\" so it reaches them here, unless they name a different "
                     "channel or ask for it to stay local.%s",
                     provider, channel, channel, format);
   } else {
      /* Provider known but no channel display_name (uncommon — only if
       * messaging_channels.display_name is NULL).  Surface what we have
       * so the LLM at least knows the surface. */
      len = snprintf(ctx, sizeof(ctx),
                     "\n\nYou're replying through %s; the user reads your replies there.%s",
                     provider, format);
   }
   if (len < 0 || (size_t)len >= sizeof(ctx)) {
      return base;
   }

   const size_t base_len = strlen(base);
   const size_t ctx_len = strlen(ctx);
   char *combined = malloc(base_len + ctx_len + 1);
   if (combined == NULL)
      return base;
   memcpy(combined, base, base_len);
   memcpy(combined + base_len, ctx, ctx_len);
   combined[base_len + ctx_len] = '\0';
   free(base);
   return combined;
}

/**
 * @brief Append the room a DAP2 satellite is in, and its Home Assistant area.
 *
 * One of the surface's standing directions (build_directives): the room the
 * satellite is in, and its Home Assistant area when mapped (satellite_db
 * lookup, best-effort; non-allowlist characters in the area become '_').
 * Transfers ownership of @p base and frees it on success; returns @p base
 * unchanged when the dispatch session isn't a DAP2 satellite with a room, or
 * on OOM.
 */
static char *append_satellite_context(char *base, session_t *dispatch) {
   if (base == NULL || dispatch == NULL)
      return base;
   if (dispatch->type != SESSION_TYPE_DAP2 || dispatch->identity.uuid[0] == '\0')
      return base;
   const char *room = dispatch->identity.location;
   if (room == NULL || room[0] == '\0')
      return base;

   /* satellite_db lookup is best-effort — failure just means no ha_area
    * suffix; we still emit the room sentence. */
   satellite_mapping_t mapping;
   const char *ha_area = NULL;
   if (satellite_db_get(dispatch->identity.uuid, &mapping) == 0 && mapping.ha_area[0] != '\0')
      ha_area = mapping.ha_area;

   /* A sentence, not "Room=X." alone: the bare key=value form gives the model
    * the data without saying what it is for. */
   char ctx[320];
   ctx[0] = '\0';
   int len = snprintf(ctx, sizeof(ctx),
                      "\nYou're speaking with the user through the satellite "
                      "in the %s.",
                      room);
   if (len < 0 || (size_t)len >= sizeof(ctx)) {
      /* Room name doesn't fit — skip the suffix entirely rather than
       * emitting a truncated sentence. */
      return base;
   }
   if (ha_area != NULL && len < (int)sizeof(ctx) - 1) {
      /* Sanitize ha_area: allowlist [A-Za-z0-9 _-] only; everything else
       * becomes '_'. */
      char safe_area[64];
      safe_strscpy(safe_area, ha_area);
      for (char *p = safe_area; *p; p++) {
         if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
               *p == ' ' || *p == '-' || *p == '_'))
            *p = '_';
      }
      snprintf(ctx + len, sizeof(ctx) - len,
               " For Home Assistant requests that don't name a room, use the \"%s\" area.",
               safe_area);
   }

   const size_t base_len = strlen(base);
   const size_t ctx_len = strlen(ctx);
   char *combined = malloc(base_len + ctx_len + 1);
   if (combined == NULL)
      return base; /* OOM fallback: leave base intact, no satellite suffix */
   memcpy(combined, base, base_len);
   memcpy(combined + base_len, ctx, ctx_len);
   combined[base_len + ctx_len] = '\0';
   free(base);
   return combined;
}

/**
 * @brief Append a directive block to a heap-allocated prompt segment.
 *
 * Concatenates @p text onto @p base with a "\n\n" separator, matching the
 * spacing the satellite/messaging context appends use.  Transfers ownership of @p base and frees it
 * on success; no-op (returns @p base unchanged) when @p text is empty or on OOM. When @p base is
 * NULL/empty the directive becomes the whole segment (no leading separator).
 */
static char *append_block_directive(char *base, const char *text) {
   if (text == NULL || text[0] == '\0')
      return base;
   const size_t tlen = strlen(text);
   if (base == NULL || base[0] == '\0') {
      char *out = malloc(tlen + 1);
      if (out == NULL)
         return base;
      memcpy(out, text, tlen + 1);
      free(base); /* base may be a non-NULL empty string */
      return out;
   }
   const size_t blen = strlen(base);
   char *out = malloc(blen + 2 + tlen + 1);
   if (out == NULL)
      return base;
   /* the next memcpy copies tlen + 1 bytes, NUL included */
   // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
   memcpy(out, base, blen);
   out[blen] = '\n';
   out[blen + 1] = '\n';
   memcpy(out + blen + 2, text, tlen + 1);
   free(base);
   return out;
}

/* Add @p text to a directive set, a blank line apart (leading newlines of the
 * piece dropped).  Takes @p set. */
static char *add_directive(char *set, const char *text) {
   while (text != NULL && *text == '\n') {
      text++;
   }
   return append_block_directive(set, text);
}

/**
 * @brief The standing directions of the surface a turn arrived on
 *
 * Which tools are unavailable right now, the room a local mic or a satellite
 * is in, a messaging channel, spoken output and speech-to-text input (the
 * local mic and a satellite are both; a WebUI turn is spoken when the user has
 * voice on), and a background job's headless mode.  The full set every time
 * ("" for none): a conversation is sent it again only when it differs from
 * the set it last had, so switching surfaces (a messaging chat continued in
 * the browser, a helmet conversation continued at a desk) reaches the model
 * and nothing of the old surface outlives it.
 */
static char *build_directives(session_t *dispatch) {
   char *set = strdup("");
   if (set == NULL)
      return NULL;
   const bool is_remote = dispatch == NULL || dispatch->type != SESSION_TYPE_LOCAL;
   if (llm_tools_enabled(NULL)) {
      char hint[2048];
      if (llm_tools_build_disabled_hint(is_remote, hint, sizeof(hint)) > 0)
         set = add_directive(set, hint);
      /* What the helmet offers now (the HUD tools' schemas name no values). */
      if (hud_discovery_describe(hint, sizeof(hint)) > 0)
         set = add_directive(set, hint);
   }
   if (dispatch == NULL)
      return set;

   char *room = append_satellite_context(strdup(""), dispatch);
   if (room != NULL) {
      set = add_directive(set, room);
      free(room);
   }
   char *channel = append_messaging_context(strdup(""), dispatch);
   if (channel != NULL) {
      set = add_directive(set, channel);
      free(channel);
   }
   if (dispatch->type == SESSION_TYPE_LOCAL && g_config.general.room[0] != '\0') {
      char room_line[sizeof(g_config.general.room) + 48];
      snprintf(room_line, sizeof(room_line), "The user is talking to you from the %s.",
               g_config.general.room);
      set = add_directive(set, room_line);
   }
   if (dispatch->type == SESSION_TYPE_LOCAL || dispatch->type == SESSION_TYPE_DAP2 ||
       dispatch->type == SESSION_TYPE_DAP) {
      set = add_directive(set, voice_directive_effective());
      set = add_directive(set, asr_disambiguation_hint_effective());
   }
   if (dispatch->type == SESSION_TYPE_JOB) {
      set = add_directive(set, JOB_HEADLESS_DIRECTIVE);
   }
   if (dispatch->type == SESSION_TYPE_WEBUI && webui_session_tts_enabled(dispatch)) {
      set = add_directive(set, voice_directive_webui_effective());
   }
   return set;
}

/* Whether @p session's turn sends a tool change in place: its model, on the
 * Claude API itself (claude_betas_inline_tools_ok). */
static bool inline_tools_for(session_t *session) {
   if (session == NULL) {
      return false;
   }
   session_llm_config_t config;
   session_get_llm_config(session, &config);
   llm_resolved_config_t resolved;
   if (llm_resolve_config(&config, &resolved) != 0 || resolved.type != LLM_CLOUD ||
       resolved.cloud_provider != CLOUD_PROVIDER_CLAUDE) {
      return false;
   }
   /* Copied at once (the resolved strings may point at the stack). */
   char model[LLM_MODEL_NAME_MAX];
   char endpoint[512];
   snprintf(model, sizeof(model), "%s", resolved.model ? resolved.model : "");
   snprintf(endpoint, sizeof(endpoint), "%s", resolved.endpoint ? resolved.endpoint : CLAUDE_URL);
   return claude_betas_inline_tools_ok(endpoint, model[0] ? model : llm_get_default_claude_model());
}

int dawn_build_prompt(session_t *session,
                      int user_id,
                      const char *user_turn_text,
                      composed_prompt_t *out) {
   if (out == NULL)
      return FAILURE;
   /* Initialize output so the caller can safely composed_prompt_free
    * on either SUCCESS or FAILURE return. */
   memset(out, 0, sizeof(*out));
   out->built_at = (int64_t)time(NULL);
   /* Before memory is read: a withdrawal from here on counts as after it. */
   if (conv_db_withdraw_seq(&out->built_seq) != AUTH_DB_SUCCESS) {
      out->built_seq = 0; /* every withdrawal on record counts as after it */
   }

   /* The system prompt a conversation starting now would freeze, in sections.
    * On failure the dispatcher keeps the turn's last prompt. */
   if (build_stable_sections(user_id, out) != SUCCESS) {
      composed_prompt_free(out);
      return FAILURE;
   }

   session_t *dispatch = session;

   /* What DAWN knows about the user (NULL when nothing, or memory is off). */
   if (user_id > 0 && g_config.memory.enabled)
      out->memory_body = memory_build_context(user_id, g_config.memory.context_budget_tokens);

   /* The surface's standing directions; none (the conversation's stay in
    * force) for a turn run from none of its surfaces. */
   if (!(dispatch != NULL && atomic_load(&dispatch->keeps_directions))) {
      out->directives = build_directives(dispatch);
      if (out->directives == NULL) {
         composed_prompt_free(out);
         return FAILURE;
      }
   }
   if (llm_tools_enabled(NULL)) {
      out->tool_defs = llm_tools_definitions_hashed(&out->tool_def_hashes, out->tool_defs_fp);
      out->tool_schemas = llm_tools_schema_hashes();
      out->inline_tools = inline_tools_for(dispatch);
   }

   /* This turn's context, sent in front of its question every turn (framed,
    * with the conversation's tag, at the seam): the time, the retrieved
    * items the conversation doesn't show yet (chosen at the seam), then
    * per-turn notes.  Earlier turns' contexts stay as they were sent. */
   out->context_head = prompt_turn_head(time(NULL));
   if (out->context_head == NULL) {
      composed_prompt_free(out);
      return FAILURE;
   }
   int64_t conv_id = 0;
   int64_t turn_id = 0;
   if (dispatch != NULL) {
      conv_id = session_turn_conversation(dispatch); /* the turn's, not the view */
      turn_id = session_get_last_user_msg_id(dispatch);
   }
   /* No items when retrieval fails: the turn runs without them. */
   (void)build_focus_block(dispatch, user_id, conv_id, turn_id, user_turn_text, out);
   /* A WebUI voice turn (other voice surfaces carry the hint as a standing
    * direction, build_directives). */
   const bool spoken = dispatch != NULL && dispatch->type == SESSION_TYPE_WEBUI &&
                       session_turn_spoken(dispatch);
   const char *hint = spoken ? asr_disambiguation_hint_effective() : NULL;
   if (hint && hint[0]) {
      out->context_tail = strdup(hint);
      if (out->context_tail == NULL) {
         composed_prompt_free(out);
         return FAILURE;
      }
   }
   return SUCCESS;
}
