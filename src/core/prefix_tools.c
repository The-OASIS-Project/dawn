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
 * A conversation's tools at a turn seam (prefix_tools.h).
 *
 * Kept on the in-force record (prefix_in_force.h), beside the sections:
 *   "tool_changes":          <changes appended in all>,
 *   "tool_changes_capped":   true once the total bound was logged,
 *   "tool_change_servers":   {"<server>": {"t": <window start>, "n": <count>,
 *                                          "logged": bool}, ...},
 *   "inline_tools_rejected": true (LLM_TOOL_DEFS_REJECTED_KEY),
 *   "tool_hashes":           {"<name>": <canonical hash>, ...} of what is in
 *                            force (the frozen set and every change since),
 *   "tool_rows":             the tool_change messages the history had then,
 *   "tools_seen":            the fingerprint of the registered set whose every
 *                            change is in force (absent while one is held back)
 * A compaction keeps them (prefix_in_force_reset_to_history forgets only what
 * the history shows again); the hashes are trusted only while the history has
 * the tool_change messages they were made with, and a seam whose registered
 * set is the one seen, with nothing compacted, compares nothing.
 */

#include "core/prefix_tools.h"

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/hash_util.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_tool_defs.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"

/* An older build's record of the schema hashes it sent, by name. */
#define LEGACY_SCHEMAS_KEY "tool_schemas"
#define SERVER_WINDOW_SEC 3600
#define REC_HASHES "tool_hashes"
#define REC_ROWS "tool_rows"
#define REC_SEEN "tools_seen"

/* Seams that compared the tools (prefix_tools_diffs). */
static atomic_uint_fast64_t s_diffs;

uint64_t prefix_tools_diffs(void) {
   return (uint64_t)atomic_load(&s_diffs);
}

static struct json_object *parse_array(const char *json) {
   struct json_object *v = json ? json_tokener_parse(json) : NULL;
   if (!json_object_is_type(v, json_type_array)) {
      json_object_put(v);
      return NULL;
   }
   return v;
}

static const char *str_of(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_is_type(obj, json_type_object) && json_object_object_get_ex(obj, key, &v) &&
                  json_object_is_type(v, json_type_string)
              ? json_object_get_string(v)
              : NULL;
}

/* The definition named @p name in @p defs (borrowed), or NULL. */
static struct json_object *find_def(struct json_object *defs, const char *name) {
   const size_t n = defs ? json_object_array_length(defs) : 0;
   for (size_t i = 0; name && i < n; i++) {
      struct json_object *d = json_object_array_get_idx(defs, i);
      const char *have = llm_tool_def_name(d);
      if (have && strcmp(have, name) == 0) {
         return d;
      }
   }
   return NULL;
}

/* The definitions of @p catalog a request may carry, in registry order. */
static struct json_object *registered_defs(struct json_object *catalog) {
   struct json_object *out = json_object_new_array();
   const size_t n = json_object_array_length(catalog);
   for (size_t i = 0; out && i < n; i++) {
      struct json_object *def = json_object_array_get_idx(catalog, i);
      if (llm_tool_def_valid(def)) {
         json_object_array_add(out, json_object_get(def));
      }
   }
   return out;
}

static bool has_names(struct json_object *frozen) {
   const size_t n = json_object_array_length(frozen);
   for (size_t i = 0; i < n; i++) {
      if (json_object_is_type(json_object_array_get_idx(frozen, i), json_type_string)) {
         return true;
      }
   }
   return false;
}

/* An older build's set of names, as definitions: each name it sent (its
 * recorded hash) with the registry's definition; *changed when what it sent
 * no longer matches (or it recorded nothing to tell). */
static struct json_object *convert_names(struct json_object *frozen,
                                         struct json_object *catalog,
                                         struct json_object *rec,
                                         const char *schemas_now,
                                         bool *changed) {
   struct json_object *had = NULL;
   const bool known = json_object_object_get_ex(rec, LEGACY_SCHEMAS_KEY, &had) &&
                      json_object_is_type(had, json_type_object);
   struct json_object *now = schemas_now ? json_tokener_parse(schemas_now) : NULL;
   struct json_object *out = json_object_new_array();
   *changed = !known;
   const size_t n = json_object_array_length(frozen);
   for (size_t i = 0; out && i < n; i++) {
      struct json_object *e = json_object_array_get_idx(frozen, i);
      if (!json_object_is_type(e, json_type_string)) {
         if (llm_tool_def_valid(e)) {
            json_object_array_add(out, json_object_get(e));
         }
         continue;
      }
      const char *name = json_object_get_string(e);
      const char *sent = known ? str_of(had, name) : NULL;
      if (known && !sent) {
         continue; /* never sent: no registered tool had the name */
      }
      struct json_object *def = find_def(catalog, name);
      if (!def || !llm_tool_def_valid(def)) {
         *changed = true; /* sent, and gone: what it was is lost */
         continue;
      }
      const char *is = str_of(now, name);
      if (known && (!is || strcmp(is, sent) != 0)) {
         *changed = true;
      }
      json_object_array_add(out, json_object_get(def));
   }
   json_object_put(now);
   return out;
}

/* The per-server bound: whether @p server may append a change now (its
 * window, on @p servers, advanced). */
static bool server_may_change(struct json_object *servers,
                              const char *server,
                              time_t now,
                              uint32_t session_id) {
   struct json_object *e = NULL;
   if (!json_object_object_get_ex(servers, server, &e) ||
       !json_object_is_type(e, json_type_object)) {
      return true;
   }
   struct json_object *t = NULL;
   struct json_object *n = NULL;
   const int64_t start = json_object_object_get_ex(e, "t", &t) ? json_object_get_int64(t) : 0;
   const int count = json_object_object_get_ex(e, "n", &n) ? json_object_get_int(n) : 0;
   if ((int64_t)now - start >= SERVER_WINDOW_SEC || count < PREFIX_TOOL_CHANGES_PER_SERVER_HOUR) {
      return true;
   }
   struct json_object *logged = NULL;
   if (!(json_object_object_get_ex(e, "logged", &logged) && json_object_get_boolean(logged))) {
      OLOG_WARNING("Session %u: tools from %s changed %d times this hour; the definitions in "
                   "force stay until the hour is up",
                   session_id, server[0] ? server : "DAWN", count);
      json_object_object_add(e, "logged", json_object_new_boolean(1));
   }
   return false;
}

static void server_count(struct json_object *servers, const char *server, time_t now) {
   struct json_object *e = NULL;
   if (!json_object_object_get_ex(servers, server, &e) ||
       !json_object_is_type(e, json_type_object)) {
      e = json_object_new_object();
      if (!e) {
         return;
      }
      json_object_object_add(servers, server, e);
   }
   struct json_object *t = NULL;
   struct json_object *n = NULL;
   const int64_t start = json_object_object_get_ex(e, "t", &t) ? json_object_get_int64(t) : 0;
   int count = json_object_object_get_ex(e, "n", &n) ? json_object_get_int(n) : 0;
   if ((int64_t)now - start >= SERVER_WINDOW_SEC) {
      json_object_object_add(e, "t", json_object_new_int64((int64_t)now));
      json_object_object_del(e, "logged");
      count = 0;
   }
   json_object_object_add(e, "n", json_object_new_int(count + 1));
}

/* What of @p changes the bounds let through now (a new array), the counts
 * advanced; NULL when nothing is. */
static struct json_object *bounded(struct json_object *rec,
                                   struct json_object *changes,
                                   uint32_t session_id) {
   struct json_object *total_obj = NULL;
   const int total = json_object_object_get_ex(rec, "tool_changes", &total_obj)
                         ? json_object_get_int(total_obj)
                         : 0;
   if (total >= PREFIX_TOOL_CHANGES_MAX) {
      struct json_object *capped = NULL;
      if (!(json_object_object_get_ex(rec, "tool_changes_capped", &capped) &&
            json_object_get_boolean(capped))) {
         OLOG_WARNING("Session %u: the conversation's tools changed %d times; the definitions "
                      "in force stay for the rest of it",
                      session_id, total);
         json_object_object_add(rec, "tool_changes_capped", json_object_new_boolean(1));
      }
      return NULL;
   }
   struct json_object *servers = NULL;
   if (!json_object_object_get_ex(rec, "tool_change_servers", &servers) ||
       !json_object_is_type(servers, json_type_object)) {
      servers = json_object_new_object();
      if (!servers) {
         return NULL;
      }
      json_object_object_add(rec, "tool_change_servers", servers);
   }
   const time_t now = time(NULL);
   struct json_object *out = json_object_new_array();
   struct json_object *counted = json_object_new_object(); /* servers counted once */
   const size_t n = json_object_array_length(changes);
   for (size_t i = 0; out && counted && i < n; i++) {
      struct json_object *def = json_object_array_get_idx(changes, i);
      char server[96];
      llm_tool_def_server(def, server, sizeof(server));
      if (!json_object_object_get_ex(counted, server, NULL) &&
          !server_may_change(servers, server, now, session_id)) {
         continue;
      }
      json_object_object_add(counted, server, json_object_new_boolean(1));
      json_object_array_add(out, json_object_get(def));
   }
   if (counted) {
      json_object_object_foreach(counted, server, unused) {
         (void)unused;
         server_count(servers, server, now);
      }
   }
   json_object_put(counted);
   if (out && json_object_array_length(out) == 0) {
      json_object_put(out);
      return NULL;
   }
   if (out) {
      json_object_object_add(rec, "tool_changes", json_object_new_int(total + 1));
   }
   return out;
}

/* Whether the newest message other than a system one is a user turn (a
 * question or tool results): a change may sit in place after it. */
static bool after_user_turn(struct json_object *hist) {
   for (size_t i = json_object_array_length(hist); i-- > 1;) {
      struct json_object *m = json_object_array_get_idx(hist, i);
      if (llm_history_role_is(m, "system")) {
         continue;
      }
      return llm_history_role_is(m, "user") || llm_history_role_is(m, "tool");
   }
   return false;
}

/* The tool_change messages in @p hist, and whether one is stored inline. */
static int scan_changes(struct json_object *hist, bool *any_inline) {
   int rows = 0;
   *any_inline = false;
   const size_t n = json_object_array_length(hist);
   for (size_t i = 1; i < n; i++) {
      struct json_object *m = json_object_array_get_idx(hist, i);
      if (llm_history_kind_of(m) == MESSAGE_KIND_TOOL_CHANGE) {
         rows++;
         *any_inline = *any_inline || llm_tool_change_stored_inline(m);
      }
   }
   return rows;
}

static int int_of(struct json_object *obj, const char *key, int dflt) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) && json_object_is_type(v, json_type_int)
              ? json_object_get_int(v)
              : dflt;
}

/* What is in force, by name and canonical hash: the record's while the
 * history still has the changes it was made with, else worked out from the
 * history.  A new object (caller puts), or NULL. */
static struct json_object *in_force_hashes(struct json_object *hist,
                                           struct json_object *rec,
                                           int rows,
                                           bool compacted) {
   struct json_object *had = NULL;
   if (!compacted && int_of(rec, REC_ROWS, -1) == rows &&
       json_object_object_get_ex(rec, REC_HASHES, &had) &&
       json_object_is_type(had, json_type_object)) {
      struct json_object *copy = NULL;
      return json_object_deep_copy(had, &copy, NULL) == 0 ? copy : NULL;
   }
   struct json_object *defs = llm_tool_defs_for_request(hist, false);
   struct json_object *out = defs ? llm_tool_defs_hashes(defs, NULL) : NULL;
   json_object_put(defs);
   return out;
}

static bool hash_differs(struct json_object *hashes, const char *name, const char *hash) {
   const char *have = str_of(hashes, name);
   return !have || strcmp(have, hash) != 0;
}

/* The conversation's inline changes, on a turn whose model or endpoint
 * doesn't take them: folded from now on (its record says so), a declared
 * boundary, once. */
static void fold_inline_now(struct json_object *hist,
                            uint32_t session_id,
                            prefix_tools_result_t *out) {
   if (!prefix_tools_mark_rejected(hist)) {
      return;
   }
   const int dropped = llm_history_drop_turn_blocks(hist);
   out->boundary = true;
   OLOG_INFO("Session %u: prefix boundary (tool_set_changed): this turn's model or endpoint "
             "doesn't take tools defined in a message; the conversation's changes fold into "
             "its tools from now on, %d turn(s) replay without their reasoning",
             session_id, dropped);
}

/* Bind the conversation's first tools (or its first since tools were off). */
static void bind_first(struct json_object *hist,
                       struct json_object *prefix,
                       struct json_object *catalog,
                       uint32_t session_id,
                       prefix_tools_result_t *out) {
   struct json_object *want = registered_defs(catalog);
   if (!want) {
      return;
   }
   json_object_object_add(prefix, LLM_HISTORY_TOOLS_KEY, want);
   out->bind = true;
   const int dropped = llm_history_drop_turn_blocks(hist);
   if (dropped > 0) {
      out->boundary = true;
      OLOG_INFO("Session %u: prefix boundary (tool_set_changed): %zu tool(s) now, %d "
                "turn(s) replay without their reasoning",
                session_id, json_object_array_length(want), dropped);
   }
}

/* An older set of names, converted to definitions (once). */
static void convert_once(struct json_object *hist,
                         struct json_object *prefix,
                         struct json_object *rec,
                         struct json_object *frozen,
                         struct json_object *catalog,
                         const char *schemas_now,
                         uint32_t session_id,
                         prefix_tools_result_t *out) {
   bool changed = false;
   struct json_object *defs = convert_names(frozen, catalog, rec, schemas_now, &changed);
   if (!defs) {
      return;
   }
   json_object_object_add(prefix, LLM_HISTORY_TOOLS_KEY, defs);
   if (rec) {
      json_object_object_del(rec, LEGACY_SCHEMAS_KEY);
      json_object_object_del(rec, REC_HASHES);
   }
   out->bind = true;
   if (changed) {
      const int dropped = llm_history_drop_turn_blocks(hist);
      out->boundary = true;
      OLOG_INFO("Session %u: prefix boundary (tool_set_changed): the conversation's tools "
                "converted to definitions, and they no longer match what it sent; %d "
                "turn(s) replay without their reasoning",
                session_id, dropped);
   } else {
      OLOG_INFO("Session %u: the conversation's tools converted to definitions (%zu, "
                "unchanged)",
                session_id, json_object_array_length(defs));
   }
}

/* The names the tool_change messages @p hist still has define (a set). */
static struct json_object *names_in_changes(struct json_object *hist) {
   struct json_object *out = json_object_new_object();
   const size_t n = json_object_array_length(hist);
   for (size_t i = 1; out && i < n; i++) {
      struct json_object *defs = llm_tool_change_defs(json_object_array_get_idx(hist, i));
      const size_t nd = defs ? json_object_array_length(defs) : 0;
      for (size_t k = 0; k < nd; k++) {
         const char *name = llm_tool_def_name(json_object_array_get_idx(defs, k));
         if (name) {
            json_object_object_add(out, name, NULL);
         }
      }
      json_object_put(defs);
   }
   return out;
}

/* What a compaction removed that the history no longer has in force, from
 * the rows it removed (never the registry: a tool whose server is gone keeps
 * its definition).  A name a later change the history still has defines kept
 * that one.  Exempt from the bounds: it adds nothing the conversation didn't
 * have.  @p force gets each one's hash. */
static struct json_object *reemits_of(struct json_object *hist,
                                      struct json_object *removed,
                                      struct json_object *force) {
   struct json_object *later = names_in_changes(hist);
   struct json_object *out = later ? json_object_new_array() : NULL;
   const size_t n = json_object_array_length(removed);
   for (size_t i = 0; out && i < n; i++) {
      struct json_object *def = json_object_array_get_idx(removed, i);
      const char *name = llm_tool_def_name(def);
      char h[DAWN_SHA256_HEX_LEN];
      if (!name || json_object_is_type(def, json_type_string) ||
          json_object_object_get_ex(later, name, NULL) || !llm_tool_def_hash(def, h) ||
          !hash_differs(force, name, h)) {
         continue;
      }
      json_object_array_add(out, json_object_get(def));
      json_object_object_add(force, name, json_object_new_string(h));
   }
   json_object_put(later);
   return out;
}

/* The registered definitions whose hash differs from what is in force (a
 * new array, in registry order), parsing the catalog only when one does. */
static struct json_object *registry_changes(const composed_prompt_t *cp,
                                            struct json_object *hashes,
                                            struct json_object *force,
                                            struct json_object **catalog,
                                            bool *set_changed,
                                            bool *schema_changed) {
   struct json_object *out = json_object_new_array();
   json_object_object_foreach(hashes, name, hv) {
      const char *h = json_object_get_string(hv);
      if (!out || !h || !hash_differs(force, name, h)) {
         continue;
      }
      if (!*catalog) {
         *catalog = parse_array(cp->tool_defs);
      }
      struct json_object *def = find_def(*catalog, name);
      if (!def || !llm_tool_def_valid(def)) {
         continue;
      }
      if (str_of(force, name)) {
         *schema_changed = true;
      } else {
         *set_changed = true;
      }
      json_object_array_add(out, json_object_get(def));
   }
   return out;
}

void prefix_tools_apply(struct json_object *hist,
                        const composed_prompt_t *cp,
                        bool has_question,
                        struct json_object *removed,
                        uint32_t session_id,
                        prefix_tools_result_t *out) {
   memset(out, 0, sizeof(*out));
   struct json_object *prefix = llm_history_prefix(hist);
   if (!prefix) {
      return;
   }
   const bool compacted = json_object_is_type(removed, json_type_array) &&
                          json_object_array_length(removed) > 0;
   const bool tools_on = cp && cp->tool_defs;
   struct json_object *frozen = llm_history_frozen_tools(hist);
   struct json_object *catalog = NULL;
   if (!frozen || has_names(frozen)) {
      catalog = tools_on ? parse_array(cp->tool_defs) : NULL;
      if (!catalog) {
         return; /* tools off: what the conversation has stays */
      }
   }
   struct json_object *rec = llm_history_in_force(prefix, true);
   if (!rec) {
      json_object_put(catalog);
      return;
   }
   if (!frozen) {
      bind_first(hist, prefix, catalog, session_id, out);
   } else if (has_names(frozen)) {
      /* has_names() is pure, so cp is non-NULL on this path */
      // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
      convert_once(hist, prefix, rec, frozen, catalog, cp->tool_schemas, session_id, out);
   }
   if (out->bind) {
      json_object_object_del(rec, REC_HASHES); /* worked out below, once */
      json_object_object_del(rec, REC_SEEN);
   }

   bool any_inline = false;
   const int rows = scan_changes(hist, &any_inline);
   /* Sent in place or folded is the conversation's: a turn whose target
    * doesn't take it folds them, once, with a boundary. */
   if (tools_on && !cp->inline_tools && any_inline && !llm_tool_defs_inline_rejected(hist)) {
      fold_inline_now(hist, session_id, out);
   }
   /* The registered set every change of which is in force, nothing compacted:
    * nothing to compare. */
   const char *seen = str_of(rec, REC_SEEN);
   if (!compacted &&
       (!tools_on || (cp->tool_defs_fp[0] && seen && strcmp(seen, cp->tool_defs_fp) == 0 &&
                      int_of(rec, REC_ROWS, -1) == rows))) {
      json_object_put(catalog);
      return;
   }
   atomic_fetch_add(&s_diffs, 1);

   struct json_object *force = in_force_hashes(hist, rec, rows, compacted);
   if (!force) {
      json_object_put(catalog);
      return; /* nothing to compare against (out of memory): the next seam does */
   }
   struct json_object *row_defs = compacted ? reemits_of(hist, removed, force)
                                            : json_object_new_array();
   const size_t n_reemit = row_defs ? json_object_array_length(row_defs) : 0;

   /* What is registered that differs from what is in force. */
   bool all_in = true;
   bool set_changed = false;
   bool schema_changed = false;
   char fp[DAWN_SHA256_HEX_LEN] = "";
   if (tools_on && row_defs) {
      struct json_object *hashes = NULL;
      if (cp->tool_def_hashes && cp->tool_defs_fp[0]) {
         hashes = json_tokener_parse(cp->tool_def_hashes);
         snprintf(fp, sizeof(fp), "%s", cp->tool_defs_fp);
      } else {
         if (!catalog) {
            catalog = parse_array(cp->tool_defs);
         }
         struct json_object *want = catalog ? registered_defs(catalog) : NULL;
         hashes = want ? llm_tool_defs_hashes(want, fp) : NULL;
         json_object_put(want);
      }
      struct json_object *changes = json_object_is_type(hashes, json_type_object)
                                        ? registry_changes(cp, hashes, force, &catalog,
                                                           &set_changed, &schema_changed)
                                        : NULL;
      all_in = changes != NULL;
      const size_t n_changes = changes ? json_object_array_length(changes) : 0;
      struct json_object *kept = n_changes > 0 ? bounded(rec, changes, session_id) : NULL;
      if (n_changes > 0 && (!kept || json_object_array_length(kept) < n_changes)) {
         all_in = false; /* held back: compared again at a later seam */
      }
      const size_t nk = kept ? json_object_array_length(kept) : 0;
      for (size_t i = 0; i < nk; i++) {
         const char *name = llm_tool_def_name(json_object_array_get_idx(kept, i));
         const char *h = str_of(hashes, name);
         if (name && h) {
            json_object_object_add(force, name, json_object_new_string(h));
         }
      }
      llm_tool_defs_merge(row_defs, kept);
      json_object_put(kept);
      json_object_put(changes);
      json_object_put(hashes);
   }

   const size_t n = row_defs ? json_object_array_length(row_defs) : 0;
   int rows_after = rows;
   if (n > 0) {
      const bool in_place = tools_on && cp->inline_tools && has_question && after_user_turn(hist) &&
                            !llm_tool_defs_inline_rejected(hist);
      struct json_object *msg = llm_tool_change_new(row_defs, in_place);
      row_defs = NULL; /* taken */
      if (msg) {
         json_object_array_add(hist, msg);
         out->appended = msg;
         rows_after++;
         if (in_place) {
            OLOG_INFO("Session %u: %zu tool definition(s) changed (%zu kept from a "
                      "compaction); appended in place (no boundary)",
                      session_id, n, n_reemit);
         } else {
            const int dropped = llm_history_drop_turn_blocks(hist);
            out->boundary = true;
            OLOG_INFO("Session %u: prefix boundary (%s): %zu tool definition(s) (%zu kept from a "
                      "compaction) folded into the request's tools; %d turn(s) replay without "
                      "their reasoning",
                      session_id,
                      set_changed ? "tool_set_changed"
                      : n_reemit  ? "compaction"
                                  : "tool_schema_changed",
                      n, n_reemit, dropped);
         }
      } else {
         /* Not appended (out of memory): what is in force is worked out
          * again at the next seam. */
         all_in = false;
         json_object_put(force);
         force = NULL;
      }
   }
   json_object_put(row_defs);
   json_object_put(catalog);

   /* What is in force now, for the next seam. */
   if (force) {
      json_object_object_add(rec, REC_HASHES, force);
      json_object_object_add(rec, REC_ROWS, json_object_new_int(rows_after));
   } else {
      json_object_object_del(rec, REC_HASHES);
   }
   if (tools_on && all_in && fp[0]) {
      json_object_object_add(rec, REC_SEEN, json_object_new_string(fp));
   } else if (tools_on) {
      json_object_object_del(rec, REC_SEEN);
   }
}

bool prefix_tools_mark_rejected(struct json_object *hist) {
   struct json_object *prefix = llm_history_prefix(hist);
   if (!prefix || llm_tool_defs_inline_rejected(hist)) {
      return false;
   }
   struct json_object *rec = llm_history_in_force(prefix, true);
   if (!rec) {
      return false;
   }
   json_object_object_add(rec, LLM_TOOL_DEFS_REJECTED_KEY, json_object_new_boolean(1));
   return true;
}
