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
 * The tool loop's view stage (llm_tool_views.h): after a batch runs, a result
 * over its share is kept whole and replaced by a view with a handle; every
 * result is then finished (neutralized last, the WebUI told what the model
 * sees).
 */

#include "llm/llm_tool_views_apply.h"

#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/tool_result_store.h"
#include "llm/llm_context.h"
#include "llm/llm_context_text.h"
#include "llm/llm_tool_view.h"
#include "llm/llm_tool_views.h"
#include "logging.h"
#include "tools/tool_registry.h"

void llm_tool_views_budget_batch(const llm_tool_loop_params_t *params,
                                 const tool_call_list_t *calls,
                                 const char *assistant_text,
                                 llm_tool_views_budget_t *out) {
   memset(out, 0, sizeof(*out));
   out->window = llm_context_get_size(params->llm_type, params->cloud_provider, params->model);
   if (out->window > 0) {
      /* How full the request is (the calibrated estimate compaction trusts;
       * a call with no session has no calibration of its own, so the raw
       * estimate), then the assistant's tool-call message about to join it,
       * counted at the worst density. */
      const int history = llm_context_estimate_tokens(params->conversation_history);
      const int request = params->has_session
                              ? llm_context_request_tokens(params->session_id, history,
                                                           params->llm_type, params->cloud_provider,
                                                           params->model)
                              : history;
      size_t call_chars = assistant_text ? strlen(assistant_text) : 0;
      for (int i = 0; calls && i < calls->count; i++) {
         call_chars += strlen(calls->calls[i].name) + strlen(calls->calls[i].arguments);
      }
      const int call_tokens = (int)(call_chars / LLM_TOOL_VIEWS_WORST_CHARS_PER_TOKEN);
      const int reserve = g_config.llm.max_tokens > 0 &&
                                  g_config.llm.max_tokens < LLM_TOOL_VIEWS_REPLY_RESERVE_MAX
                              ? g_config.llm.max_tokens
                              : LLM_TOOL_VIEWS_REPLY_RESERVE_MAX;
      out->room_tokens = (int)((float)out->window * llm_context_hard_threshold()) - request -
                         call_tokens - reserve;
   }
   out->chars = llm_tool_views_budget_chars(out->window, out->room_tokens, out->window > 0);
}

_Static_assert(LLM_TOOL_VIEWS_BATCH_MAX >= LLM_TOOLS_MAX_PARALLEL_CALLS,
               "a batch the stage shapes holds every parallel call");

/* Left alone by the stage: a result spoken directly, one the tool handled
 * itself, a vision result, or one its tool always shows whole (a visual:
 * the WebUI renders it from the full text).  Keyed on the tool, never on the
 * content, which any page or message could imitate. */
static bool left_alone(const tool_result_t *r, const tool_metadata_t *meta) {
   return r->skip_followup || !r->should_respond || r->vision_image != NULL ||
          (meta && meta->result_whole);
}

/* The first parameter of @p meta that narrows a call, or NULL. */
static const char *narrowing_param(const tool_metadata_t *meta) {
   for (int i = 0; meta && i < meta->param_count; i++) {
      if (llm_tool_views_narrows(meta->params[i].name)) {
         return meta->params[i].name;
      }
   }
   return NULL;
}

/* Out of memory viewing @p r: a notice in its place, never the whole result. */
static void too_large_to_show(tool_result_t *r) {
   free(r->result_extended);
   r->result_extended = NULL;
   snprintf(r->result, LLM_TOOLS_RESULT_LEN,
            "[Tool result too large to show; call the tool again with narrower arguments.]");
}

/* What the stage decided for a batch, shared by its results. */
typedef struct {
   session_t *session;
   const llm_tool_views_budget_t *budget;
   bool offers_read;
   char tag[LLM_CONTEXT_TAG_MAX];
} stage_t;

/* View result @p r of @p call (tool @p meta) within @p share characters:
 * store it (when it may be), replace it with its view, and finish it under
 * the view's header. */
static void view_one(const stage_t *st,
                     const tool_call_t *call,
                     const tool_metadata_t *meta,
                     tool_result_t *r,
                     size_t share) {
   /* The conversation's tag masked first: the view, the stored copy and the
    * cached tree all come from the same text. */
   char *masked = strdup(tool_result_content(r));
   masked = masked ? session_prefix_mask_secret(st->session, masked) : NULL;
   if (!masked) {
      /* Out of memory: never the whole result in its place. */
      OLOG_ERROR("Tool view: out of memory viewing %s's result", call->name);
      too_large_to_show(r);
      llm_tools_finish_result(call, r, NULL);
      return;
   }
   const size_t len = strlen(masked);
   const size_t body_budget = share > LLM_TOOL_VIEWS_HEADER_MAX + LLM_TOOL_VIEW_MIN_BUDGET
                                  ? share - LLM_TOOL_VIEWS_HEADER_MAX
                                  : LLM_TOOL_VIEW_MIN_BUDGET;
   llm_tool_view_info_t info;
   struct json_object *tree = NULL;
   char *view = llm_tool_view_ex(masked, len, body_budget, NULL, &info, &tree);
   if (!view || !info.shortened) {
      free(masked);
      json_object_put(tree);
      if (view) {
         free(view); /* it fit after all: shown as it came */
      } else {
         OLOG_ERROR("Tool view: out of memory viewing %s's result", call->name);
         too_large_to_show(r);
      }
      llm_tools_finish_result(call, r, NULL);
      return;
   }
   const bool is_read = meta && meta->result_no_store;
   char handle[TOOL_RESULTS_ID_LEN] = "";
   bool head_tail_only = false;
   if (st->session && st->offers_read && !is_read) {
      /* An email's or a page's text keeps its frame: a read of it is framed
       * as the view is (a plan's output included). */
      const char *frame = tool_result_frame(r);
      const int rc = tool_result_store_put_framed(st->session, call->name, call->id, masked, len,
                                                  info.mode == LLM_TOOL_VIEW_JSON, frame, handle,
                                                  &head_tail_only);
      if (rc != TOOL_RESULT_STORE_OK) {
         handle[0] = '\0';
         if (rc == TOOL_RESULT_STORE_FAILED) {
            OLOG_WARNING("Tool view: %s's result wasn't stored; its view carries no handle",
                         call->name);
         }
      }
   }
   free(masked);
   if (tree && handle[0]) {
      tool_result_store_tree_seed(st->session, handle, tree, len); /* takes it */
   } else {
      json_object_put(tree);
   }
   const size_t view_bytes = strlen(view);
   const llm_tool_views_header_t h = {
      .tag = st->tag,
      .chars = len,
      .handle = handle[0] ? handle : NULL,
      .json = info.mode == LLM_TOOL_VIEW_JSON,
      .offers_read = st->offers_read,
      .head_tail_only = head_tail_only,
      .narrow = narrowing_param(meta),
      .is_read = is_read,
   };
   char header[LLM_TOOL_VIEWS_HEADER_MAX];
   if (llm_tool_views_header(&h, header, sizeof(header)) == 0) {
      /* Only a tag longer than any DAWN makes gets here. */
      snprintf(header, sizeof(header), "[Tool result shortened.]\n");
   }
   OLOG_INFO("Tool view: %s %zu bytes -> %zu bytes, %s, share %zu of %zu, %s", call->name, len,
             view_bytes, info.mode == LLM_TOOL_VIEW_JSON ? "json" : "text", share,
             st->budget->chars, handle[0] ? handle : "no handle");
   llm_tools_result_set_content(r, view);
   llm_tools_finish_result(call, r, header);
}

void llm_tool_views_finish_batch(const tool_call_list_t *calls,
                                 tool_result_list_t *results,
                                 void *batch) {
   const llm_tool_views_batch_t *b = batch;
   const int n = results->count < LLM_TOOL_VIEWS_BATCH_MAX ? results->count
                                                           : LLM_TOOL_VIEWS_BATCH_MAX;
   const tool_metadata_t *metas[LLM_TOOL_VIEWS_BATCH_MAX];
   size_t demands[LLM_TOOL_VIEWS_BATCH_MAX] = { 0 };
   bool first[LLM_TOOL_VIEWS_BATCH_MAX] = { false };
   size_t shares[LLM_TOOL_VIEWS_BATCH_MAX];
   size_t sizes[LLM_TOOL_VIEWS_BATCH_MAX];
   bool any_over = false;
   for (int i = 0; i < n; i++) {
      const tool_result_t *r = &results->results[i];
      metas[i] = tool_registry_find(calls->calls[i].name);
      sizes[i] = strlen(tool_result_content(r));
      first[i] = false;
      if (left_alone(r, metas[i])) {
         demands[i] = SIZE_MAX;
         continue;
      }
      /* A tool that asks to be shown whole up to a size is served first when
       * its result is within it. */
      const size_t whole_up_to = metas[i] ? metas[i]->max_result_chars : 0;
      demands[i] = sizes[i];
      first[i] = whole_up_to > 0 && sizes[i] <= whole_up_to;
   }
   llm_tool_views_split(demands, first, n, b->budget->chars, shares);
   size_t total = 0;
   int over = 0;
   for (int i = 0; i < n; i++) {
      total += demands[i] != SIZE_MAX ? sizes[i] : 0;
      if (shares[i] != SIZE_MAX && shares[i] < sizes[i]) {
         over++;
      }
   }
   any_over = over > 0;
   /* The batch's plan, for measuring the budget (and finding why a batch
    * wasn't shaped): only a batch large enough for it to matter. */
   if (total > b->budget->chars / 4) {
      OLOG_INFO("Tool views: batch of %d, %zu chars against a budget of %zu chars (window %d, "
                "room %d tokens): %d over their share",
                n, total, b->budget->chars, b->budget->window, b->budget->room_tokens, over);
   }

   stage_t st = { .session = b->session, .budget = b->budget };
   if (any_over) {
      /* Whether result_read is offered and would run, decided as execution
       * decides it: from the command context's surface. */
      session_t *ctx = session_get_command_context();
      const bool is_remote = ctx ? ctx->type != SESSION_TYPE_LOCAL
                                 : (b->session && b->session->type != SESSION_TYPE_LOCAL);
      st.offers_read = llm_tools_request_offers(b->params->conversation_history, is_remote,
                                                "result_read");
      if (!b->session || !session_prefix_tag(b->session, st.tag, sizeof(st.tag))) {
         st.tag[0] = '\0';
      }
   }
   for (int i = 0; i < n; i++) {
      tool_result_t *r = &results->results[i];
      if (shares[i] != SIZE_MAX && shares[i] < sizes[i]) {
         view_one(&st, &calls->calls[i], metas[i], r, shares[i]);
      } else {
         llm_tools_finish_result(&calls->calls[i], r, NULL);
      }
   }
   /* Anything past n is finished by llm_tools_execute_all as it came. */
}
