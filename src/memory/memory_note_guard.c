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
 * Note-extraction guard — see include/memory/memory_note_guard.h for the
 * rationale.  Two responsibilities: (1) COLLECT, from the full conversation
 * history, the bodies filed via the document_manage save_note/save_text tool
 * and the call-ids of document_read/document_search results that echo stored
 * text; (2) REDACT those bodies (and echoes) from a per-message copy at
 * extraction time.  Both provider history shapes are handled.
 */

#include "memory/memory_note_guard.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "logging.h"

/* Marker substituted for a filed body found in a message's text/content.  The
 * label form keeps the extraction LLM aware a note exists (harmless) while the
 * verbatim body is gone. */
#define GUARD_FILED_MARKER_PLAIN "[filed to notes store]"

/* Marker substituted for a document_read/document_search result that echoes
 * stored content.  Unconditional for those calls — it is our own deterministic
 * tool output mirroring text already in the document store. */
#define GUARD_ECHO_MARKER \
   "[stored document/note content elided - available verbatim via document_read]"

/* Minimum normalized length (chars) of a filed body before we will search for
 * it inside OTHER messages.  Reference text is many words; a very short filed
 * string ("Bob") could appear incidentally, so we do not scan for it.  The
 * structural redaction of the save tool-call's own `text` argument is
 * unconditional and does NOT consult this floor. */
#define GUARD_MIN_BODY_NORM_LEN 16

/* Safety bound on splice iterations per text (markers never contain a filed
 * body, so this is belt-and-suspenders against a pathological re-match, not an
 * expected limit). */
#define GUARD_MAX_REPLACEMENTS 64

/* Longest label echoed into a marker. */
#define GUARD_MARKER_LABEL_MAX 80

typedef struct {
   char *body;  /* filed note/text body (owned) */
   char *label; /* label, or "" when unlabeled (owned) */
   char *norm;  /* whitespace-normalized, lowercased body (owned) — the search
                 * needle, computed once at collect time; NULL on OOM */
   size_t nlen; /* length of norm; bodies with nlen < the floor are not scanned */
} guard_body_t;

struct memory_note_guard {
   guard_body_t *bodies; /* filed bodies + labels (owned) */
   int body_count;
   int body_cap;
   char **read_ids; /* tool_call_ids of document_read/search calls (owned) */
   int read_id_count;
   int read_id_cap;
};

/* =============================================================================
 * Collection
 * ============================================================================= */

static char *guard_normalize_ws(const char *src, size_t **map_out, size_t *norm_len_out);

static void guard_add_body(memory_note_guard_t *g, const char *body, const char *label) {
   if (!body || !*body) {
      return;
   }
   if (g->body_count == g->body_cap) {
      int new_cap = (g->body_cap == 0) ? 4 : (g->body_cap * 2);
      guard_body_t *grown = realloc(g->bodies, (size_t)new_cap * sizeof(guard_body_t));
      if (!grown) {
         return;
      }
      g->bodies = grown;
      g->body_cap = new_cap;
   }
   char *body_copy = strdup(body);
   char *label_copy = strdup(label ? label : "");
   if (!body_copy || !label_copy) {
      free(body_copy);
      free(label_copy);
      return;
   }
   /* Normalize the needle ONCE here, not per-haystack at search time.  The map
    * is only needed for haystack offset translation, so discard it.  On OOM the
    * body still enters the list (keeps the guard active for the structural
    * tool-arg redaction) but won't be scanned (norm == NULL). */
   size_t *nmap = NULL;
   size_t nlen = 0;
   char *norm = guard_normalize_ws(body, &nmap, &nlen);
   free(nmap);
   g->bodies[g->body_count].body = body_copy;
   g->bodies[g->body_count].label = label_copy;
   g->bodies[g->body_count].norm = norm;
   g->bodies[g->body_count].nlen = norm ? nlen : 0;
   g->body_count++;
}

static void guard_add_read_id(memory_note_guard_t *g, const char *id) {
   if (!id || !*id) {
      return;
   }
   if (g->read_id_count == g->read_id_cap) {
      int new_cap = (g->read_id_cap == 0) ? 4 : (g->read_id_cap * 2);
      char **grown = realloc(g->read_ids, (size_t)new_cap * sizeof(char *));
      if (!grown) {
         return;
      }
      g->read_ids = grown;
      g->read_id_cap = new_cap;
   }
   char *copy = strdup(id);
   if (!copy) {
      return;
   }
   g->read_ids[g->read_id_count++] = copy;
}

static bool guard_is_read_id(const memory_note_guard_t *g, const char *id) {
   if (!id) {
      return false;
   }
   for (int i = 0; i < g->read_id_count; i++) {
      if (strcmp(g->read_ids[i], id) == 0) {
         return true;
      }
   }
   return false;
}

/* Given a tool name, its call id, and its parsed input object, record anything
 * relevant.  Shared by both provider shapes (OpenAI parses the arguments string
 * into `input` first; Claude passes `input` directly). */
static void guard_collect_tool(memory_note_guard_t *g,
                               const char *name,
                               const char *id,
                               struct json_object *input) {
   if (!name) {
      return;
   }
   if (strcmp(name, "document_manage") == 0) {
      if (!input || !json_object_is_type(input, json_type_object)) {
         return;
      }
      struct json_object *action_obj = NULL;
      if (!json_object_object_get_ex(input, "action", &action_obj)) {
         return;
      }
      const char *action = json_object_get_string(action_obj);
      if (!action) {
         return;
      }
      struct json_object *label_obj = NULL;
      const char *label = NULL;
      if (json_object_object_get_ex(input, "label", &label_obj)) {
         label = json_object_get_string(label_obj);
      }
      /* The content-bearing arg differs by action: save_note/save_text/append
       * carry it in 'text'; edit carries the new text in change.replace (a JSON
       * object the LLM passes as a string).  All of it is note content that must
       * NOT be re-mined into memory at session end.
       * NOTE: the action→content-field map is also encoded in
       * guard_redact_save_input() (which blanks the field in the tool-call args) —
       * a new content-bearing action must be added in BOTH places. */
      if (strcmp(action, "save_note") == 0 || strcmp(action, "save_text") == 0 ||
          strcmp(action, "append") == 0) {
         struct json_object *text_obj = NULL;
         const char *text = NULL;
         if (json_object_object_get_ex(input, "text", &text_obj)) {
            text = json_object_get_string(text_obj);
         }
         guard_add_body(g, text, label);
      } else if (strcmp(action, "edit") == 0) {
         struct json_object *change_obj = NULL;
         if (json_object_object_get_ex(input, "change", &change_obj)) {
            /* 'change' may arrive as a JSON string (the declared shape) or, from a
             * lenient provider, already an object — handle both. */
            struct json_object *parsed = NULL;
            bool owned = false;
            if (json_object_is_type(change_obj, json_type_string)) {
               parsed = json_tokener_parse(json_object_get_string(change_obj));
               owned = true;
            } else if (json_object_is_type(change_obj, json_type_object)) {
               parsed = change_obj;
            }
            /* Register BOTH replace (the new content) and find (the located
             * snippet — existing note text the model copied verbatim) so neither
             * survives in a user turn to be re-mined. */
            struct json_object *replace_obj = NULL;
            if (parsed && json_object_object_get_ex(parsed, "replace", &replace_obj)) {
               guard_add_body(g, json_object_get_string(replace_obj), label);
            }
            struct json_object *find_obj = NULL;
            if (parsed && json_object_object_get_ex(parsed, "find", &find_obj)) {
               guard_add_body(g, json_object_get_string(find_obj), label);
            }
            if (owned && parsed) {
               json_object_put(parsed);
            }
         }
      }
   } else if (strcmp(name, "document_read") == 0 || strcmp(name, "document_search") == 0) {
      guard_add_read_id(g, id);
   }
}

/* Collect from an OpenAI assistant message's tool_calls array. */
static void guard_collect_openai_tool_calls(memory_note_guard_t *g,
                                            struct json_object *tool_calls) {
   int n = json_object_array_length(tool_calls);
   for (int i = 0; i < n; i++) {
      struct json_object *tc = json_object_array_get_idx(tool_calls, i);
      if (!tc) {
         continue;
      }
      struct json_object *id_obj = NULL;
      const char *id = json_object_object_get_ex(tc, "id", &id_obj) ? json_object_get_string(id_obj)
                                                                    : NULL;
      struct json_object *func = NULL;
      if (!json_object_object_get_ex(tc, "function", &func)) {
         continue;
      }
      struct json_object *name_obj = NULL;
      const char *name = json_object_object_get_ex(func, "name", &name_obj)
                             ? json_object_get_string(name_obj)
                             : NULL;
      struct json_object *args_obj = NULL;
      const char *args = json_object_object_get_ex(func, "arguments", &args_obj)
                             ? json_object_get_string(args_obj)
                             : NULL;
      struct json_object *parsed = args ? json_tokener_parse(args) : NULL;
      guard_collect_tool(g, name, id, parsed);
      if (parsed) {
         json_object_put(parsed);
      }
   }
}

/* Collect from a Claude content array (tool_use blocks). */
static void guard_collect_claude_content(memory_note_guard_t *g, struct json_object *content) {
   int n = json_object_array_length(content);
   for (int i = 0; i < n; i++) {
      struct json_object *block = json_object_array_get_idx(content, i);
      if (!block || !json_object_is_type(block, json_type_object)) {
         continue;
      }
      struct json_object *type_obj = NULL;
      if (!json_object_object_get_ex(block, "type", &type_obj)) {
         continue;
      }
      const char *type = json_object_get_string(type_obj);
      if (!type || strcmp(type, "tool_use") != 0) {
         continue;
      }
      struct json_object *id_obj = NULL;
      const char *id = json_object_object_get_ex(block, "id", &id_obj)
                           ? json_object_get_string(id_obj)
                           : NULL;
      struct json_object *name_obj = NULL;
      const char *name = json_object_object_get_ex(block, "name", &name_obj)
                             ? json_object_get_string(name_obj)
                             : NULL;
      struct json_object *input = NULL;
      json_object_object_get_ex(block, "input", &input);
      guard_collect_tool(g, name, id, input);
   }
}

static void guard_collect_message(memory_note_guard_t *g, struct json_object *msg) {
   struct json_object *tool_calls = NULL;
   if (json_object_object_get_ex(msg, "tool_calls", &tool_calls) &&
       json_object_is_type(tool_calls, json_type_array)) {
      guard_collect_openai_tool_calls(g, tool_calls); /* OpenAI assistant */
   }
   struct json_object *content = NULL;
   if (json_object_object_get_ex(msg, "content", &content) &&
       json_object_is_type(content, json_type_array)) {
      guard_collect_claude_content(g, content); /* Claude assistant tool_use */
   }
}

/* =============================================================================
 * Whitespace-normalized exact-substring matching
 * ============================================================================= */

/* Build a whitespace-collapsed, lowercased copy of @p src plus a map from each
 * normalized char index to its byte offset in @p src.  Leading/trailing
 * whitespace is dropped; internal whitespace runs collapse to a single space
 * mapped to the run's first byte.  map[norm_len] == strlen(src).  Caller frees
 * both *norm and *map. */
static char *guard_normalize_ws(const char *src, size_t **map_out, size_t *norm_len_out) {
   size_t slen = strlen(src);
   char *norm = malloc(slen + 1);
   size_t *map = malloc((slen + 1) * sizeof(size_t));
   if (!norm || !map) {
      free(norm);
      free(map);
      return NULL;
   }
   size_t n = 0;
   bool pending_space = false;
   size_t pending_space_off = 0;
   for (size_t i = 0; i < slen; i++) {
      unsigned char c = (unsigned char)src[i];
      if (isspace(c)) {
         if (!pending_space) {
            pending_space = true;
            pending_space_off = i;
         }
         continue;
      }
      if (pending_space && n > 0) { /* collapse run to one space (skip if leading) */
         map[n] = pending_space_off;
         norm[n++] = ' ';
      }
      pending_space = false;
      map[n] = i;
      norm[n++] = (char)tolower(c);
   }
   norm[n] = '\0';
   map[n] = slen;
   *map_out = map;
   *norm_len_out = n;
   return norm;
}

/* Find the leftmost (in original-byte terms) occurrence in @p text of any filed
 * body at/above the length floor.  On success sets the byte span [*start,*end)
 * in @p text and the matching label. */
static bool guard_find_earliest_body(const memory_note_guard_t *g,
                                     const char *text,
                                     size_t *start_out,
                                     size_t *end_out,
                                     const char **label_out) {
   size_t *hmap = NULL;
   size_t hlen = 0;
   char *hnorm = guard_normalize_ws(text, &hmap, &hlen);
   if (!hnorm) {
      return false;
   }
   bool found = false;
   size_t best_start = 0;
   size_t best_end = 0;
   const char *best_label = NULL;
   for (int b = 0; b < g->body_count; b++) {
      const char *nnorm = g->bodies[b].norm; /* pre-normalized at collect time */
      size_t nlen = g->bodies[b].nlen;
      if (!nnorm || nlen < GUARD_MIN_BODY_NORM_LEN) {
         continue;
      }
      char *hit = strstr(hnorm, nnorm);
      if (hit) {
         size_t a = (size_t)(hit - hnorm);
         size_t s = hmap[a];
         size_t e = hmap[a + nlen]; /* a + nlen <= hlen */
         if (!found || s < best_start) {
            found = true;
            best_start = s;
            best_end = e;
            best_label = g->bodies[b].label;
         }
      }
   }
   free(hnorm);
   free(hmap);
   if (found) {
      *start_out = best_start;
      *end_out = best_end;
      *label_out = best_label;
   }
   return found;
}

/* Compose the filed-body marker, sanitizing control chars out of the label. */
static void guard_build_marker(char *buf, size_t buflen, const char *label) {
   if (!label || !*label) {
      snprintf(buf, buflen, "%s", GUARD_FILED_MARKER_PLAIN);
      return;
   }
   char clean[GUARD_MARKER_LABEL_MAX + 1];
   size_t j = 0;
   for (size_t i = 0; label[i] != '\0' && j < GUARD_MARKER_LABEL_MAX; i++) {
      unsigned char c = (unsigned char)label[i];
      clean[j++] = (c < 0x20 || c == '\'') ? ' ' : (char)c;
   }
   clean[j] = '\0';
   snprintf(buf, buflen, "[filed to notes store: '%s']", clean);
}

/* Return a malloc'd copy of @p text with every filed body replaced by its
 * marker, or NULL when no body matches (caller keeps the original). */
static char *guard_redact_bodies_in_text(const memory_note_guard_t *g, const char *text) {
   if (g->body_count == 0) {
      return NULL;
   }
   /* cur is lazily allocated on the first match: NULL return = no change (caller
    * shares the original).  hay points at the original until the first splice,
    * then at cur — text itself is never mutated.  One find per iteration. */
   char *cur = NULL;
   const char *hay = text;
   for (int iter = 0; iter < GUARD_MAX_REPLACEMENTS; iter++) {
      size_t s = 0;
      size_t e = 0;
      const char *label = NULL;
      if (!guard_find_earliest_body(g, hay, &s, &e, &label)) {
         break;
      }
      char marker[GUARD_MARKER_LABEL_MAX + 48];
      guard_build_marker(marker, sizeof(marker), label);
      size_t mlen = strlen(marker);
      size_t haylen = strlen(hay);
      size_t tail = haylen - e;
      char *next = malloc(s + mlen + tail + 1);
      if (!next) {
         return cur; /* partial (or NULL = no change); cur stays owned */
      }
      memcpy(next, hay, s);
      memcpy(next + s, marker, mlen);
      memcpy(next + s + mlen, hay + e, tail);
      next[s + mlen + tail] = '\0';
      free(cur); /* free(NULL) on the first iteration is fine */
      cur = next;
      hay = cur;
   }
   return cur;
}

/* =============================================================================
 * Redaction — message rebuild
 * ============================================================================= */

/* If @p input is a document_manage call that carries note content, return a new
 * object copying its fields with the content field replaced by the plain marker;
 * else NULL (no change).  The content field is `text` for save_note/save_text/
 * append and `change` for edit (the new text lives in change.replace).
 * NOTE: the action→content-field map mirrors guard_collect_tool() — a new
 * content-bearing action must be added in BOTH places. */
static struct json_object *guard_redact_save_input(struct json_object *input) {
   if (!input || !json_object_is_type(input, json_type_object)) {
      return NULL;
   }
   struct json_object *action_obj = NULL;
   if (!json_object_object_get_ex(input, "action", &action_obj)) {
      return NULL;
   }
   const char *action = json_object_get_string(action_obj);
   if (!action) {
      return NULL;
   }
   const char *content_key = NULL;
   if (strcmp(action, "save_note") == 0 || strcmp(action, "save_text") == 0 ||
       strcmp(action, "append") == 0) {
      content_key = "text";
   } else if (strcmp(action, "edit") == 0) {
      content_key = "change";
   } else {
      return NULL;
   }
   struct json_object *content_obj = NULL;
   if (!json_object_object_get_ex(input, content_key, &content_obj)) {
      return NULL; /* nothing to hide */
   }
   const char *content = json_object_get_string(content_obj);
   if (!content || !*content) {
      return NULL;
   }
   struct json_object *out = json_object_new_object();
   if (!out) {
      return NULL;
   }
   json_object_object_foreach(input, key, val) {
      if (strcmp(key, content_key) == 0) {
         json_object_object_add(out, key, json_object_new_string(GUARD_FILED_MARKER_PLAIN));
      } else {
         json_object_object_add(out, key, json_object_get(val));
      }
   }
   return out;
}

/* Redact an OpenAI tool_calls array.  On change, sets *out to a new array and
 * returns true; else returns false (*out untouched). */
static bool guard_redact_tool_calls(struct json_object *tool_calls, struct json_object **out) {
   int n = json_object_array_length(tool_calls);
   struct json_object *rebuilt = NULL;
   for (int i = 0; i < n; i++) {
      struct json_object *tc = json_object_array_get_idx(tool_calls, i);
      struct json_object *new_tc = NULL;
      struct json_object *func = NULL;
      if (tc && json_object_object_get_ex(tc, "function", &func)) {
         struct json_object *args_obj = NULL;
         if (json_object_object_get_ex(func, "arguments", &args_obj)) {
            const char *args = json_object_get_string(args_obj);
            struct json_object *parsed = args ? json_tokener_parse(args) : NULL;
            struct json_object *red = parsed ? guard_redact_save_input(parsed) : NULL;
            if (red) {
               const char *red_str = json_object_to_json_string_ext(red, JSON_C_TO_STRING_PLAIN);
               struct json_object *new_func = json_object_new_object();
               json_object_object_foreach(func, fkey, fval) {
                  if (strcmp(fkey, "arguments") == 0) {
                     json_object_object_add(new_func, fkey, json_object_new_string(red_str));
                  } else {
                     json_object_object_add(new_func, fkey, json_object_get(fval));
                  }
               }
               new_tc = json_object_new_object();
               json_object_object_foreach(tc, tkey, tval) {
                  if (strcmp(tkey, "function") == 0) {
                     json_object_object_add(new_tc, tkey, new_func);
                  } else {
                     json_object_object_add(new_tc, tkey, json_object_get(tval));
                  }
               }
               json_object_put(red);
            }
            if (parsed) {
               json_object_put(parsed);
            }
         }
      }
      if (new_tc && !rebuilt) {
         /* first change — back-fill the array with prior (unchanged) entries */
         rebuilt = json_object_new_array();
         for (int j = 0; j < i; j++) {
            json_object_array_add(rebuilt,
                                  json_object_get(json_object_array_get_idx(tool_calls, j)));
         }
      }
      if (rebuilt) {
         json_object_array_add(rebuilt, new_tc ? new_tc : json_object_get(tc));
      } else if (new_tc) {
         json_object_put(new_tc); /* unreachable: rebuilt is set on first change */
      }
   }
   if (rebuilt) {
      *out = rebuilt;
      return true;
   }
   return false;
}

/* Redact a content array (Claude assistant tool_use/text blocks, or a Claude
 * tool_result user message).  On change sets *out and returns true. */
static bool guard_redact_content_array(const memory_note_guard_t *g,
                                       struct json_object *content,
                                       struct json_object **out) {
   int n = json_object_array_length(content);
   struct json_object *rebuilt = NULL;
   for (int i = 0; i < n; i++) {
      struct json_object *block = json_object_array_get_idx(content, i);
      struct json_object *new_block = NULL;
      struct json_object *type_obj = NULL;
      if (block && json_object_is_type(block, json_type_object) &&
          json_object_object_get_ex(block, "type", &type_obj)) {
         const char *type = json_object_get_string(type_obj);
         if (!type) {
            /* Non-string "type" (e.g. JSON null) — leave the block untouched. */
         } else if (strcmp(type, "text") == 0) {
            struct json_object *txt_obj = NULL;
            if (json_object_object_get_ex(block, "text", &txt_obj)) {
               char *red = guard_redact_bodies_in_text(g, json_object_get_string(txt_obj));
               if (red) {
                  new_block = json_object_new_object();
                  json_object_object_foreach(block, bkey, bval) {
                     if (strcmp(bkey, "text") == 0) {
                        json_object_object_add(new_block, bkey, json_object_new_string(red));
                     } else {
                        json_object_object_add(new_block, bkey, json_object_get(bval));
                     }
                  }
                  free(red);
               }
            }
         } else if (strcmp(type, "tool_use") == 0) {
            struct json_object *input = NULL;
            json_object_object_get_ex(block, "input", &input);
            struct json_object *red = guard_redact_save_input(input);
            if (red) {
               new_block = json_object_new_object();
               json_object_object_foreach(block, bkey, bval) {
                  if (strcmp(bkey, "input") == 0) {
                     json_object_object_add(new_block, bkey, red);
                  } else {
                     json_object_object_add(new_block, bkey, json_object_get(bval));
                  }
               }
            }
         } else if (strcmp(type, "tool_result") == 0) {
            struct json_object *tuid = NULL;
            if (json_object_object_get_ex(block, "tool_use_id", &tuid) &&
                guard_is_read_id(g, json_object_get_string(tuid))) {
               new_block = json_object_new_object();
               json_object_object_foreach(block, bkey, bval) {
                  if (strcmp(bkey, "content") == 0) {
                     json_object_object_add(new_block, bkey,
                                            json_object_new_string(GUARD_ECHO_MARKER));
                  } else {
                     json_object_object_add(new_block, bkey, json_object_get(bval));
                  }
               }
            }
         }
      }
      if (new_block && !rebuilt) {
         rebuilt = json_object_new_array();
         for (int j = 0; j < i; j++) {
            json_object_array_add(rebuilt, json_object_get(json_object_array_get_idx(content, j)));
         }
      }
      if (rebuilt) {
         json_object_array_add(rebuilt, new_block ? new_block : json_object_get(block));
      }
   }
   if (rebuilt) {
      *out = rebuilt;
      return true;
   }
   return false;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

/* NOTE: the guard collects filed bodies only from STRUCTURED tool calls (OpenAI
 * `tool_calls` + Claude `tool_use` blocks) — the shape produced by native tool
 * calling, which is the only tool transport (the legacy `<command>`-tag path was
 * retired 2026-08). */
memory_note_guard_t *memory_note_guard_create(struct json_object *conversation_history) {
   if (!conversation_history || !g_config.memory.note_extraction_guard) {
      return NULL;
   }
   if (!json_object_is_type(conversation_history, json_type_array)) {
      return NULL;
   }
   memory_note_guard_t *g = calloc(1, sizeof(*g));
   if (!g) {
      return NULL;
   }
   int n = json_object_array_length(conversation_history);
   for (int i = 0; i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(conversation_history, i);
      if (msg && json_object_is_type(msg, json_type_object)) {
         guard_collect_message(g, msg);
      }
   }
   if (g->body_count == 0 && g->read_id_count == 0) {
      memory_note_guard_free(g); /* nothing to do — inert */
      return NULL;
   }
   OLOG_INFO("memory_note_guard: %d filed body(ies), %d read-echo id(s) to redact from extraction",
             g->body_count, g->read_id_count);
   return g;
}

bool memory_note_guard_active(const memory_note_guard_t *guard) {
   return guard && (guard->body_count > 0 || guard->read_id_count > 0);
}

struct json_object *memory_note_guard_redact(const memory_note_guard_t *g,
                                             struct json_object *msg) {
   if (!g || !msg || (g->body_count == 0 && g->read_id_count == 0)) {
      return json_object_get(msg);
   }

   struct json_object *role_obj = NULL;
   const char *role = json_object_object_get_ex(msg, "role", &role_obj)
                          ? json_object_get_string(role_obj)
                          : NULL;

   struct json_object *new_content = NULL;    /* owned when set */
   struct json_object *new_tool_calls = NULL; /* owned when set */

   struct json_object *content = NULL;
   if (json_object_object_get_ex(msg, "content", &content)) {
      if (json_object_is_type(content, json_type_string)) {
         const char *txt = json_object_get_string(content);
         struct json_object *tcid = NULL;
         if (role && strcmp(role, "tool") == 0 &&
             json_object_object_get_ex(msg, "tool_call_id", &tcid) &&
             guard_is_read_id(g, json_object_get_string(tcid))) {
            new_content = json_object_new_string(GUARD_ECHO_MARKER); /* OpenAI read echo */
         } else {
            char *red = guard_redact_bodies_in_text(g, txt);
            if (red) {
               new_content = json_object_new_string(red);
               free(red);
            }
         }
      } else if (json_object_is_type(content, json_type_array)) {
         struct json_object *rebuilt = NULL;
         if (guard_redact_content_array(g, content, &rebuilt)) {
            new_content = rebuilt;
         }
      }
   }

   struct json_object *tcs = NULL;
   if (json_object_object_get_ex(msg, "tool_calls", &tcs) &&
       json_object_is_type(tcs, json_type_array)) {
      struct json_object *rebuilt = NULL;
      if (guard_redact_tool_calls(tcs, &rebuilt)) {
         new_tool_calls = rebuilt;
      }
   }

   if (!new_content && !new_tool_calls) {
      return json_object_get(msg); /* nothing changed — share original */
   }

   struct json_object *out = json_object_new_object();
   if (!out) {
      if (new_content) {
         json_object_put(new_content);
      }
      if (new_tool_calls) {
         json_object_put(new_tool_calls);
      }
      return json_object_get(msg); /* OOM — share original (unredacted but valid) */
   }
   json_object_object_foreach(msg, key, val) {
      if (new_content && strcmp(key, "content") == 0) {
         json_object_object_add(out, key, new_content);
         new_content = NULL;
      } else if (new_tool_calls && strcmp(key, "tool_calls") == 0) {
         json_object_object_add(out, key, new_tool_calls);
         new_tool_calls = NULL;
      } else {
         json_object_object_add(out, key, json_object_get(val));
      }
   }
   if (new_content) {
      json_object_put(new_content); /* key absent (shouldn't happen) — avoid leak */
   }
   if (new_tool_calls) {
      json_object_put(new_tool_calls);
   }
   return out;
}

void memory_note_guard_free(memory_note_guard_t *guard) {
   if (!guard) {
      return;
   }
   for (int i = 0; i < guard->body_count; i++) {
      free(guard->bodies[i].body);
      free(guard->bodies[i].label);
      free(guard->bodies[i].norm);
   }
   free(guard->bodies);
   for (int i = 0; i < guard->read_id_count; i++) {
      free(guard->read_ids[i]);
   }
   free(guard->read_ids);
   free(guard);
}
