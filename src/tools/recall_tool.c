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
 * Unified cross-source recall tool — see recall_tool.h and
 * docs/CROSS_TOOL_RECALL_DESIGN.md.
 */

#include "tools/recall_tool.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "core/focus/focus_source.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/recall_format.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Forward declarations
 * ============================================================================= */

static char *recall_callback(const char *action, char *value, int *should_respond);
static bool recall_is_available(void);

/* =============================================================================
 * Tool metadata
 * ============================================================================= */

static const treg_param_t recall_params[] = {
   {
       .name = "query",
       .description = "What to gather context about — a topic, person, project, or status "
                      "(a few words, e.g. 'Open Sauce 2026 prep' or 'wrist recovery').",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t recall_metadata = {
   .name = "recall",
   .device_string = "recall",
   .description =
       "Your FIRST move for any \"what do we know / what's the status / how does X stand / tell me "
       "about\" question answered from the user's own knowledge. Gathers EVERYTHING known about a "
       "topic, person, project, or status in ONE call — searches memory (facts, summaries, "
       "relationships), saved notes, uploaded documents, and the calendar together, ranked by "
       "relevance (hybrid keyword + semantic). Prefer this over jumping to a single per-source "
       "search, which misses cross-source context — UNLESS you already know the exact note label, "
       "document, or memory id to fetch (then use document_read / memory get directly). "
       "Returns results grouped by source, each with a pointer to where to read the full or exact "
       "text. This is the high-level context tool; the per-source search tools are for targeted "
       "follow-ups.",
   .params = recall_params,
   .param_count = 1,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = 0,
   .is_available = recall_is_available,
   .callback = recall_callback,
};

/* =============================================================================
 * Registration / availability
 * ============================================================================= */

int recall_tool_register(void) {
   return tool_registry_register(&recall_metadata);
}

static bool recall_is_available(void) {
   /* Most sources (documents, calendar, summary/entity adapters) need an
    * embedding to rank; gate on the engine like document_search.  Note: this
    * hides the tool entirely when the engine is DOWN.  The callback's
    * keyword-only degrade (qptr=NULL) therefore covers only a transient
    * per-query embed FAILURE, not the engine-unavailable case. */
   return embedding_engine_available();
}

/* =============================================================================
 * Callback
 * ============================================================================= */

static char *recall_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0')
      return strdup("Error: recall needs a query (a topic, person, project, or status).");

   const int user_id = tool_get_current_user_id();
   const int dims = embedding_engine_dims();
   if (dims <= 0)
      return strdup("Error: embedding engine not initialized.");

   float *qvec = malloc((size_t)dims * sizeof(float));
   if (!qvec)
      return strdup("Error: memory allocation failed.");

   int out_dims = 0;
   const float *qptr = qvec;
   if (embedding_engine_embed(value, qvec, dims, &out_dims) != 0 || out_dims != dims) {
      /* Graceful degrade: keyword-capable adapters still run; the framework
       * skips embedding-only adapters (documents/calendar) when qptr == NULL. */
      OLOG_WARNING("recall: query embedding failed — proceeding keyword-only");
      qptr = NULL;
   }

   const recall_config_t *rc = &g_config.memory.recall;
   const focus_limits_t limits = {
      .top_k = rc->top_k,
      .min_score = rc->min_score,
      .budget_bytes = rc->budget_bytes,
   };

   focus_compose_result_t result;
   memset(&result, 0, sizeof(result));
   /* Match the per-turn focus path's include_private=false.  The flag is
    * currently a no-op (every adapter ignores it — the "1f gap"), so this is
    * behaviorally inert today; passing false means that if private-conversation
    * filtering is ever implemented, recall inherits the SAFE default rather than
    * surfacing private content into a possibly-shared session. */
   const int compose_rc = focus_compose_ex(user_id, /*include_private=*/false, value, qptr,
                                           (size_t)dims, time(NULL), rc->per_source_max, &limits,
                                           &result);
   free(qvec);

   if (compose_rc != SUCCESS) {
      focus_result_free(&result);
      return strdup("Error: couldn't gather context (recall failed). Try a targeted memory or "
                    "document search instead.");
   }

   OLOG_INFO("recall: user=%d query='%.40s' candidates=%d", user_id, value, result.candidate_count);

   /* v1: the turn's injected-id set isn't wired through yet (design §4.2a
    * fallback) — pass NULL so the formatter notes the likely overlap instead
    * of silently re-stating already-injected context. */
   char *out = recall_format_result(value, &result, NULL, 0);
   focus_result_free(&result);
   return out ? out : strdup("recall: failed to format result.");
}
