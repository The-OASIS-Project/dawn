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
 * The compaction core (llm_compaction.h): sizes, the mechanical summary, and
 * the escalation from an LLM summary to a mechanical one.
 */

#include "llm/llm_compaction.h"

#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_context_text.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "utils/string_utils.h"

int llm_compaction_target_tokens(int context_size, float threshold) {
   if (threshold < 0.25f)
      threshold = 0.25f;
   float target_ratio = threshold - 0.20f;
   float floor_ratio = 0.30f;
   if (target_ratio < floor_ratio)
      target_ratio = floor_ratio;
   return (int)(context_size * target_ratio);
}

/* A string's length, as json-c keeps it (no scan); 0 for anything else. */
static size_t str_len(struct json_object *obj) {
   return json_object_is_type(obj, json_type_string) ? (size_t)json_object_get_string_len(obj) : 0;
}

int llm_compaction_estimate_range(struct json_object *history, int start_idx, int end_idx) {
   if (!history || !json_object_is_type(history, json_type_array))
      return 0;

   if (start_idx < 0)
      start_idx = 0;
   int len = json_object_array_length(history);
   if (end_idx > len)
      end_idx = len;
   if (start_idx >= end_idx)
      return 0;

   size_t total_chars = 0;

   for (int i = start_idx; i < end_idx; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj = NULL;

      /* Reasoning a turn replays (a Claude answer's thinking) counts toward
       * the next request's context too. */
      total_chars += llm_turn_message_reasoning_chars(msg);

      if (json_object_object_get_ex(msg, "content", &content_obj)) {
         if (json_object_is_type(content_obj, json_type_string)) {
            total_chars += str_len(content_obj);
         } else if (json_object_is_type(content_obj, json_type_array)) {
            int content_len = json_object_array_length(content_obj);
            for (int j = 0; j < content_len; j++) {
               struct json_object *part = json_object_array_get_idx(content_obj, j);
               struct json_object *text_obj = NULL;
               if (json_object_object_get_ex(part, "text", &text_obj)) {
                  total_chars += str_len(text_obj);
               }
               /* Claude tool_result blocks carry their (often huge) payload in
                * "content", not "text".  Without this, a large tool result reads
                * as ~0 tokens, slips past the compaction guard, and the next
                * request overflows the model window -> provider HTTP 400.  The
                * payload is a plain string or a nested block array. */
               struct json_object *pcontent = NULL;
               if (json_object_object_get_ex(part, "content", &pcontent)) {
                  if (json_object_is_type(pcontent, json_type_string)) {
                     total_chars += str_len(pcontent);
                  } else if (json_object_is_type(pcontent, json_type_array)) {
                     int pclen = json_object_array_length(pcontent);
                     for (int k = 0; k < pclen; k++) {
                        struct json_object *blk = json_object_array_get_idx(pcontent, k);
                        struct json_object *btext = NULL;
                        if (json_object_object_get_ex(blk, "text", &btext)) {
                           total_chars += str_len(btext);
                        }
                     }
                  }
               }
               struct json_object *type_obj = NULL;
               if (json_object_object_get_ex(part, "type", &type_obj)) {
                  const char *type = json_object_get_string(type_obj);
                  if (type && (strcmp(type, "image_url") == 0 || strcmp(type, "image") == 0)) {
                     total_chars += LLM_COMPACTION_IMAGE_ESTIMATE_CHARS;
                  }
               }
            }
         }
      }
      total_chars += 20;
   }

   size_t tokens = total_chars / 4;
   return (tokens > (size_t)INT_MAX) ? INT_MAX : (int)tokens;
}


#define COMPACT_DET_MAX_MSGS 200
#define COMPACT_DET_SNIPPET 80

char *llm_compaction_deterministic(struct json_object *to_summarize, int token_budget) {
   int buf_size = token_budget * 4 + 128;
   char buf[LLM_CONTEXT_SUMMARY_TARGET_L3 * 4 + 128];
   if (buf_size > (int)sizeof(buf))
      buf_size = (int)sizeof(buf);

   int offset = 0;
   int written = snprintf(buf, buf_size, "[Conversation summary (truncated)]\n");
   if (written > 0)
      offset = written;

   int msg_count = json_object_array_length(to_summarize);
   int processed = 0;

   for (int i = 0; i < msg_count && processed < COMPACT_DET_MAX_MSGS; i++) {
      int remaining = buf_size - offset - 1;
      if (remaining < 40)
         break;

      struct json_object *msg = json_object_array_get_idx(to_summarize, i);
      struct json_object *role_obj = NULL;
      const char *role = "unknown";
      if (json_object_object_get_ex(msg, "role", &role_obj))
         role = json_object_get_string(role_obj);

      struct json_object *content_obj = NULL;
      const char *content = NULL;
      char array_content_buf[COMPACT_DET_SNIPPET + 32];
      if (json_object_object_get_ex(msg, "content", &content_obj)) {
         if (json_object_is_type(content_obj, json_type_string)) {
            content = json_object_get_string(content_obj);
         } else if (json_object_is_type(content_obj, json_type_array)) {
            /* Multi-part content (tool_result / vision) — pull a short text
             * snippet if present and note when an image was attached, so the
             * summary doesn't silently drop all trace of what happened here
             * (e.g. a persisted `viewing` tool capture, see
             * llm_tools_add_results_openai/claude). */
            const char *text_part = NULL;
            bool has_image = false;
            int clen = json_object_array_length(content_obj);
            for (int j = 0; j < clen; j++) {
               struct json_object *part = json_object_array_get_idx(content_obj, j);
               struct json_object *type_obj = NULL;
               if (!part || !json_object_object_get_ex(part, "type", &type_obj))
                  continue;
               const char *ptype = json_object_get_string(type_obj);
               if (!text_part && ptype && strcmp(ptype, "text") == 0) {
                  struct json_object *text_obj = NULL;
                  if (json_object_object_get_ex(part, "text", &text_obj))
                     text_part = json_object_get_string(text_obj);
               } else if (ptype &&
                          (strcmp(ptype, "image_url") == 0 || strcmp(ptype, "image") == 0)) {
                  has_image = true;
               }
            }
            if (has_image || text_part) {
               snprintf(array_content_buf, sizeof(array_content_buf), "%s%s",
                        has_image ? "[image] " : "", text_part ? text_part : "");
               content = array_content_buf;
            }
         }
      }
      if (!content || content[0] == '\0')
         continue;

      int snippet_len = COMPACT_DET_SNIPPET;
      int content_len = (int)strlen(content);
      if (snippet_len > content_len)
         snippet_len = content_len;
      /* Never inside a UTF-8 character: a provider refuses a request that has half of one. */
      while (snippet_len > 0 && snippet_len < content_len &&
             ((unsigned char)content[snippet_len] & 0xC0) == 0x80)
         snippet_len--;

      written = snprintf(buf + offset, remaining, "- %s: %.*s%s\n", role, snippet_len, content,
                         (content_len > snippet_len) ? "..." : "");
      if (written >= remaining) {
         buf[offset] = '\0';
         break;
      }
      offset += written;
      processed++;
   }

   return strdup(buf);
}


char *llm_compaction_summarize(struct json_object *to_summarize,
                               int kept_tokens,
                               int target_tokens,
                               const char *tag,
                               llm_compaction_summarize_fn summarize,
                               void *ctx,
                               const atomic_bool *cancel,
                               llm_compaction_level_t *level_out) {
   /* The input's own size, for the size gate below. */
   const int input_tokens = llm_compaction_estimate_range(
       to_summarize, 0, (int)json_object_array_length(to_summarize));

   char *summary = NULL;
   llm_compaction_level_t level = LLM_COMPACT_NORMAL;
   for (; level <= LLM_COMPACT_MAX_LEVEL; level++) {
      free(summary);
      summary = NULL;

      if (level == LLM_COMPACT_DETERMINISTIC) {
         summary = llm_compaction_deterministic(to_summarize, LLM_CONTEXT_SUMMARY_TARGET_L3);
      } else if (summarize) {
         summary = summarize(to_summarize, level, ctx);
      }
      if (summary) {
         /* Written from history that holds untrusted text: what imitates DAWN's
          * framing or carries a tag doesn't pass into the summary it replays. */
         summary = llm_context_mask_tag(llm_context_neutralize_owned(summary), tag);
      }
      if (summary && strlen(summary) > LLM_COMPACTION_SUMMARY_MAX) {
         /* Capped here, once: what is stored is what was sent. */
         summary[LLM_COMPACTION_SUMMARY_MAX] = '\0';
         utf8_trim_incomplete(summary);
      }

      if (!summary && cancel && atomic_load(cancel)) {
         OLOG_INFO("llm_compaction: summary cancelled");
         return NULL;
      }
      if (!summary) {
         if (level < LLM_COMPACT_MAX_LEVEL) {
            OLOG_WARNING("llm_compaction: L%d summary failed, escalating to L%d", level + 1,
                         level + 2);
            continue;
         }
         OLOG_ERROR("llm_compaction: All compaction levels failed");
         return NULL;
      }

      /* Estimate includes the CONVERSATION SUMMARY framing around it
       * (llm_history_summary_text, ~120 chars with the tag). */
      int summary_chars = (int)strlen(summary) + 120;
      int summary_tokens = (summary_chars + 20) / 4; /* +20 for message overhead, /4 heuristic */
      int estimated_total = kept_tokens + summary_tokens;

      OLOG_INFO("llm_compaction: L%d summary: ~%d tokens, total ~%d (target %d)", level + 1,
                summary_tokens, estimated_total, target_tokens);

      if (estimated_total <= target_tokens)
         break;

      /* L3 is the guaranteed floor — always accept its result */
      if (level == LLM_COMPACT_DETERMINISTIC) {
         OLOG_WARNING("llm_compaction: L3 result (%d tokens) still exceeds target (%d), "
                      "accepting as best effort",
                      estimated_total, target_tokens);
         break;
      }

      /* Size-gate: if summary is longer than the input, LLM isn't cooperating */
      if (level == LLM_COMPACT_NORMAL && summary_tokens > input_tokens) {
         OLOG_WARNING("llm_compaction: L1 summary (%d tokens) exceeds input (%d tokens), "
                      "skipping L2 — model not following instructions",
                      summary_tokens, input_tokens);
         level = LLM_COMPACT_AGGRESSIVE; /* Loop increment brings us to DETERMINISTIC */
         continue;
      }

      OLOG_WARNING("llm_compaction: L%d result (%d tokens) exceeds target (%d), escalating",
                   level + 1, estimated_total, target_tokens);
   }
   if (level_out) {
      *level_out = level;
   }
   return summary;
}

float llm_compaction_factor_update(float factor, int samples, int d_prompt, int d_estimate) {
   if (d_estimate < LLM_COMPACTION_FACTOR_MIN_GROWTH || d_prompt <= 0) {
      return factor;
   }
   float sample = (float)d_prompt / (float)d_estimate;
   if (sample < LLM_COMPACTION_FACTOR_MIN) {
      sample = LLM_COMPACTION_FACTOR_MIN;
   } else if (sample > LLM_COMPACTION_FACTOR_MAX) {
      sample = LLM_COMPACTION_FACTOR_MAX;
   }
   if (samples <= 0) {
      return sample;
   }
   return factor + LLM_COMPACTION_FACTOR_WEIGHT * (sample - factor);
}

/* The fixed part, as the last request's model read it, then as this one does. */
static float fixed_part(const llm_compaction_calibration_t *cal) {
   float fixed = (float)cal->last_prompt - cal->last_factor * (float)cal->last_estimate;
   return fixed > 0.0f ? fixed * cal->factor / cal->last_factor : 0.0f;
}

static bool usable(const llm_compaction_calibration_t *cal) {
   return cal && cal->known && cal->last_factor > 0.0f && cal->factor > 0.0f;
}

int llm_compaction_calibrated_tokens(const llm_compaction_calibration_t *cal, int estimate) {
   if (!usable(cal)) {
      return estimate;
   }
   const float tokens = fixed_part(cal) + cal->factor * (float)estimate;
   return tokens > (float)INT_MAX ? INT_MAX : (int)tokens;
}

int llm_compaction_estimate_budget(const llm_compaction_calibration_t *cal, int tokens) {
   if (!usable(cal)) {
      return tokens;
   }
   const float room = ((float)tokens - fixed_part(cal)) / cal->factor;
   return room > 0.0f ? (int)room : 0;
}
