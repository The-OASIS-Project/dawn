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
 * truncation.  See include/core/strbuf.h for interface details and rationale.
 */

#include "core/strbuf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

/* Lower bound on initial capacity — any caller asking for less rounds up so
 * the first append doesn't trigger immediate doubling overhead. */
#define STRBUF_MIN_INITIAL_CAP 64

void strbuf_init(strbuf_t *sb, size_t initial_cap) {
   strbuf_init_with_max(sb, initial_cap, 0);
}

void strbuf_init_with_max(strbuf_t *sb, size_t initial_cap, size_t max_cap) {
   if (!sb)
      return;
   sb->buf = NULL;
   sb->len = 0;
   sb->cap = 0;
   sb->max_cap = (max_cap > 0) ? max_cap : STRBUF_DEFAULT_MAX_CAP;
   sb->oom = false;

   /* Honor non-zero initial_cap — caller's hint that they expect at least
    * that much growth, so eat the alloc up front rather than doubling later. */
   if (initial_cap > 0) {
      size_t want = (initial_cap < STRBUF_MIN_INITIAL_CAP) ? STRBUF_MIN_INITIAL_CAP : initial_cap;
      if (want > sb->max_cap)
         want = sb->max_cap;
      sb->buf = malloc(want);
      if (sb->buf) {
         sb->buf[0] = '\0';
         sb->cap = want;
      } else {
         sb->oom = true;
         OLOG_ERROR("strbuf: initial alloc failed (cap=%zu)", want);
      }
   }
}

void strbuf_free(strbuf_t *sb) {
   if (!sb)
      return;
   free(sb->buf);
   sb->buf = NULL;
   sb->len = 0;
   sb->cap = 0;
   sb->oom = false;
}

/* Ensure capacity for at least @p needed bytes total (including the NUL
 * terminator).  Doubles current cap until satisfied; returns 1 on success
 * or 0 on OOM / max_cap exhaustion (and sets sb->oom). */
static int strbuf_ensure(strbuf_t *sb, size_t needed) {
   if (sb->oom)
      return 0;
   if (needed <= sb->cap)
      return 1;

   /* Hard cap: refuse to grow past max_cap. */
   if (needed > sb->max_cap) {
      sb->oom = true;
      OLOG_ERROR("strbuf: needed %zu exceeds max_cap %zu — appends will be dropped", needed,
                 sb->max_cap);
      return 0;
   }

   /* Doubling growth, capped at max_cap. */
   size_t new_cap = (sb->cap == 0) ? STRBUF_MIN_INITIAL_CAP : sb->cap * 2;
   while (new_cap < needed) {
      /* Check overflow before doubling: if doubling would overflow size_t,
       * jump straight to max_cap (we know needed <= max_cap from the check
       * above, so this still satisfies the request). */
      if (new_cap > SIZE_MAX / 2) {
         new_cap = sb->max_cap;
         break;
      }
      new_cap *= 2;
   }
   if (new_cap > sb->max_cap)
      new_cap = sb->max_cap;

   char *grown = realloc(sb->buf, new_cap);
   if (!grown) {
      sb->oom = true;
      OLOG_ERROR("strbuf: realloc(%zu) failed", new_cap);
      return 0;
   }
   /* When growing from zero, ensure the new buffer is NUL-terminated so the
    * strbuf_str() contract holds even before any data is written. */
   if (sb->cap == 0)
      grown[0] = '\0';
   sb->buf = grown;
   sb->cap = new_cap;
   return 1;
}

int strbuf_appendf(strbuf_t *sb, const char *fmt, ...) {
   if (!sb || !fmt)
      return STRBUF_E_OOM;
   if (sb->oom)
      return STRBUF_E_OOM;

   /* Try a stack buffer first; covers the typical short-format hot path
    * (e.g., 80-byte fact lines) with a single vsnprintf instead of two.
    * Only fall back to the probe-then-write path on overflow.  Saves
    * ~50-200 ns per call on Cortex-A78AE (per embedded-efficiency review M1). */
   char stack_buf[256];
   va_list ap, ap_copy;
   va_start(ap, fmt);
   va_copy(ap_copy, ap);
   int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
   va_end(ap);

   if (needed < 0) {
      va_end(ap_copy);
      return STRBUF_E_OOM; /* encoding error treated as OOM (rare; libc bug) */
   }

   if ((size_t)needed < sizeof(stack_buf)) {
      /* Fit in stack buffer — no second vsnprintf needed.  memcpy into the
       * grown strbuf at sb->len. */
      va_end(ap_copy);
      if (!strbuf_ensure(sb, sb->len + (size_t)needed + 1))
         return STRBUF_E_OOM;
      memcpy(sb->buf + sb->len, stack_buf, (size_t)needed);
      sb->len += (size_t)needed;
      sb->buf[sb->len] = '\0';
      return needed;
   }

   /* Stack overflow: must go to two-pass path with the actual size. */
   if (!strbuf_ensure(sb, sb->len + (size_t)needed + 1)) {
      va_end(ap_copy);
      return STRBUF_E_OOM;
   }

   int written = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap_copy);
   va_end(ap_copy);
   if (written < 0)
      return STRBUF_E_OOM;
   sb->len += (size_t)written;
   return written;
}

int strbuf_append(strbuf_t *sb, const char *text) {
   if (!sb || !text)
      return STRBUF_E_OOM;
   if (sb->oom)
      return STRBUF_E_OOM;
   if (*text == '\0')
      return 0; /* legitimate empty input */
   return strbuf_append_n(sb, text, strlen(text));
}

int strbuf_append_n(strbuf_t *sb, const char *data, size_t len) {
   if (!sb || !data)
      return STRBUF_E_OOM;
   if (sb->oom)
      return STRBUF_E_OOM;
   if (len == 0)
      return 0; /* legitimate empty input */
   if (!strbuf_ensure(sb, sb->len + len + 1))
      return STRBUF_E_OOM;
   memcpy(sb->buf + sb->len, data, len);
   sb->len += len;
   sb->buf[sb->len] = '\0';
   return (int)len;
}

bool strbuf_oom(const strbuf_t *sb) {
   return sb ? sb->oom : true;
}

size_t strbuf_len(const strbuf_t *sb) {
   return sb ? sb->len : 0;
}

const char *strbuf_str(const strbuf_t *sb) {
   if (!sb || !sb->buf)
      return "";
   return sb->buf;
}

char *strbuf_steal_or_null(strbuf_t *sb) {
   if (!sb || sb->oom || !sb->buf)
      return NULL;
   char *out = sb->buf;
   sb->buf = NULL;
   sb->len = 0;
   sb->cap = 0;
   sb->oom = false;
   return out;
}

char *strbuf_steal(strbuf_t *sb) {
   if (!sb || sb->oom)
      return NULL;
   if (!sb->buf) {
      /* Empty buffer (never written) — give the caller a freshly-allocated
       * empty string so they don't need to special-case NULL.  strdup may
       * itself fail on true OOM; in that case NULL bubbles up and the
       * caller's existing OOM-handling kicks in. */
      return strdup("");
   }
   char *out = sb->buf;
   sb->buf = NULL;
   sb->len = 0;
   sb->cap = 0;
   sb->oom = false;
   return out;
}
