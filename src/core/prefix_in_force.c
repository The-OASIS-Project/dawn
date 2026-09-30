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
 * What is in force in a conversation whose system prompt is frozen
 * (prefix_in_force.h).
 *
 * The record, on the prefix message under LLM_HISTORY_IN_FORCE_KEY:
 *   {"tag":          "<the conversation's tag>",
 *    "sections":     {"<name>": {"h": "<sha256 of its text>", "t": "<title>"}, ...},
 *    "previous":     {"<name>": "<the hash it held before its last change>", ...},
 *    "directives":   "<sha256 of the standing directions in force>",
 *    "tool_schemas": {"<frozen tool>": "<sha256 of its schema>", ...}}
 * A section's hash is of its text as built (the tag placeholder unfilled).
 */

#define _GNU_SOURCE /* memmem */
#include "core/prefix_in_force.h"

#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "core/hash_util.h"
#include "core/strbuf.h"
#include "llm/llm_history_kind.h"
#include "logging.h"

/* The standing directions when a surface has none (after one that had some). */
#define DIRECTIVES_NONE "No standing directions apply to this surface now."

static void hash_text(const char *text, char out[DAWN_SHA256_HEX_LEN]) {
   dawn_sha256_hex(text ? text : "", text ? strlen(text) : 0, out);
}

static const char *str_of(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) && json_object_is_type(v, json_type_string)
              ? json_object_get_string(v)
              : NULL;
}

static struct json_object *prefix_of(struct json_object *hist) {
   if (!json_object_is_type(hist, json_type_array) || json_object_array_length(hist) == 0) {
      return NULL;
   }
   struct json_object *first = json_object_array_get_idx(hist, 0);
   return llm_history_kind_of(first) == MESSAGE_KIND_PREFIX ? first : NULL;
}

/* The record on @p prefix, made when it has none. */
static struct json_object *record_of(struct json_object *prefix) {
   struct json_object *rec = NULL;
   if (json_object_object_get_ex(prefix, LLM_HISTORY_IN_FORCE_KEY, &rec) &&
       json_object_is_type(rec, json_type_object)) {
      return rec;
   }
   rec = json_object_new_object();
   if (rec) {
      json_object_object_add(prefix, LLM_HISTORY_IN_FORCE_KEY, rec);
   }
   return rec;
}

/* The object at @p key of @p rec, made when absent. */
static struct json_object *child(struct json_object *rec, const char *key) {
   struct json_object *obj = NULL;
   if (json_object_object_get_ex(rec, key, &obj) && json_object_is_type(obj, json_type_object)) {
      return obj;
   }
   obj = json_object_new_object();
   if (obj) {
      json_object_object_add(rec, key, obj);
   }
   return obj;
}

static struct json_object *section_entry(const char *hash, const char *title) {
   struct json_object *e = json_object_new_object();
   if (e) {
      json_object_object_add(e, "h", json_object_new_string(hash));
      json_object_object_add(e, "t", json_object_new_string(title));
   }
   return e;
}

void prefix_in_force_new_tag(char *out, size_t size) {
   uint32_t r = 0;
   if (getrandom(&r, sizeof(r), 0) != (ssize_t)sizeof(r)) {
      r = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ (uint32_t)(uintptr_t)out;
   }
   /* "dawn-ctx-" + 8 hex digits: a shape nothing else DAWN makes has (its
    * calendar UIDs are "dawn-<hex>-..."), so it can be defused wherever it
    * turns up in text DAWN didn't write (llm_context_neutralize). */
   snprintf(out, size, "dawn-ctx-%08x", r);
}

void prefix_in_force_init(struct json_object *prefix_msg,
                          const composed_prompt_t *cp,
                          const char *tag) {
   if (!prefix_msg || !cp) {
      return;
   }
   struct json_object *rec = json_object_new_object();
   struct json_object *sections = json_object_new_object();
   if (!rec || !sections) {
      json_object_put(rec);
      json_object_put(sections);
      return;
   }
   for (int i = 0; i < cp->n_sections; i++) {
      char h[DAWN_SHA256_HEX_LEN];
      hash_text(cp->sections[i].text, h);
      json_object_object_add(sections, cp->sections[i].name,
                             section_entry(h, cp->sections[i].title));
   }
   char none[DAWN_SHA256_HEX_LEN];
   hash_text("", none);
   if (tag && tag[0]) {
      json_object_object_add(rec, "tag", json_object_new_string(tag));
   }
   json_object_object_add(rec, "sections", sections);
   json_object_object_add(rec, "directives", json_object_new_string(none));
   json_object_object_add(prefix_msg, LLM_HISTORY_IN_FORCE_KEY, rec);
}

const char *prefix_in_force_ensure_tag(struct json_object *hist) {
   struct json_object *prefix = prefix_of(hist);
   struct json_object *rec = prefix ? record_of(prefix) : NULL;
   if (!rec) {
      return NULL;
   }
   const char *tag = str_of(rec, "tag");
   if (tag && tag[0]) {
      return tag;
   }
   char made[LLM_CONTEXT_TAG_MAX];
   prefix_in_force_new_tag(made, sizeof(made));
   json_object_object_add(rec, "tag", json_object_new_string(made));
   return str_of(rec, "tag");
}

/* The piece of an instruction message @p msg that updates the section titled
 * @p title: its start (at "Updated instructions: <title>.") and length, or
 * false when it has none. */
static bool instruction_piece(const char *msg, const char *title, const char **start, size_t *len) {
   char head[256];
   snprintf(head, sizeof(head), "Updated instructions: %s.", title ? title : "");
   const char *at = msg ? strstr(msg, head) : NULL;
   if (!at) {
      return false;
   }
   const char *next = strstr(at + strlen(head), "\n\nUpdated instructions: ");
   *start = at;
   *len = next ? (size_t)(next - at) : strlen(at);
   return true;
}

/* The hash in force for section @p sec: the record's; for a history with no
 * section record (never made, or reset by a compaction), what the history
 * shows: its newest instruction message that updates the section, else the
 * frozen prefix, holding the section's text (tag filled in). */
static const char *in_force_hash(struct json_object *hist,
                                 struct json_object *sections,
                                 const char *frozen,
                                 const prompt_section_t *sec,
                                 const char *tag,
                                 const char *own_hash) {
   if (sections) {
      struct json_object *e = NULL;
      return json_object_object_get_ex(sections, sec->name, &e) ? str_of(e, "h") : NULL;
   }
   char *text = llm_context_with_tag(sec->text, tag);
   if (!text) {
      return NULL;
   }
   const int len = hist ? (int)json_object_array_length(hist) : 0;
   for (int i = len - 1; i > 0; i--) {
      struct json_object *msg = json_object_array_get_idx(hist, i);
      const char *start = NULL;
      size_t n = 0;
      if (llm_history_kind_of(msg) != MESSAGE_KIND_INSTRUCTION ||
          !instruction_piece(str_of(msg, "content"), sec->title, &start, &n)) {
         continue;
      }
      /* Its newest update: in force if it holds this text (a removal holds none). */
      const bool there = memmem(start, n, text, strlen(text)) != NULL;
      free(text);
      return there ? own_hash : "";
   }
   const bool there = frozen && strstr(frozen, text);
   free(text);
   return there ? own_hash : NULL;
}

static bool in_prompt(const composed_prompt_t *cp, const char *name) {
   for (int i = 0; i < cp->n_sections; i++) {
      if (strcmp(cp->sections[i].name, name) == 0) {
         return true;
      }
   }
   return false;
}

char *prefix_in_force_instructions(struct json_object *hist, const composed_prompt_t *cp) {
   struct json_object *prefix = prefix_of(hist);
   if (!prefix || !cp || cp->n_sections <= 0) {
      return NULL;
   }
   struct json_object *rec = record_of(prefix);
   if (!rec) {
      return NULL;
   }
   struct json_object *sections = NULL;
   if (!json_object_object_get_ex(rec, "sections", &sections) ||
       !json_object_is_type(sections, json_type_object)) {
      sections = NULL;
   }
   const char *frozen = str_of(prefix, "content");
   const char *tag = str_of(rec, "tag");

   /* Worked out beside the record and swapped in whole, so a failure leaves
    * it as it was. */
   struct json_object *next = json_object_new_object();
   struct json_object *previous = json_object_new_object();
   strbuf_t sb;
   strbuf_init(&sb, 1024);
   bool changed = !sections; /* a history with no record gets one */
   for (int i = 0; next && previous && i < cp->n_sections; i++) {
      const prompt_section_t *sec = &cp->sections[i];
      char h[DAWN_SHA256_HEX_LEN];
      hash_text(sec->text, h);
      const char *had = in_force_hash(hist, sections, frozen, sec, tag, h);
      json_object_object_add(next, sec->name, section_entry(h, sec->title));
      if (had && strcmp(had, h) == 0) {
         continue;
      }
      changed = true;
      const char *before = had ? had : "";
      struct json_object *prev_rec = child(rec, "previous");
      const char *prev = prev_rec ? str_of(prev_rec, sec->name) : NULL;
      if (prev && strcmp(prev, h) == 0) {
         OLOG_WARNING("prefix: section '%s' changed back to what it held two changes ago; "
                      "each flip appends it again",
                      sec->name);
      }
      json_object_object_add(previous, sec->name, json_object_new_string(before));
      char *text = llm_context_with_tag(sec->text, tag);
      strbuf_appendf(&sb,
                     "%sUpdated instructions: %s. This replaces the earlier version in the "
                     "system prompt above.\n\n%s",
                     strbuf_len(&sb) ? "\n\n" : "", sec->title, text ? text : sec->text);
      free(text);
   }
   /* Sections in force that the prompt no longer has. */
   if (sections && next && previous) {
      json_object_object_foreach(sections, name, entry) {
         if (in_prompt(cp, name)) {
            continue;
         }
         changed = true;
         const char *title = str_of(entry, "t");
         const char *had = str_of(entry, "h");
         json_object_object_add(previous, name, json_object_new_string(had ? had : ""));
         strbuf_appendf(&sb,
                        "%sUpdated instructions: %s. The earlier version in the system "
                        "prompt above no longer applies.",
                        strbuf_len(&sb) ? "\n\n" : "", title ? title : name);
      }
   }
   if (!next || !previous || strbuf_oom(&sb) || !changed) {
      json_object_put(next);
      json_object_put(previous);
      strbuf_free(&sb);
      return NULL;
   }
   json_object_object_add(rec, "sections", next);
   struct json_object *prev_rec = child(rec, "previous");
   json_object_object_foreach(previous, pname, pval) {
      if (prev_rec) {
         json_object_object_add(prev_rec, pname, json_object_get(pval));
      }
   }
   json_object_put(previous);
   if (strbuf_len(&sb) == 0) {
      strbuf_free(&sb);
      return NULL; /* a record made for a history with none; nothing to send */
   }
   char *out = strbuf_steal(&sb);
   strbuf_free(&sb);
   return out;
}

const char *prefix_in_force_directives_text(const char *directives) {
   return (directives && directives[0]) ? directives : DIRECTIVES_NONE;
}

/* The directions in force for a history with no record of them: the newest
 * directive appended, if any. */
static void legacy_directives_hash(struct json_object *hist, char out[DAWN_SHA256_HEX_LEN]) {
   const char *text = "";
   for (int i = (int)json_object_array_length(hist) - 1; i >= 0; i--) {
      struct json_object *msg = json_object_array_get_idx(hist, i);
      if (llm_history_kind_of(msg) == MESSAGE_KIND_DIRECTIVE) {
         const char *c = str_of(msg, "content");
         text = (c && strcmp(c, DIRECTIVES_NONE) != 0) ? c : "";
         break;
      }
   }
   hash_text(text, out);
}

bool prefix_in_force_directives_changed(struct json_object *hist, const char *directives) {
   struct json_object *prefix = prefix_of(hist);
   struct json_object *rec = prefix ? record_of(prefix) : NULL;
   if (!rec || !directives) {
      return false;
   }
   char want[DAWN_SHA256_HEX_LEN];
   hash_text(directives, want);
   char legacy[DAWN_SHA256_HEX_LEN];
   const char *had = str_of(rec, "directives");
   if (!had) {
      legacy_directives_hash(hist, legacy);
      had = legacy;
   }
   if (strcmp(had, want) == 0) {
      if (!str_of(rec, "directives")) {
         json_object_object_add(rec, "directives", json_object_new_string(want));
      }
      return false;
   }
   json_object_object_add(rec, "directives", json_object_new_string(want));
   return true;
}

void prefix_in_force_check_tool_schemas(struct json_object *hist, const char *schemas) {
   struct json_object *prefix = prefix_of(hist);
   struct json_object *names = llm_history_frozen_tools(hist);
   struct json_object *rec = prefix ? record_of(prefix) : NULL;
   struct json_object *now = schemas ? json_tokener_parse(schemas) : NULL;
   if (!rec || !names || !json_object_is_type(now, json_type_object)) {
      json_object_put(now);
      return;
   }
   struct json_object *had = NULL;
   const bool known = json_object_object_get_ex(rec, "tool_schemas", &had) &&
                      json_object_is_type(had, json_type_object);
   struct json_object *next = json_object_new_object();
   strbuf_t changed;
   strbuf_init(&changed, 128);
   const size_t n = json_object_array_length(names);
   for (size_t i = 0; next && i < n; i++) {
      const char *name = json_object_get_string(json_object_array_get_idx(names, i));
      if (!name) {
         continue;
      }
      const char *h = str_of(now, name);
      const char *was = known ? str_of(had, name) : NULL;
      if (known && (!h || !was || strcmp(h, was) != 0)) {
         strbuf_appendf(&changed, "%s%s%s", strbuf_len(&changed) ? ", " : "", name,
                        h ? "" : " (gone)");
      }
      if (h) {
         json_object_object_add(next, name, json_object_new_string(h));
      }
   }
   if (strbuf_len(&changed) > 0) {
      OLOG_WARNING("prefix: the conversation's frozen tools changed since it froze them (%s): "
                   "every later request sends them as they are now",
                   strbuf_str(&changed));
   }
   strbuf_free(&changed);
   json_object_put(now);
   if (next && (!known || !json_object_equal(had, next))) {
      json_object_object_add(rec, "tool_schemas", next);
   } else {
      json_object_put(next);
   }
}

char *prefix_in_force_json(struct json_object *hist) {
   struct json_object *prefix = prefix_of(hist);
   struct json_object *rec = NULL;
   if (!prefix || !json_object_object_get_ex(prefix, LLM_HISTORY_IN_FORCE_KEY, &rec)) {
      return NULL;
   }
   const char *json = json_object_to_json_string_ext(rec, JSON_C_TO_STRING_PLAIN);
   return json ? strdup(json) : NULL;
}

void prefix_in_force_reset_to_history(struct json_object *hist) {
   struct json_object *prefix = prefix_of(hist);
   struct json_object *rec = NULL;
   if (!prefix || !json_object_object_get_ex(prefix, LLM_HISTORY_IN_FORCE_KEY, &rec) ||
       !json_object_is_type(rec, json_type_object)) {
      return;
   }
   /* The tag and the tool schemas stay: the frozen prefix still declares them. */
   json_object_object_del(rec, "sections");
   json_object_object_del(rec, "previous");
   json_object_object_del(rec, "directives");
}
