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
 * Unified LLM response finalizer — see include/core/llm_response_finalize.h.
 */

#include "core/llm_response_finalize.h"

#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/text_filter.h"
#include "dawn_error.h"
#include "llm/llm_context_text.h"
#include "llm/llm_selection.h"
#include "logging.h"
#include "memory/memory_citation.h"

/* Trim trailing ASCII whitespace in place (Claude rejects assistant turns that
 * end in whitespace; harmless-to-beneficial for every other surface). */
static void rtrim(char *text) {
   size_t len = strlen(text);
   while (len > 0) {
      char c = text[len - 1];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
         break;
      }
      text[--len] = '\0';
   }
}

/* <end_of_turn> reaches a reply as text from a local model's server (a chat
 * template that doesn't stop on it, the model's next turn after it).  A
 * first-party cloud API ends the turn itself and never sends the marker, so there
 * it is a mention (a reply about chat templates) and stays.  Anything else (a
 * local model, OpenRouter, a custom endpoint, a turn the local LLM took over, no
 * session) is cut, as before. */
void llm_response_end_of_turn_strip(session_t *session, char *text) {
   if (text == NULL || strstr(text, END_OF_TURN_TAG) == NULL) {
      return; /* the common case: no marker, no need to know the model */
   }
   /* A turn the local LLM took over answers as a local model does, whatever
    * the session has selected. */
   if (session != NULL && !atomic_load(&session->turn_on_local_fallback)) {
      session_llm_config_t config;
      session_get_llm_config(session, &config);
      if (config.type == LLM_CLOUD && (config.cloud_provider == CLOUD_PROVIDER_CLAUDE ||
                                       config.cloud_provider == CLOUD_PROVIDER_OPENAI ||
                                       config.cloud_provider == CLOUD_PROVIDER_GEMINI)) {
         /* The provider's own API, not a custom endpoint (a local server
          * speaking its protocol). */
         const char *url = config.endpoint[0] != '\0' ? config.endpoint
                                                      : llm_cloud_url(config.cloud_provider);
         if (strcmp(url, llm_builtin_url(config.cloud_provider)) == 0) {
            return;
         }
      }
   }
   text_filter_end_of_turn_strip(text);
}

/* The cleanup of a reply whose <cited> tags are normalized: @p clean (taken).
 * With @p capture, its citations are recorded on the way.  Returns the cleaned
 * text (owned), or NULL on OOM. */
static char *clean_normalized(session_t *session, char *clean, bool capture) {
   /* The tags DAWN reads out of a reply come out next, before the reply is
    * defused: what stripping removes can't join a marker back together after
    * it was checked. */
   /* [emote:…] first (removing one joins its neighbours), and only on the local
    * voice surface: the one told to write them.  Elsewhere it is text (a reply
    * explaining the feature, a quoted message). */
   size_t openers = 0;
   for (const char *p = clean; (p = strstr(p, CITED_TAG_OPEN)) != NULL; p += CITED_TAG_OPEN_LEN) {
      openers++;
   }
   const size_t len_raw = strlen(clean);
   if (session != NULL && session->type == SESSION_TYPE_LOCAL) {
      text_filter_emote_strip(clean, NULL, 0);
   }
   const size_t len_emote = strlen(clean);
   llm_response_end_of_turn_strip(session, clean);
   const size_t len_eot = strlen(clean);
   /* Citations are read where the strip will act: a tag counts only at the end
    * of the reply, and the reply ends where the strips above left it.  (Self-
    * gating: a no-op unless citations are on and the session has a stash.) */
   if (capture) {
      memory_citation_capture(session, clean);
   }
   text_filter_cited_strip(clean); /* a trailing <cited> tag (on every surface) */
   /* What the strips took, by count: a reply that lost text to one says so in
    * the log, since the text as the model sent it is kept nowhere. */
   const size_t len_cited = strlen(clean);
   if (openers > 0 || len_cited != len_raw) {
      OLOG_INFO("finalize: %zu <cited> opener(s) in a %zu-byte reply; removed %zu bytes "
                "(emote %zu, end-of-turn %zu, cited %zu)",
                openers, len_raw, len_raw - len_cited, len_raw - len_emote, len_emote - len_eot,
                len_eot - len_cited);
   }

   /* What the model was led to write that imitates DAWN's framing, or carries
    * the conversation's tag, doesn't stay in the conversation to pass for
    * DAWN's on a later turn (the text is kept otherwise, byte for byte). */
   char *safe = session_prefix_mask_secret(session, llm_context_neutralize(clean));
   if (!safe) {
      free(clean);
      return NULL;
   }
   if (strcmp(safe, clean) != 0) {
      OLOG_WARNING(
          "finalize: the model's text imitated DAWN's framing or carried its tag; defused");
   }
   free(clean);
   rtrim(safe);
   return safe;
}

char *llm_response_clean(session_t *session, const char *raw) {
   char *clean = strdup(raw != NULL ? raw : "");
   if (clean == NULL) {
      return NULL;
   }
   text_filter_cited_normalize(clean);
   return clean_normalized(session, clean, false);
}

int llm_response_finalize(session_t *session, const char *raw_response, response_final_t *out) {
   if (out == NULL) {
      return FAILURE;
   }
   out->text = NULL;
   out->length = 0;

   char *clean = strdup(raw_response != NULL ? raw_response : "");
   if (clean == NULL) {
      return FAILURE;
   }

   /* Canonicalize whitespace-malformed citation tags (`< cited>`, `<cited >`, …)
    * BEFORE capture + strip: the exact-match grammar both paths use would otherwise
    * miss a space-bearing tag, so it would both lose its citation AND leak into the
    * persisted/rendered body. */
   text_filter_cited_normalize(clean);

   clean = clean_normalized(session, clean, true);
   if (clean == NULL) {
      return FAILURE;
   }
   out->text = clean;
   out->length = strlen(clean);
   return SUCCESS;
}

void response_final_free(response_final_t *out) {
   if (out == NULL) {
      return;
   }
   free(out->text);
   out->text = NULL;
   out->length = 0;
}
