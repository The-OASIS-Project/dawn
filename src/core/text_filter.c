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
 * Text filtering utilities for command tag stripping.
 */

#include "core/text_filter.h"

#include <string.h>

/* Use constants from header (CMD_TAG_OPEN, CMD_TAG_CLOSE, etc.) */

void text_filter_reset(cmd_tag_filter_state_t *state) {
   if (state) {
      memset(state, 0, sizeof(*state));
   }
}

void text_filter_command_tags(cmd_tag_filter_state_t *state,
                              const char *text,
                              text_filter_output_fn output_fn,
                              void *ctx) {
   if (!state || !text || !output_fn) {
      return;
   }

   const char *p = text;
   const char *output_start = NULL;

   while (*p) {
      if (state->nesting_depth > 0) {
         /* Inside command tag - look for <command> (nested) or </command> */
         if (*p == '<') {
            state->buffer[0] = '<';
            state->len = 1;
            p++;
            continue;
         }

         if (state->len > 0) {
            /* Bounds check for safety */
            if (state->len >= CMD_TAG_BUF_SIZE - 1) {
               state->len = 0;
               p++;
               continue;
            }

            /* Check for nested <command> opening tag */
            if (*p == CMD_TAG_OPEN[state->len]) {
               state->buffer[state->len++] = *p;

               if (state->len == CMD_TAG_OPEN_LEN) {
                  /* Found nested <command> - increase depth with overflow protection */
                  if (state->nesting_depth < CMD_TAG_MAX_NESTING) {
                     state->nesting_depth++;
                  }
                  /* Beyond max depth, stay at max (still filters content) */
                  state->len = 0;
               }
               p++;
               continue;
            }

            /* Check for </command> closing tag */
            if (*p == CMD_TAG_CLOSE[state->len]) {
               state->buffer[state->len++] = *p;

               if (state->len == CMD_TAG_CLOSE_LEN) {
                  /* Found </command> - decrease nesting depth */
                  state->nesting_depth--;
                  state->len = 0;
               }
               p++;
               continue;
            }

            /* Not matching either tag - reset buffer */
            state->len = 0;
            p++;
            continue;
         }

         /* Inside command, not matching any tag - discard */
         p++;
         continue;
      }

      /* Normal mode - look for <command> */
      if (*p == '<') {
         /* Flush pending output before buffering potential tag */
         if (output_start) {
            output_fn(output_start, (size_t)(p - output_start), ctx);
            output_start = NULL;
         }
         /* Emit any held partial-tag bytes before restarting on this '<': no tag
          * has an interior '<', so a held partial can never continue past one —
          * those bytes are confirmed non-tag text (fixes a dropped '<' on "<<command>"). */
         if (state->len > 0) {
            output_fn(state->buffer, state->len, ctx);
         }
         state->buffer[0] = '<';
         state->len = 1;
         p++;
         continue;
      }

      if (state->len > 0) {
         /* Bounds check for safety */
         if (state->len >= CMD_TAG_BUF_SIZE - 1) {
            output_fn(state->buffer, state->len, ctx);
            state->len = 0;
            continue;
         }

         /* Single-char comparison instead of strncmp */
         if (*p != CMD_TAG_OPEN[state->len]) {
            /* Not open tag - emit buffer and re-process current char */
            output_fn(state->buffer, state->len, ctx);
            state->len = 0;
            continue;
         }

         state->buffer[state->len++] = *p;

         if (state->len == CMD_TAG_OPEN_LEN) {
            /* Found <command> - enter command mode */
            state->nesting_depth = 1;
            state->len = 0;
         }
         p++;
         continue;
      }

      /* Normal character - add to output span */
      if (!output_start) {
         output_start = p;
      }
      p++;
   }

   /* Flush any remaining output */
   if (output_start) {
      output_fn(output_start, (size_t)(p - output_start), ctx);
   }
}

/* Context for buffer output callback */
typedef struct {
   char *buf;
   size_t size;
   size_t len;
} buffer_ctx_t;

/* Callback that appends to a buffer */
static void buffer_output_fn(const char *text, size_t len, void *ctx) {
   buffer_ctx_t *bc = (buffer_ctx_t *)ctx;
   if (!bc || !bc->buf || bc->size == 0) {
      return;
   }

   size_t remaining = bc->size - bc->len - 1;
   size_t to_copy = len < remaining ? len : remaining;

   if (to_copy > 0) {
      memcpy(bc->buf + bc->len, text, to_copy);
      bc->len += to_copy;
      bc->buf[bc->len] = '\0';
   }
}

int text_filter_command_tags_to_buffer(cmd_tag_filter_state_t *state,
                                       const char *text,
                                       char *out_buf,
                                       size_t out_size) {
   if (!state || !text || !out_buf || out_size == 0) {
      return 0;
   }

   buffer_ctx_t ctx = { .buf = out_buf, .size = out_size, .len = 0 };
   out_buf[0] = '\0';

   text_filter_command_tags(state, text, buffer_output_fn, &ctx);

   return (int)ctx.len;
}

/* =============================================================================
 * Memory-citation tag filter — strips <cited>…</cited> from streamed text.
 *
 * Same chunk-spanning discipline as the command filter, minus nesting: once the
 * opener is matched, ALL bytes are suppressed until the closer.  Mirrors the
 * command state machine so the restart-on-mismatch behavior is identical.
 * ============================================================================= */

void text_filter_cited_reset(cited_tag_filter_state_t *state) {
   if (state) {
      memset(state, 0, sizeof(*state));
   }
}

void text_filter_cited_tags(cited_tag_filter_state_t *state,
                            const char *text,
                            text_filter_output_fn output_fn,
                            void *ctx) {
   if (!state || !text || !output_fn) {
      return;
   }

   const char *p = text;
   const char *output_start = NULL;

   while (*p) {
      if (state->in_tag) {
         /* Inside <cited> — suppress everything, scan for </cited> (may split). */
         if (*p == '<') {
            /* Potential start of the closer; (re)begin matching it. */
            state->buffer[0] = '<';
            state->len = 1;
            p++;
            continue;
         }

         if (state->len > 0) {
            if (state->len >= CITED_TAG_BUF_SIZE - 1) {
               state->len = 0; /* overrun guard: not a closer, keep suppressing */
               p++;
               continue;
            }
            if (*p == CITED_TAG_CLOSE[state->len]) {
               state->buffer[state->len++] = *p;
               if (state->len == CITED_TAG_CLOSE_LEN) {
                  state->in_tag = false; /* closer complete — resume passthrough */
                  state->len = 0;
               }
               p++;
               continue;
            }
            state->len = 0; /* not the closer — discard, still suppressing */
            p++;
            continue;
         }

         p++; /* suppressed content byte */
         continue;
      }

      /* Passthrough mode — look for the <cited> opener. */
      if (*p == '<') {
         /* Flush pending output before buffering a potential tag. */
         if (output_start) {
            output_fn(output_start, (size_t)(p - output_start), ctx);
            output_start = NULL;
         }
         /* Emit any held partial-opener bytes before restarting on this '<' — a
          * new '<' can't continue the previous partial (no interior '<' in the
          * tag), so they're confirmed real text (fixes a dropped '<' on "<<cited>"). */
         if (state->len > 0) {
            output_fn(state->buffer, state->len, ctx);
         }
         state->buffer[0] = '<';
         state->len = 1;
         p++;
         continue;
      }

      if (state->len > 0) {
         if (state->len >= CITED_TAG_BUF_SIZE - 1) {
            output_fn(state->buffer, state->len, ctx); /* not a tag — release */
            state->len = 0;
            continue;
         }
         if (*p != CITED_TAG_OPEN[state->len]) {
            /* Partial match broke — emit held bytes and re-process current char
             * (a '<' among them restarts a fresh match, as in the command filter). */
            output_fn(state->buffer, state->len, ctx);
            state->len = 0;
            continue;
         }
         state->buffer[state->len++] = *p;
         if (state->len == CITED_TAG_OPEN_LEN) {
            state->in_tag = true; /* opener complete — begin suppressing */
            state->len = 0;
         }
         p++;
         continue;
      }

      /* Normal character — extend the passthrough span. */
      if (!output_start) {
         output_start = p;
      }
      p++;
   }

   if (output_start) {
      output_fn(output_start, (size_t)(p - output_start), ctx);
   }
}

int text_filter_cited_tags_to_buffer(cited_tag_filter_state_t *state,
                                     const char *text,
                                     char *out_buf,
                                     size_t out_size) {
   if (!state || !text || !out_buf || out_size == 0) {
      return 0;
   }

   buffer_ctx_t ctx = { .buf = out_buf, .size = out_size, .len = 0 };
   out_buf[0] = '\0';

   text_filter_cited_tags(state, text, buffer_output_fn, &ctx);

   return (int)ctx.len;
}

int text_filter_cited_flush_to_buffer(cited_tag_filter_state_t *state,
                                      char *out_buf,
                                      size_t out_size) {
   if (!state || !out_buf || out_size == 0) {
      return 0;
   }
   out_buf[0] = '\0';

   buffer_ctx_t ctx = { .buf = out_buf, .size = out_size, .len = 0 };
   /* A held partial OPENER (passthrough mode, len>0) is real text — emit it.
    * Mid-tag (in_tag) means the closer never arrived: drop the suppressed tail. */
   if (!state->in_tag && state->len > 0) {
      buffer_output_fn(state->buffer, state->len, &ctx);
   }
   text_filter_cited_reset(state);
   return (int)ctx.len;
}
