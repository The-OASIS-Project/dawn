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
 * Unified LLM response finalizer — Layer 2 core.
 *
 * Turns model text into the CANONICAL CLEAN TEXT that every surface (voice,
 * WebUI, satellite, messaging, jobs, reinvoke) stores, delivers and sends back
 * to the model: the turn's final answer (llm_response_finalize) and what the
 * model said before a tool call (llm_response_clean).  It strips the tags DAWN
 * reads out of a reply (`[emote:…]` on the local voice surface, `<end_of_turn>`
 * from a model that writes it, a trailing `<cited>…</cited>`), defuses text imitating DAWN's
 * framing and masks the conversation's tag, and trims trailing whitespace (Claude rejects assistant
 * turns ending in whitespace).  Finalize also audits which surfaced `[M#]` items the answer cited
 * (`memory_citation_capture`, self-gating on `g_config.memory.citation_enabled`).
 *
 * It does NOT execute anything and does NOT touch TTS-specific cleaning
 * (markdown `*`, emojis) — that stays on the spoken-output path.
 *
 * WEBUI-header-free by design: depends only on the session type and libc, so it
 * relocates cleanly if/when core session code leaves the ENABLE_WEBUI build
 * block.
 */
#ifndef LLM_RESPONSE_FINALIZE_H
#define LLM_RESPONSE_FINALIZE_H

#include <stddef.h>

#include "dawn_error.h" /* SUCCESS / FAILURE return contract */

/* Forward declaration only: the .c reads the session (its type, its tag, its
 * citation stash); a forward decl keeps this header subsystem-neutral (libc +
 * dawn_error only). */
typedef struct session session_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cut @p text at its first `<end_of_turn>` when @p session's model is one
 *        that writes the marker as text, in place.
 *
 * Local models (and OpenRouter, which fronts them) can pass the marker through
 * with more of the model's output after it; on the Claude, OpenAI and Gemini
 * APIs at their own endpoints it is only ever a mention and is kept.  A custom
 * endpoint (a local server speaking one of their protocols) or a NULL @p session
 * cuts.  Text without the marker returns at once, without reading the session.  For the response
 * cleanup here and the spoken-sentence paths (local voice, WebUI audio).
 *
 * @param session The turn's session (its resolved LLM selection), or NULL
 * @param text    NUL-terminated, mutable C string; NULL is tolerated (no-op)
 *
 * @locks session->llm_config_mutex, then g_config_rwlock (not held together);
 *        call with no session lock held
 */
void llm_response_end_of_turn_strip(session_t *session, char *text);

/**
 * @brief Result of finalizing a completed LLM response.
 *
 * The clean text is ALLOCATED (not stripped in place): the caller keeps the
 * raw response, and the strdup is negligible on this once-per-turn path.
 */
typedef struct {
   char *text;    /**< Owned canonical clean text; free via response_final_free(). */
   size_t length; /**< strlen(text). */
} response_final_t;

/**
 * @brief Produce canonical clean text from a completed LLM response.
 *
 * Captures the answer's memory citations, then the cleanup described above
 * (llm_response_clean).  The input is not modified.
 *
 * @param session      The turn's session (carries the citation stash); may be NULL
 *                     (citation capture then no-ops).
 * @param raw_response  The completed LLM response text (NULL/empty → empty result).
 * @param out           Result; on SUCCESS out->text is allocated and non-NULL.
 * @return SUCCESS with out populated, or FAILURE with out zeroed (alloc failure).
 */
int llm_response_finalize(session_t *session, const char *raw_response, response_final_t *out);

/**
 * @brief The cleanup of llm_response_finalize, without the citation capture.
 *
 * For model text that is stored and replayed but isn't the turn's answer: what
 * the model says before a tool call.  Its `<cited>` tags are removed and not
 * counted (the capture audits the final answer).
 *
 * @param session  The turn's session (its type picks the expression-tag rule, its
 *                 tag is masked); may be NULL.
 * @param raw      The model's text (NULL → empty result).
 * @return The cleaned text (caller frees), or NULL on allocation failure.
 */
char *llm_response_clean(session_t *session, const char *raw);

/**
 * @brief Free the text owned by a response_final_t and zero it.  NULL-safe.
 */
void response_final_free(response_final_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LLM_RESPONSE_FINALIZE_H */
