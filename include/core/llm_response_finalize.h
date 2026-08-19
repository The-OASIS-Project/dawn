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
 * Turns a completed raw LLM response into the CANONICAL CLEAN TEXT that every
 * surface (voice, WebUI, satellite, messaging, jobs, reinvoke) persists and
 * delivers.  Phase 0 strips residual non-user-facing tags — `<command>…
 * </command>` pairs and the `<end_of_turn>` local-model marker — and trims
 * trailing whitespace (Claude rejects assistant turns ending in whitespace).
 *
 * It does NOT execute anything (the legacy `<command>` transport was retired)
 * and does NOT touch TTS-specific cleaning (markdown `*`, emojis) — that stays
 * on the spoken-output path.  Phase 1 will add `<cited>` extraction plus a
 * citation audit/reinforce pass keyed on a per-turn stash carried on `session`;
 * the `session` parameter is reserved for that and unused in Phase 0.
 *
 * WEBUI-header-free by design: depends only on the session type and libc, so it
 * relocates cleanly if/when core session code leaves the ENABLE_WEBUI build
 * block.
 */
#ifndef LLM_RESPONSE_FINALIZE_H
#define LLM_RESPONSE_FINALIZE_H

#include <stddef.h>

#include "dawn_error.h" /* SUCCESS / FAILURE return contract */

/* Forward declaration only — Phase 0 passes the session pointer straight through
 * (unused).  Phase 1 will include core/session_manager.h in the .c to dereference
 * it for the per-turn citation stash.  Keeping it a forward decl here leaves this
 * header subsystem-neutral (libc + dawn_error only), so the module relocates
 * cleanly if/when core session code leaves the ENABLE_WEBUI build block. */
typedef struct session session_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of finalizing a completed LLM response.
 *
 * Phase 0 carries only the canonical clean text.  Phase 1 will grow citation
 * fields (cited ids, dropped count) alongside it.
 *
 * The clean text is ALLOCATED (not stripped in place) deliberately: Phase 1's
 * citation audit needs the raw and clean text to coexist, and the extra strdup
 * is negligible on this once-per-turn cold path — do not "optimize" it to
 * in-place.
 */
typedef struct {
   char *text;    /**< Owned canonical clean text; free via response_final_free(). */
   size_t length; /**< strlen(text). */
} response_final_t;

/**
 * @brief Produce canonical clean text from a completed LLM response.
 *
 * Strips `<command>…</command>` pairs and truncates at `<end_of_turn>`, then
 * trims trailing whitespace.  The input is not modified.
 *
 * @param session      Reserved for Phase-1 citation resolution; unused here.
 * @param raw_response  The completed LLM response text (NULL/empty → empty result).
 * @param out           Result; on SUCCESS out->text is allocated and non-NULL.
 * @return SUCCESS with out populated, or FAILURE with out zeroed (alloc failure).
 */
int llm_response_finalize(session_t *session, const char *raw_response, response_final_t *out);

/**
 * @brief Free the text owned by a response_final_t and zero it.  NULL-safe.
 */
void response_final_free(response_final_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LLM_RESPONSE_FINALIZE_H */
