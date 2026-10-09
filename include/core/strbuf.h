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
 * Growable string-builder for assembling formatted output without silent
 * truncation.  Complements buf_printf.h's BUF_PRINTF macro: that one is for
 * truly fixed-size stack buffers; this is for heap output where the final
 * size is data-dependent and dropping content is a correctness bug
 * (e.g., the memory_callback search/recent response, where silently elided
 * facts confuse the LLM consumer).
 *
 * Uses the doubling-realloc pattern from llm_streaming.c, with a hard
 * max_cap safety cap (default 256 KiB — comfortably above any realistic
 * memory tool / system-prompt response, but bounded so a runaway bug or
 * attack cannot exhaust memory).  Sticky OOM flag so a failed allocation
 * doesn't crash callers; subsequent appends become no-ops and the caller
 * checks `strbuf_oom()` before consuming `buf`.
 *
 * Not thread-safe; each strbuf_t is owned by one stack frame.
 */

#ifndef CORE_STRBUF_H
#define CORE_STRBUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* Default safety cap on growth.  256 KiB is well above any realistic memory
 * tool / system-prompt response (10 facts × 6 KiB excerpts + entity graph
 * fits comfortably under ~80 KB) but tight enough to fail-loud on a runaway
 * bug or attack.  Override per-init via strbuf_init_with_max() if a use case
 * legitimately needs more.  Tightened from 1 MiB on architecture-review
 * advice — earlier value was generous past the point of usefulness. */
#define STRBUF_DEFAULT_MAX_CAP (256UL * 1024UL)

/* Sentinel returned by strbuf_appendf / _append / _append_n when the buffer
 * is in OOM state (max_cap exhausted or allocation failed).  Distinguishes
 * OOM from a legitimate empty append (e.g., strbuf_appendf(sb, "%s", "")
 * which correctly returns 0).  Callers that want to act on OOM should test
 * for STRBUF_E_OOM (negative) rather than falsy. */
#define STRBUF_E_OOM (-1)

typedef struct {
   char *buf;      /* NUL-terminated; NULL only when len==0 && cap==0 */
   size_t len;     /* current length excluding NUL */
   size_t cap;     /* current capacity (always >= len + 1 unless oom) */
   size_t max_cap; /* hard upper bound on growth (0 = STRBUF_DEFAULT_MAX_CAP) */
   bool oom;       /* sticky: once set, all future appends become no-ops */
} strbuf_t;

/**
 * @brief Initialize a strbuf with an initial capacity hint.
 *
 * Lazy: no allocation happens until the first append.  Pass 0 for
 * @p initial_cap to defer entirely (recommended unless you know the
 * approximate size up front, in which case pass a value large enough
 * to avoid early doublings).
 *
 * @param sb Strbuf to initialize (must be non-NULL)
 * @param initial_cap Suggested starting capacity in bytes (0 = lazy)
 */
void strbuf_init(strbuf_t *sb, size_t initial_cap);

/**
 * @brief Initialize a strbuf with a custom max-cap safety bound.
 *
 * Use when the default 256 KiB cap is wrong for the situation: pass a
 * SMALLER cap for truly bounded uses that should fail-loud sooner; pass a
 * LARGER cap when the use case legitimately needs more (e.g., assembling a
 * long system prompt where the budget is data-driven).  Pass 0 for
 * @p max_cap to fall back to the default.
 *
 * @param sb Strbuf to initialize (must be non-NULL)
 * @param initial_cap Suggested starting capacity in bytes (0 = lazy)
 * @param max_cap Hard upper bound; appends fail (set oom) past this.
 *                Pass 0 to use STRBUF_DEFAULT_MAX_CAP (256 KiB).
 */
void strbuf_init_with_max(strbuf_t *sb, size_t initial_cap, size_t max_cap);

/**
 * @brief Free the strbuf's heap allocation.  Safe to call multiple times.
 */
void strbuf_free(strbuf_t *sb);

/**
 * @brief Append printf-formatted text to the buffer, growing as needed.
 *
 * @return Chars appended (excluding NUL).  May be 0 for a legitimate empty
 *         expansion (e.g., `strbuf_appendf(sb, "%s", "")`).  Returns
 *         @ref STRBUF_E_OOM (-1) when the buffer is in OOM state — sticky
 *         once set; subsequent appends are no-ops returning STRBUF_E_OOM.
 *         Callers that want to short-circuit downstream work on OOM should
 *         test `< 0` rather than falsy.
 */
int strbuf_appendf(strbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * @brief Append a NUL-terminated string verbatim.  Cheaper than appendf for
 *        no-format cases.
 *
 * @return Bytes appended; 0 for empty input; @ref STRBUF_E_OOM on OOM.
 */
int strbuf_append(strbuf_t *sb, const char *text);

/**
 * @brief Append @p len bytes verbatim (may include NULs in the middle, though
 *        the buffer's terminal NUL is still maintained).
 *
 * @return Bytes appended (always == @p len on success); 0 if @p len == 0;
 *         @ref STRBUF_E_OOM on OOM.
 */
int strbuf_append_n(strbuf_t *sb, const char *data, size_t len);

/**
 * @brief Did a previous append fail (allocation or max_cap exhaustion)?
 *
 * Once true, all subsequent appends are no-ops.  Caller must check this
 * before consuming the buffer if the failure mode matters.
 */
bool strbuf_oom(const strbuf_t *sb);

/**
 * @brief Current length excluding NUL.
 */
size_t strbuf_len(const strbuf_t *sb);

/**
 * @brief Current pointer to NUL-terminated buffer contents (read-only view).
 *
 * Returns "" (a static empty string) if the buffer has never been written to.
 * The returned pointer is invalidated by any subsequent append; do not store
 * it across appends.
 */
const char *strbuf_str(const strbuf_t *sb);

/**
 * @brief Transfer ownership of the heap buffer to the caller, returning a
 *        non-NULL string on success (including the empty-buffer case).
 *
 * Caller must `free()` the returned pointer.  After steal, the strbuf is
 * reset to its zero state (buf=NULL, len=cap=0, oom=false) and may be
 * re-used or freed without effect.
 *
 * If the buffer was never written to, returns a freshly-allocated empty
 * string (`strdup("")`) so callers don't need a separate empty-vs-success
 * branch.  Returns NULL ONLY in two cases: (a) the strbuf is in OOM state
 * (sticky from a prior failed append), or (b) the empty-string strdup
 * itself failed (true OOM).  Callers that need to distinguish these cases
 * can call @ref strbuf_oom() before stealing.
 *
 * Use @ref strbuf_steal_or_null when you specifically want NULL on the
 * empty-buffer case (rare; older API kept for callers that already check).
 */
char *strbuf_steal(strbuf_t *sb);

/**
 * @brief Like @ref strbuf_steal but returns NULL when the buffer is empty
 *        (never written) or in OOM state — caller can't distinguish these
 *        without a prior @ref strbuf_oom() check.
 */
char *strbuf_steal_or_null(strbuf_t *sb);

#endif /* CORE_STRBUF_H */
