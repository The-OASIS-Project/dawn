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
 * WebUI Memory Handlers - Memory management for WebSocket clients
 *
 * This module handles WebSocket messages for memory operations:
 * - get_memory_stats, list_memory_facts, list_memory_preferences
 * - list_memory_summaries, search_memory, delete_memory_fact
 * - delete_memory_preference, delete_all_memories
 */

#include <string.h>

#include "auth/auth_db.h"
#include "core/memory_filter.h"
#include "core/strbuf.h"
#include "logging.h"
#include "memory/contacts_db.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_similarity.h"
#include "webui/webui_internal.h"

/* Default pagination limits */
#define DEFAULT_MEMORY_LIMIT 20
#define MAX_MEMORY_LIMIT 50

/* =============================================================================
 * Memory Statistics Handler
 * ============================================================================= */

/**
 * @brief Get memory statistics for the current user
 */
void handle_get_memory_stats(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_memory_stats_response"));
   json_object *resp_payload = json_object_new_object();

   memory_stats_t stats;
   int result = memory_db_get_stats(conn->auth_user_id, &stats);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(stats.fact_count));
      json_object_object_add(resp_payload, "pref_count", json_object_new_int(stats.pref_count));
      json_object_object_add(resp_payload, "summary_count",
                             json_object_new_int(stats.summary_count));
      json_object_object_add(resp_payload, "entity_count", json_object_new_int(stats.entity_count));
      json_object_object_add(resp_payload, "oldest_fact", json_object_new_int64(stats.oldest_fact));
      json_object_object_add(resp_payload, "newest_fact", json_object_new_int64(stats.newest_fact));

      /* Count contacts for this user */
      int contact_count = 0;
      contacts_count(conn->auth_user_id, &contact_count);
      json_object_object_add(resp_payload, "contact_count", json_object_new_int(contact_count));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to get memory stats"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Facts Handlers
 * ============================================================================= */

/**
 * @brief List memory facts for the current user (paginated)
 */
void handle_list_memory_facts(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_memory_facts_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DEFAULT_MEMORY_LIMIT;
   int offset = 0;
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > MAX_MEMORY_LIMIT) {
            limit = DEFAULT_MEMORY_LIMIT;
         }
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0) {
            offset = 0;
         }
      }
   }

   /* Query database */
   memory_fact_t facts[MAX_MEMORY_LIMIT];
   memset(facts, 0, sizeof(facts));
   int count = 0;
   int list_rc = memory_db_fact_list(conn->auth_user_id, facts, limit, offset, &count);

   if (list_rc == MEMORY_DB_SUCCESS) {
      /* Batch-fetch provenance for all facts in one lock cycle. */
      int64_t src_conv_ids[MAX_MEMORY_LIMIT] = { 0 };
      int64_t src_starts[MAX_MEMORY_LIMIT] = { 0 };
      int64_t src_ends[MAX_MEMORY_LIMIT] = { 0 };
      int64_t fact_ids[MAX_MEMORY_LIMIT];
      for (int k = 0; k < count; k++)
         fact_ids[k] = facts[k].id;
      memory_db_facts_get_sources(conn->auth_user_id, fact_ids, count, src_conv_ids, src_starts,
                                  src_ends);

      json_object *facts_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *fact_obj = json_object_new_object();
         json_object_object_add(fact_obj, "id", json_object_new_int64(facts[i].id));
         json_object_object_add(fact_obj, "fact_text", json_object_new_string(facts[i].fact_text));
         json_object_object_add(fact_obj, "confidence",
                                json_object_new_double(facts[i].confidence));
         json_object_object_add(fact_obj, "source", json_object_new_string(facts[i].source));
         json_object_object_add(fact_obj, "created_at", json_object_new_int64(facts[i].created_at));
         json_object_object_add(fact_obj, "last_accessed",
                                json_object_new_int64(facts[i].last_accessed));
         json_object_object_add(fact_obj, "access_count",
                                json_object_new_int(facts[i].access_count));

         if (src_conv_ids[i] > 0) {
            json_object_object_add(fact_obj, "source_conversation_id",
                                   json_object_new_int64(src_conv_ids[i]));
            json_object_object_add(fact_obj, "source_msg_id_start",
                                   json_object_new_int64(src_starts[i]));
            json_object_object_add(fact_obj, "source_msg_id_end",
                                   json_object_new_int64(src_ends[i]));
         }

         json_object_array_add(facts_array, fact_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "facts", facts_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list memory facts"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* Module-level callback for handle_get_memory_fact_source */
static int source_msg_to_json(const conversation_message_t *msg, void *ctx) {
   json_object *arr = (json_object *)ctx;
   if (!msg || !msg->content)
      return 0;
   if (strcmp(msg->role, "system") == 0 || strcmp(msg->role, "tool") == 0)
      return 0;
   json_object *obj = json_object_new_object();
   json_object_object_add(obj, "id", json_object_new_int64(msg->id));
   json_object_object_add(obj, "role", json_object_new_string(msg->role));
   json_object_object_add(obj, "content", json_object_new_string(msg->content));
   json_object_object_add(obj, "created_at", json_object_new_int64((int64_t)msg->created_at));
   json_object_array_add(arr, obj);
   return 0;
}

/**
 * @brief Return verbatim source messages for a fact (v40 memory provenance)
 */
void handle_get_memory_fact_source(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("get_memory_fact_source_response"));
   json_object *resp_payload = json_object_new_object();

   int64_t fact_id = 0;
   if (payload) {
      json_object *id_obj;
      if (json_object_object_get_ex(payload, "fact_id", &id_obj)) {
         fact_id = json_object_get_int64(id_obj);
      }
   }
   json_object_object_add(resp_payload, "fact_id", json_object_new_int64(fact_id));

   if (fact_id <= 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "reason", json_object_new_string("not_available"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = 0, start_id = 0, end_id = 0;
   int rc = memory_db_fact_get_source(fact_id, conn->auth_user_id, &conv_id, &start_id, &end_id);
   if (rc != MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "reason", json_object_new_string("not_available"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(resp_payload, "msg_id_start", json_object_new_int64(start_id));
   json_object_object_add(resp_payload, "msg_id_end", json_object_new_int64(end_id));

   /* Cap range at 500 messages (matches tool-path budget) */
   int64_t capped_end = (end_id - start_id > 500) ? start_id + 500 : end_id;
   json_object *messages_arr = json_object_new_array();
   conv_db_get_messages_by_range(conv_id, conn->auth_user_id, start_id, capped_end,
                                 /*include_private=*/false, source_msg_to_json, messages_arr);

   json_object_object_add(resp_payload, "messages", messages_arr);
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory fact
 */
void handle_delete_memory_fact(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_memory_fact_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get fact ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "fact_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing fact_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t fact_id = json_object_get_int64(id_obj);
   int result = memory_db_fact_delete(fact_id, conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Fact deleted"));
      OLOG_INFO("WebUI: User %d deleted memory fact %lld", conn->auth_user_id, (long long)fact_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Fact not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete fact"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Preferences Handlers
 * ============================================================================= */

/**
 * @brief Serialize a preference array to a JSON array.  Shared by the
 * paginated list handler and the search handler so both surface the
 * same field shape and a single source of truth lives here.
 */
static json_object *build_preferences_json_array(const memory_preference_t *prefs, int count) {
   json_object *prefs_array = json_object_new_array();
   for (int i = 0; i < count; i++) {
      json_object *pref_obj = json_object_new_object();
      json_object_object_add(pref_obj, "id", json_object_new_int64(prefs[i].id));
      json_object_object_add(pref_obj, "category", json_object_new_string(prefs[i].category));
      json_object_object_add(pref_obj, "value", json_object_new_string(prefs[i].value));
      json_object_object_add(pref_obj, "confidence", json_object_new_double(prefs[i].confidence));
      json_object_object_add(pref_obj, "source", json_object_new_string(prefs[i].source));
      json_object_object_add(pref_obj, "created_at", json_object_new_int64(prefs[i].created_at));
      json_object_object_add(pref_obj, "updated_at", json_object_new_int64(prefs[i].updated_at));
      json_object_object_add(pref_obj, "reinforcement_count",
                             json_object_new_int(prefs[i].reinforcement_count));
      json_object_array_add(prefs_array, pref_obj);
   }
   return prefs_array;
}

/**
 * @brief Serialize an entity array to a JSON array, enriched with
 * Phase 1 entity-merge alias summaries (drop alias rows, attach
 * `alias_count` to canonicals) and relation bulk-load (emit each
 * entity's outgoing + incoming edges inline).  Shared by the
 * paginated list handler and the search handler.
 *
 * Pulls alias summary + relation set in two queries regardless of
 * @p count; duplicating these on each search keystroke is acceptable
 * at the dev's 264-entity scale, and the alternative — caching them
 * in the connection — leaks state across tabs.
 */
#define ENTITIES_JSON_ALIAS_SUMMARY_MAX 512
/* Bulk relation pull cap.  At 2000, sizeof(memory_relation_t) ~= 176 →
 * ~352 KB stack per call, comfortable on the LWS service thread's 8 MB
 * pthread stack.  Bumped from 400 once a real user crossed it (1700+
 * relations after a corpus consolidation).  Per-entity capping (filed in
 * docs/TODO.md as a follow-up) is the right shape past ~5000 relations;
 * keep this single-global cap as the cheap intermediate.
 *
 * Soft-linked to EXPORT_MAX_RELATIONS below — they happen to share a
 * value today because both budgets scale with the user's total relation
 * count.  NOT a hard contract: this gates an interactive hot path that
 * fires per search keystroke, while EXPORT_MAX_RELATIONS gates a one-shot
 * cold export.  They can legitimately diverge (e.g. lift the export cap
 * higher when paging lands while leaving the live cap modest).  When
 * bumping one, look at the other and decide on merit — don't reflexively
 * keep them equal. */
#define ENTITIES_JSON_RELATIONS_MAX 2000
static json_object *build_entities_json_array(int user_id,
                                              const memory_entity_t *entities,
                                              int count) {
   memory_alias_summary_row_t alias_summary[ENTITIES_JSON_ALIAS_SUMMARY_MAX];
   int alias_summary_count = 0;
   /* Initialize relations + per-row canonical-root arrays unconditionally
    * so subsequent indexed reads cannot hit uninitialized stack data on
    * any future caller path that skips the SQL load.  subj_roots[] and
    * obj_roots[] are parallel to all_rels[] — entry i in each array
    * describes the same relation.
    *
    * Stack-footprint guard: at the current 2000-row cap the combined
    * locals weigh ~400 KB (368 KB for all_rels + 16 KB × 2 for the roots);
    * post-v49 mention_count field bumped sizeof(memory_relation_t) from
    * 176 → 184 bytes.  Comfortable on the LWS service thread's 8 MB glibc
    * default, but a future bump past 4000 rows should heap-allocate via
    * the same calloc pattern handle_export_memories uses below.
    * Static-assert backstops a silent cap-bump from blowing the stack on
    * a deep call chain. */
   _Static_assert((sizeof(memory_relation_t) + sizeof(int64_t) * 2) * ENTITIES_JSON_RELATIONS_MAX <
                      512 * 1024,
                  "build_entities_json_array stack locals exceed 512 KB — heap-allocate "
                  "via calloc (see handle_export_memories) before bumping the cap further");
   memory_relation_t all_rels[ENTITIES_JSON_RELATIONS_MAX];
   int64_t subj_roots[ENTITIES_JSON_RELATIONS_MAX];
   int64_t obj_roots[ENTITIES_JSON_RELATIONS_MAX];
   memset(all_rels, 0, sizeof(all_rels));
   memset(subj_roots, 0, sizeof(subj_roots));
   memset(obj_roots, 0, sizeof(obj_roots));
   int total_rels = 0;
   int64_t user_self_id = 0;

   /* Skip both bulk queries when there are no entities to enrich — an
    * empty entity-search keystroke runs through this helper roughly every
    * 300 ms while the user types.  Trims one prepare/step/finalize per
    * empty keystroke and one mutex acquisition. */
   if (count > 0) {
      memory_db_entity_alias_summary(user_id, alias_summary, ENTITIES_JSON_ALIAS_SUMMARY_MAX,
                                     &alias_summary_count);
      memory_db_relation_list_with_canonical_roots_by_user(user_id, all_rels, subj_roots, obj_roots,
                                                           ENTITIES_JSON_RELATIONS_MAX,
                                                           &total_rels);
      /* Single per-user lookup, consumed below to set is_user_self on the
       * matching row's JSON.  Consumed by JS-side direction-preference
       * guard in memory_aliases.js (never auto-suggest demoting the
       * user_self anchor in manual-link confirm modals).  Failure
       * leaves user_self_id = 0 which renders is_user_self=false on
       * every entity — degraded but safe; log so a real DB error
       * doesn't go silent. */
      if (memory_db_entity_get_user_self_id(user_id, &user_self_id) != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("entities_json: get_user_self_id failed for user %d — "
                      "is_user_self defaults to false for all entities",
                      user_id);
         user_self_id = 0;
      }
      /* Bulk relation pull is bounded by ENTITIES_JSON_RELATIONS_MAX —
       * a graph denser than that loses tail edges silently from rendered
       * cards.  Surface a warning so the operator knows to bump the cap
       * (or switch to a paginated edge query) before the UI starts mis-
       * representing the user's data.  Not user-facing yet; when this
       * fires in practice we add a `relations_truncated` JSON field and
       * an entity-card banner. */
      if (total_rels >= ENTITIES_JSON_RELATIONS_MAX) {
         OLOG_WARNING("entities_json: relation pull saturated cap %d for user %d — entity cards "
                      "may be missing edges",
                      ENTITIES_JSON_RELATIONS_MAX, user_id);
      }
   }

   json_object *entities_array = json_object_new_array();
   for (int i = 0; i < count; i++) {
      bool is_alias = false;
      for (int a = 0; a < alias_summary_count; a++) {
         if (alias_summary[a].alias_entity_id == entities[i].id) {
            is_alias = true;
            break;
         }
      }
      if (is_alias) {
         continue;
      }

      int alias_count = 0;
      for (int a = 0; a < alias_summary_count; a++) {
         if (alias_summary[a].canonical_entity_id == entities[i].id) {
            alias_count++;
         }
      }

      json_object *entity_obj = json_object_new_object();
      json_object_object_add(entity_obj, "id", json_object_new_int64(entities[i].id));
      json_object_object_add(entity_obj, "name", json_object_new_string(entities[i].name));
      /* Normalized form (lowercased, whitespace-collapsed).  Consumed by
       * the JS-side shouldSwapForLongerCanonical in memory_aliases.js
       * for the manual-link direction-preference heuristic — needs the
       * canonical form so token-count and prefix-substring comparisons
       * match the C-side rule in memory_db_alias_writes.c. */
      json_object_object_add(entity_obj, "canonical_name",
                             json_object_new_string(entities[i].canonical_name));
      json_object_object_add(entity_obj, "entity_type",
                             json_object_new_string(entities[i].entity_type));
      json_object_object_add(entity_obj, "mention_count",
                             json_object_new_int(entities[i].mention_count));
      json_object_object_add(entity_obj, "alias_count", json_object_new_int(alias_count));
      json_object_object_add(entity_obj, "is_user_self",
                             json_object_new_boolean(user_self_id != 0 &&
                                                     entities[i].id == user_self_id));
      json_object_object_add(entity_obj, "first_seen",
                             json_object_new_int64(entities[i].first_seen));
      json_object_object_add(entity_obj, "last_seen", json_object_new_int64(entities[i].last_seen));

      /* Equivalence-class-aware relation emit: subj_roots[]/obj_roots[]
       * carry each relation's canonical-root entity id (resolved by SQL,
       * Bundle 2 pattern).  Match on root == entities[i].id so relations
       * attached to any alias in the class roll up under the canonical.
       * Without this, a card shows only edges attached directly to the
       * canonical row, while mention_count aggregates the full class —
       * the "card says 48 mentions but only 2 relations" mismatch. */
      json_object *relations_array = json_object_new_array();

      /* OUT: subject's canonical-root is this entity. */
      for (int r = 0; r < total_rels; r++) {
         if (subj_roots[r] != entities[i].id)
            continue;
         json_object *rel_obj = json_object_new_object();
         json_object_object_add(rel_obj, "relation", json_object_new_string(all_rels[r].relation));
         json_object_object_add(rel_obj, "object_name",
                                json_object_new_string(all_rels[r].object_name));
         json_object_object_add(rel_obj, "object_entity_id",
                                json_object_new_int64(all_rels[r].object_entity_id));
         json_object_object_add(rel_obj, "direction", json_object_new_string("out"));
         json_object_object_add(rel_obj, "confidence",
                                json_object_new_double(all_rels[r].confidence));
         /* v49: per-row re-witness count.  JS aggregates across alias rows
          * within the same class (display-only — UNIQUE invariant is scoped
          * to literal entity_id, so cross-alias dedup happens at render). */
         json_object_object_add(rel_obj, "mention_count",
                                json_object_new_int(all_rels[r].mention_count));
         /* Validity range — "spoke_at Dragon Con 2024" is a good deal less
          * useful without WHEN.  Omitted when unset so the client can tell
          * "no date recorded" from a real value rather than rendering 1970. */
         if (all_rels[r].valid_from > 0) {
            json_object_object_add(rel_obj, "valid_from",
                                   json_object_new_int64(all_rels[r].valid_from));
         }
         if (all_rels[r].valid_to > 0) {
            json_object_object_add(rel_obj, "valid_to",
                                   json_object_new_int64(all_rels[r].valid_to));
         }
         json_object_array_add(relations_array, rel_obj);
      }

      /* IN: object's canonical-root is this entity AND subject's root
       * is NOT (within-class relations would already have been emitted
       * above; skipping prevents double-counting).  Display name + click
       * target use subj_roots[r] — the canonical id — NOT the raw
       * subject_entity_id.  entities[] is filtered to canonicals at the
       * top of this function (alias-skip), so looking up by the raw
       * alias id would silently miss and render a blank name; looking
       * up by canonical id finds the right card. */
      for (int r = 0; r < total_rels; r++) {
         if (obj_roots[r] != entities[i].id)
            continue;
         if (subj_roots[r] == entities[i].id)
            continue;
         const char *subj_name = "";
         for (int e = 0; e < count; e++) {
            if (entities[e].id == subj_roots[r]) {
               subj_name = entities[e].name;
               break;
            }
         }
         json_object *rel_obj = json_object_new_object();
         json_object_object_add(rel_obj, "relation", json_object_new_string(all_rels[r].relation));
         json_object_object_add(rel_obj, "object_name", json_object_new_string(subj_name));
         json_object_object_add(rel_obj, "object_entity_id", json_object_new_int64(subj_roots[r]));
         json_object_object_add(rel_obj, "direction", json_object_new_string("in"));
         json_object_object_add(rel_obj, "confidence",
                                json_object_new_double(all_rels[r].confidence));
         json_object_object_add(rel_obj, "mention_count",
                                json_object_new_int(all_rels[r].mention_count));
         if (all_rels[r].valid_from > 0) {
            json_object_object_add(rel_obj, "valid_from",
                                   json_object_new_int64(all_rels[r].valid_from));
         }
         if (all_rels[r].valid_to > 0) {
            json_object_object_add(rel_obj, "valid_to",
                                   json_object_new_int64(all_rels[r].valid_to));
         }
         json_object_array_add(relations_array, rel_obj);
      }

      json_object_object_add(entity_obj, "relations", relations_array);
      json_object_array_add(entities_array, entity_obj);
   }

   return entities_array;
}

/**
 * @brief List memory preferences for the current user
 */
void handle_list_memory_preferences(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("list_memory_preferences_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DEFAULT_MEMORY_LIMIT;
   int offset = 0;
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > MAX_MEMORY_LIMIT) {
            limit = DEFAULT_MEMORY_LIMIT;
         }
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0) {
            offset = 0;
         }
      }
   }

   /* Query database */
   memory_preference_t prefs[MAX_MEMORY_LIMIT];
   memset(prefs, 0, sizeof(prefs));
   int count = 0;
   int rc = memory_db_pref_list(conn->auth_user_id, prefs, limit, offset, &count);

   if (rc == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "preferences",
                             build_preferences_json_array(prefs, count));
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list preferences"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory preference by category
 */
void handle_delete_memory_preference(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("delete_memory_preference_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get category */
   json_object *cat_obj;
   if (!json_object_object_get_ex(payload, "category", &cat_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing category"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *category = json_object_get_string(cat_obj);
   int result = memory_db_pref_delete(conn->auth_user_id, category);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Preference deleted"));
      OLOG_INFO("WebUI: User %d deleted memory preference '%s'", conn->auth_user_id, category);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Preference not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete preference"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Summaries Handler
 * ============================================================================= */

/**
 * @brief List memory summaries for the current user
 */
void handle_list_memory_summaries(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("list_memory_summaries_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DEFAULT_MEMORY_LIMIT;
   int offset = 0;
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > MAX_MEMORY_LIMIT) {
            limit = DEFAULT_MEMORY_LIMIT;
         }
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0) {
            offset = 0;
         }
      }
   }

   /* Query database */
   memory_summary_t summaries[MAX_MEMORY_LIMIT];
   memset(summaries, 0, sizeof(summaries));
   int count = 0;
   int rc = memory_db_summary_list(conn->auth_user_id, summaries, limit, offset, &count);

   if (rc == MEMORY_DB_SUCCESS) {
      json_object *summaries_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *summary_obj = json_object_new_object();
         json_object_object_add(summary_obj, "id", json_object_new_int64(summaries[i].id));
         json_object_object_add(summary_obj, "session_id",
                                json_object_new_string(summaries[i].session_id));
         json_object_object_add(summary_obj, "summary",
                                json_object_new_string(summaries[i].summary));
         json_object_object_add(summary_obj, "topics", json_object_new_string(summaries[i].topics));
         json_object_object_add(summary_obj, "sentiment",
                                json_object_new_string(summaries[i].sentiment));
         json_object_object_add(summary_obj, "created_at",
                                json_object_new_int64(summaries[i].created_at));
         json_object_object_add(summary_obj, "message_count",
                                json_object_new_int(summaries[i].message_count));
         json_object_object_add(summary_obj, "duration_seconds",
                                json_object_new_int(summaries[i].duration_seconds));
         json_object_array_add(summaries_array, summary_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "summaries", summaries_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list summaries"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory summary
 */
void handle_delete_memory_summary(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("delete_memory_summary_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get summary ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "summary_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing summary_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t summary_id = json_object_get_int64(id_obj);
   int result = memory_db_summary_delete(summary_id, conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Summary deleted"));
      OLOG_INFO("WebUI: User %d deleted memory summary %lld", conn->auth_user_id,
                (long long)summary_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Summary not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete summary"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Entities Handlers
 * ============================================================================= */

/**
 * @brief List memory entities with their relations for the current user
 */
void handle_list_memory_entities(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("list_memory_entities_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DEFAULT_MEMORY_LIMIT;
   int offset = 0;
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > MAX_MEMORY_LIMIT) {
            limit = DEFAULT_MEMORY_LIMIT;
         }
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0) {
            offset = 0;
         }
      }
   }

   /* Load entities (sorted by mention_count desc) */
   memory_entity_t entities[MAX_MEMORY_LIMIT];
   memset(entities, 0, sizeof(entities));
   int count = 0;
   int rc = memory_db_entity_list(conn->auth_user_id, entities, limit, offset, &count);

   if (rc == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "entities",
                             build_entities_json_array(conn->auth_user_id, entities, count));
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list entities"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory entity and its relations
 */
void handle_delete_memory_entity(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("delete_memory_entity_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "entity_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing entity_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t entity_id = json_object_get_int64(id_obj);
   int result = memory_db_entity_delete(entity_id, conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Entity deleted"));
      OLOG_INFO("WebUI: User %d deleted memory entity %lld", conn->auth_user_id,
                (long long)entity_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Entity not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete entity"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Entity Merge Handler
 * ============================================================================= */

/**
 * @brief Merge two memory entities (source absorbed into target)
 */
void handle_merge_memory_entities(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("merge_memory_entities_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *source_obj, *target_obj;
   if (!json_object_object_get_ex(payload, "source_id", &source_obj) ||
       !json_object_object_get_ex(payload, "target_id", &target_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing source_id or target_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t source_id = json_object_get_int64(source_obj);
   int64_t target_id = json_object_get_int64(target_obj);

   if (source_id == target_id) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot merge an entity with itself"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int result = memory_db_entity_merge(conn->auth_user_id, source_id, target_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Entities merged"));
      OLOG_INFO("WebUI: User %d merged entity %lld into %lld", conn->auth_user_id,
                (long long)source_id, (long long)target_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Entity not found or not owned by user"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to merge entities"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Search Handler
 * ============================================================================= */

/**
 * @brief Search memory facts by keyword
 */
void handle_search_memory(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("search_memory_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get search query */
   json_object *query_obj;
   if (!json_object_object_get_ex(payload, "query", &query_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing query"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *query = json_object_get_string(query_obj);
   if (!query || strlen(query) == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Empty query"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Limit query length to prevent resource exhaustion */
   if (strlen(query) > 256) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Query too long"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Search all four record types in parallel.  The frontend used to
    * filter preferences and entities client-side over only the
    * paginated-in subset, so anything past the first page silently
    * vanished from search hits.  Backend search closes that gap. */
   memory_fact_t facts[MAX_MEMORY_LIMIT];
   memset(facts, 0, sizeof(facts));
   int count = 0;
   int rc1 = memory_db_fact_search(conn->auth_user_id, query, facts, MAX_MEMORY_LIMIT, &count);

   memory_summary_t summaries[MEMORY_MAX_SUMMARIES];
   memset(summaries, 0, sizeof(summaries));
   int summary_count = 0;
   int rc2 = memory_db_summary_search(conn->auth_user_id, query, summaries, MEMORY_MAX_SUMMARIES,
                                      &summary_count);

   memory_preference_t prefs[MAX_MEMORY_LIMIT];
   memset(prefs, 0, sizeof(prefs));
   int pref_count = 0;
   int rc3 = memory_db_pref_search(conn->auth_user_id, query, prefs, MAX_MEMORY_LIMIT, &pref_count);

   memory_entity_t entities[MAX_MEMORY_LIMIT];
   memset(entities, 0, sizeof(entities));
   int entity_count = 0;
   int rc4 = memory_db_entity_search(conn->auth_user_id, query, entities, MAX_MEMORY_LIMIT,
                                     &entity_count);

   if (rc1 == MEMORY_DB_SUCCESS && rc2 == MEMORY_DB_SUCCESS && rc3 == MEMORY_DB_SUCCESS &&
       rc4 == MEMORY_DB_SUCCESS) {
      json_object *facts_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *fact_obj = json_object_new_object();
         json_object_object_add(fact_obj, "id", json_object_new_int64(facts[i].id));
         json_object_object_add(fact_obj, "fact_text", json_object_new_string(facts[i].fact_text));
         json_object_object_add(fact_obj, "confidence",
                                json_object_new_double(facts[i].confidence));
         json_object_object_add(fact_obj, "source", json_object_new_string(facts[i].source));
         json_object_object_add(fact_obj, "created_at", json_object_new_int64(facts[i].created_at));
         json_object_array_add(facts_array, fact_obj);
      }

      json_object *summaries_array = json_object_new_array();
      for (int i = 0; i < summary_count; i++) {
         json_object *summary_obj = json_object_new_object();
         json_object_object_add(summary_obj, "id", json_object_new_int64(summaries[i].id));
         json_object_object_add(summary_obj, "summary",
                                json_object_new_string(summaries[i].summary));
         json_object_object_add(summary_obj, "topics", json_object_new_string(summaries[i].topics));
         json_object_object_add(summary_obj, "created_at",
                                json_object_new_int64(summaries[i].created_at));
         json_object_array_add(summaries_array, summary_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "facts", facts_array);
      json_object_object_add(resp_payload, "summaries", summaries_array);
      json_object_object_add(resp_payload, "preferences",
                             build_preferences_json_array(prefs, pref_count));
      json_object_object_add(resp_payload, "entities",
                             build_entities_json_array(conn->auth_user_id, entities, entity_count));
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(count));
      json_object_object_add(resp_payload, "summary_count", json_object_new_int(summary_count));
      json_object_object_add(resp_payload, "pref_count", json_object_new_int(pref_count));
      json_object_object_add(resp_payload, "entity_count", json_object_new_int(entity_count));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Search failed"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Delete All Memories Handler
 * ============================================================================= */

/**
 * @brief Delete all memories for the current user
 *
 * Requires explicit confirmation via "confirm" field in payload.
 */
void handle_delete_all_memories(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_all_memories_response"));
   json_object *resp_payload = json_object_new_object();

   /* Require explicit confirmation */
   json_object *confirm_obj;
   if (!json_object_object_get_ex(payload, "confirm", &confirm_obj) ||
       strcmp(json_object_get_string(confirm_obj), "DELETE") != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Must confirm by setting confirm=\"DELETE\""));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int result = memory_db_delete_user_memories(conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("All memories deleted"));
      OLOG_INFO("WebUI: User %d deleted all memories", conn->auth_user_id);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete memories"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Export Handler
 * ============================================================================= */

/* Maximum items to export per type */
#define EXPORT_MAX_FACTS 500
#define EXPORT_MAX_PREFS 200
#define EXPORT_MAX_ENTITIES 200
/* Soft-linked to ENTITIES_JSON_RELATIONS_MAX above (see comment there for
 * the full rationale).  Same value today because both budgets scale with
 * the user's total relation count.  Independent in principle: this caps
 * a one-shot cold export, the other caps an interactive hot path.  Keep
 * the cross-reference in mind when bumping either, but evaluate on merit
 * — they're allowed to diverge. */
#define EXPORT_MAX_RELATIONS 2000

/**
 * @brief Export all memories for the current user
 *
 * Supports two formats:
 * - "json": Full DAWN JSON with metadata (lossless, for backup/restore)
 * - "text": Human-readable text (portable to other AIs)
 */
void handle_export_memories(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("export_memories_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse format (default: "json") */
   const char *format = "json";
   if (payload) {
      json_object *fmt_obj;
      if (json_object_object_get_ex(payload, "format", &fmt_obj)) {
         format = json_object_get_string(fmt_obj);
      }
   }

   /* Validate format */
   if (strcmp(format, "json") != 0 && strcmp(format, "text") != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid format (use 'json' or 'text')"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   if (strcmp(format, "text") == 0) {
      /* --- Human-readable text export --- */
      memory_fact_t *facts = calloc(EXPORT_MAX_FACTS, sizeof(memory_fact_t));
      memory_preference_t *prefs = calloc(EXPORT_MAX_PREFS, sizeof(memory_preference_t));
      if (!facts || !prefs) {
         free(facts);
         free(prefs);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Memory allocation failed"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      int fact_count = 0;
      memory_db_fact_list(conn->auth_user_id, facts, EXPORT_MAX_FACTS, 0, &fact_count);
      int pref_count = 0;
      memory_db_pref_list(conn->auth_user_id, prefs, EXPORT_MAX_PREFS, 0, &pref_count);

      /* Build text output: one line per memory.  Use strbuf (growable) so a
       * dense profile can't silently truncate facts/prefs the way the prior
       * fixed-size buffer did.  Initial cap sized to a reasonable per-row
       * estimate; will grow if rows are larger. */
      strbuf_t sb;
      strbuf_init(&sb, (size_t)(fact_count + pref_count + 10) * 200);

      strbuf_append(&sb, "# My Memories\n\n");

      if (fact_count > 0) {
         strbuf_append(&sb, "## Facts\n");
         for (int i = 0; i < fact_count; i++) {
            if (strbuf_appendf(&sb, "- %s\n", facts[i].fact_text) < 0)
               break;
         }
         strbuf_append(&sb, "\n");
      }

      if (pref_count > 0) {
         strbuf_append(&sb, "## Preferences\n");
         for (int i = 0; i < pref_count; i++) {
            if (strbuf_appendf(&sb, "- %s: %s\n", prefs[i].category, prefs[i].value) < 0)
               break;
         }
      }

      if (strbuf_oom(&sb)) {
         strbuf_free(&sb);
         free(facts);
         free(prefs);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Export buffer exceeded safety cap"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      char *text_buf = strbuf_steal(&sb);
      if (!text_buf) {
         free(facts);
         free(prefs);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Memory allocation failed"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "format", json_object_new_string("text"));
      json_object_object_add(resp_payload, "data", json_object_new_string(text_buf));
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(fact_count));
      json_object_object_add(resp_payload, "pref_count", json_object_new_int(pref_count));

      free(text_buf);
      free(facts);
      free(prefs);

   } else {
      /* --- DAWN JSON export (lossless with metadata) --- */
      memory_fact_t *facts = calloc(EXPORT_MAX_FACTS, sizeof(memory_fact_t));
      memory_preference_t *prefs = calloc(EXPORT_MAX_PREFS, sizeof(memory_preference_t));
      memory_entity_t *entities = calloc(EXPORT_MAX_ENTITIES, sizeof(memory_entity_t));
      memory_relation_t *relations = calloc(EXPORT_MAX_RELATIONS, sizeof(memory_relation_t));
      /* Parallel arrays for canonical-root resolution — see build_entities_
       * json_array above for the same pattern.  Without these, relations
       * attached to alias rows would be silently dropped from the export
       * under the canonical entity (the bug Bundle 2's aggregation pattern
       * already solved for mention_count). */
      int64_t *subj_roots = calloc(EXPORT_MAX_RELATIONS, sizeof(int64_t));
      int64_t *obj_roots = calloc(EXPORT_MAX_RELATIONS, sizeof(int64_t));
      if (!facts || !prefs || !entities || !relations || !subj_roots || !obj_roots) {
         free(facts);
         free(prefs);
         free(entities);
         free(relations);
         free(subj_roots);
         free(obj_roots);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Memory allocation failed"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      int fact_count = 0;
      memory_db_fact_list(conn->auth_user_id, facts, EXPORT_MAX_FACTS, 0, &fact_count);
      int pref_count = 0;
      memory_db_pref_list(conn->auth_user_id, prefs, EXPORT_MAX_PREFS, 0, &pref_count);
      int entity_count = 0;
      memory_db_entity_list(conn->auth_user_id, entities, EXPORT_MAX_ENTITIES, 0, &entity_count);
      int relation_count = 0;
      memory_db_relation_list_with_canonical_roots_by_user(conn->auth_user_id, relations,
                                                           subj_roots, obj_roots,
                                                           EXPORT_MAX_RELATIONS, &relation_count);

      /* Build export JSON */
      json_object *export_obj = json_object_new_object();
      json_object_object_add(export_obj, "version", json_object_new_int(1));
      json_object_object_add(export_obj, "format", json_object_new_string("dawn_memory"));
      json_object_object_add(export_obj, "exported_at", json_object_new_int64((int64_t)time(NULL)));

      /* Facts array */
      json_object *facts_arr = json_object_new_array();
      for (int i = 0; i < fact_count; i++) {
         json_object *f = json_object_new_object();
         json_object_object_add(f, "fact_text", json_object_new_string(facts[i].fact_text));
         json_object_object_add(f, "confidence", json_object_new_double(facts[i].confidence));
         json_object_object_add(f, "source", json_object_new_string(facts[i].source));
         json_object_object_add(f, "created_at", json_object_new_int64(facts[i].created_at));
         json_object_array_add(facts_arr, f);
      }
      json_object_object_add(export_obj, "facts", facts_arr);

      /* Preferences array */
      json_object *prefs_arr = json_object_new_array();
      for (int i = 0; i < pref_count; i++) {
         json_object *p = json_object_new_object();
         json_object_object_add(p, "category", json_object_new_string(prefs[i].category));
         json_object_object_add(p, "value", json_object_new_string(prefs[i].value));
         json_object_object_add(p, "confidence", json_object_new_double(prefs[i].confidence));
         json_object_object_add(p, "source", json_object_new_string(prefs[i].source));
         json_object_object_add(p, "created_at", json_object_new_int64(prefs[i].created_at));
         json_object_array_add(prefs_arr, p);
      }
      json_object_object_add(export_obj, "preferences", prefs_arr);

      /* Entities array */
      json_object *entities_arr = json_object_new_array();
      for (int i = 0; i < entity_count; i++) {
         json_object *e = json_object_new_object();
         json_object_object_add(e, "name", json_object_new_string(entities[i].name));
         json_object_object_add(e, "entity_type", json_object_new_string(entities[i].entity_type));
         json_object_object_add(e, "mention_count", json_object_new_int(entities[i].mention_count));
         json_object_object_add(e, "first_seen", json_object_new_int64(entities[i].first_seen));

         /* Attach relations for this entity.  Equivalence-class-aware:
          * match subj_roots[r] against the canonical's id (not the raw
          * subject_entity_id), so relations attached to alias rows roll
          * up under the canonical in the export. */
         json_object *rels_arr = json_object_new_array();
         for (int r = 0; r < relation_count; r++) {
            if (subj_roots[r] != entities[i].id)
               continue;
            json_object *rel = json_object_new_object();
            json_object_object_add(rel, "relation", json_object_new_string(relations[r].relation));
            json_object_object_add(rel, "object_name",
                                   json_object_new_string(relations[r].object_name));
            json_object_object_add(rel, "confidence",
                                   json_object_new_double(relations[r].confidence));
            json_object_object_add(rel, "mention_count",
                                   json_object_new_int(relations[r].mention_count));
            json_object_array_add(rels_arr, rel);
         }
         json_object_object_add(e, "relations", rels_arr);

         json_object_array_add(entities_arr, e);
      }
      json_object_object_add(export_obj, "entities", entities_arr);

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "format", json_object_new_string("json"));
      json_object_object_add(resp_payload, "data", export_obj);
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(fact_count));
      json_object_object_add(resp_payload, "pref_count", json_object_new_int(pref_count));
      json_object_object_add(resp_payload, "entity_count", json_object_new_int(entity_count));

      free(facts);
      free(prefs);
      free(entities);
      free(relations);
      free(subj_roots);
      free(obj_roots);
   }

   /* Copy format before response is freed (format may point into payload JSON) */
   bool is_text = (strcmp(format, "text") == 0);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: User %d exported memories (format=%s)", conn->auth_user_id,
             is_text ? "text" : "json");
}

/* =============================================================================
 * Memory Import Handler
 * ============================================================================= */

/* Maximum items per import (limits DB queries on LWS service thread) */
#define IMPORT_MAX_LINES 200
#define IMPORT_MAX_TEXT_LEN (256 * 1024) /* 256KB max paste */

/**
 * @brief Check if a fact is a duplicate of existing memories
 *
 * Uses hash lookup + Jaccard similarity fallback.
 *
 * @return true if duplicate found
 */
static bool is_fact_duplicate(int user_id, const char *fact_text) {
   uint32_t hash = memory_normalize_and_hash(fact_text);
   if (hash == 0)
      return false;

   memory_fact_t existing[5];
   int found = 0;
   memory_db_fact_find_by_hash(user_id, hash, existing, 5, &found);
   if (found > 0)
      return true;

   /* Fallback: Jaccard similarity against recent matches */
   memory_fact_t similar[3];
   int sim_count = 0;
   memory_db_fact_find_similar(user_id, fact_text, similar, 3, &sim_count);
   for (int i = 0; i < sim_count; i++) {
      if (memory_is_duplicate(fact_text, similar[i].fact_text, MEMORY_SIMILARITY_THRESHOLD))
         return true;
   }
   return false;
}

/**
 * @brief Import memories from JSON or plain text
 *
 * Two import paths:
 * - "json" format: DAWN JSON export (direct restore with metadata)
 * - "text" format: Plain text paste (from Claude/ChatGPT, one fact per line)
 *
 * Supports preview mode (commit=false) that returns what would be imported
 * without actually writing to the database.
 */
void handle_import_memories(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("import_memories_response"));
   json_object *resp_payload = json_object_new_object();

   if (!payload) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing payload"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Parse common fields */
   json_object *format_obj, *commit_obj;
   const char *format = "text";
   bool commit = false;

   if (json_object_object_get_ex(payload, "format", &format_obj))
      format = json_object_get_string(format_obj);
   if (json_object_object_get_ex(payload, "commit", &commit_obj))
      commit = json_object_get_boolean(commit_obj);

   /* Validate format */
   if (strcmp(format, "json") != 0 && strcmp(format, "text") != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid format (use 'json' or 'text')"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int imported_facts = 0;
   int imported_prefs = 0;
   int skipped_dupes = 0;
   int skipped_empty = 0;
   int skipped_blocked = 0;
   json_object *preview_arr = json_object_new_array();

   if (strcmp(format, "json") == 0) {
      /* --- DAWN JSON import --- */
      json_object *data_obj;
      if (!json_object_object_get_ex(payload, "data", &data_obj)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Missing data"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      /* Import facts */
      json_object *facts_arr;
      if (json_object_object_get_ex(data_obj, "facts", &facts_arr)) {
         int len = json_object_array_length(facts_arr);
         for (int i = 0; i < len && i < IMPORT_MAX_LINES; i++) {
            json_object *f = json_object_array_get_idx(facts_arr, i);
            json_object *text_obj, *conf_obj, *src_obj;

            if (!json_object_object_get_ex(f, "fact_text", &text_obj))
               continue;
            const char *text = json_object_get_string(text_obj);
            if (!text || strlen(text) == 0) {
               skipped_empty++;
               continue;
            }

            float confidence = 0.8f;
            if (json_object_object_get_ex(f, "confidence", &conf_obj))
               confidence = (float)json_object_get_double(conf_obj);

            /* Truncate to max fact length (same as text path) */
            char truncated[MEMORY_FACT_TEXT_MAX];
            strncpy(truncated, text, sizeof(truncated) - 1);
            truncated[sizeof(truncated) - 1] = '\0';

            if (memory_filter_check(truncated)) {
               skipped_blocked++;
               continue;
            }

            if (is_fact_duplicate(conn->auth_user_id, truncated)) {
               skipped_dupes++;
               continue;
            }

            /* Add to preview */
            json_object *preview_item = json_object_new_object();
            json_object_object_add(preview_item, "type", json_object_new_string("fact"));
            json_object_object_add(preview_item, "text", json_object_new_string(truncated));
            json_object_object_add(preview_item, "confidence", json_object_new_double(confidence));
            json_object_array_add(preview_arr, preview_item);

            if (commit) {
               memory_db_fact_create(conn->auth_user_id, truncated, confidence, "import", NULL,
                                     NULL, NULL);
            }
            imported_facts++;
         }
      }

      /* Import preferences */
      json_object *prefs_arr;
      if (json_object_object_get_ex(data_obj, "preferences", &prefs_arr)) {
         int len = json_object_array_length(prefs_arr);
         for (int i = 0; i < len && i < IMPORT_MAX_LINES; i++) {
            json_object *p = json_object_array_get_idx(prefs_arr, i);
            json_object *cat_obj, *val_obj, *conf_obj;

            if (!json_object_object_get_ex(p, "category", &cat_obj) ||
                !json_object_object_get_ex(p, "value", &val_obj))
               continue;

            const char *category_raw = json_object_get_string(cat_obj);
            const char *value_raw = json_object_get_string(val_obj);
            if (!category_raw || !value_raw || strlen(category_raw) == 0 ||
                strlen(value_raw) == 0) {
               skipped_empty++;
               continue;
            }

            /* Truncate to field limits */
            char category[MEMORY_CATEGORY_MAX];
            strncpy(category, category_raw, sizeof(category) - 1);
            category[sizeof(category) - 1] = '\0';
            char value[MEMORY_PREF_VALUE_MAX];
            strncpy(value, value_raw, sizeof(value) - 1);
            value[sizeof(value) - 1] = '\0';

            float confidence = 0.8f;
            if (json_object_object_get_ex(p, "confidence", &conf_obj))
               confidence = (float)json_object_get_double(conf_obj);

            if (memory_filter_check(value) || memory_filter_check(category)) {
               skipped_blocked++;
               continue;
            }

            json_object *preview_item = json_object_new_object();
            json_object_object_add(preview_item, "type", json_object_new_string("preference"));
            json_object_object_add(preview_item, "category", json_object_new_string(category));
            json_object_object_add(preview_item, "value", json_object_new_string(value));
            json_object_array_add(preview_arr, preview_item);

            if (commit) {
               memory_db_pref_upsert(conn->auth_user_id, category, value, confidence, "import",
                                     NULL);
            }
            imported_prefs++;
         }
      }

   } else {
      /* --- Plain text import (from Claude/ChatGPT) --- */
      json_object *text_obj;
      if (!json_object_object_get_ex(payload, "text", &text_obj)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Missing text"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      const char *text = json_object_get_string(text_obj);
      if (!text || strlen(text) == 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Empty text"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      if (strlen(text) > IMPORT_MAX_TEXT_LEN) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Text too large (256KB max)"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      /* Parse line by line: strip "- " bullet prefix, skip headers/blank lines */
      char *text_copy = strdup(text);
      if (!text_copy) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Memory allocation failed"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      int line_count = 0;
      char *saveptr = NULL;
      char *line = strtok_r(text_copy, "\n", &saveptr);

      while (line && line_count < IMPORT_MAX_LINES) {
         line_count++;

         /* Skip leading whitespace */
         while (*line == ' ' || *line == '\t')
            line++;

         /* Skip headers (lines starting with #) */
         if (*line == '#' || *line == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
         }

         /* Strip bullet prefix "- " or "* " */
         const char *fact_text = line;
         if ((line[0] == '-' || line[0] == '*') && line[1] == ' ')
            fact_text = line + 2;

         /* Skip if too short (likely noise) */
         if (strlen(fact_text) < 3) {
            skipped_empty++;
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
         }

         /* Truncate to max fact length */
         char truncated[MEMORY_FACT_TEXT_MAX];
         strncpy(truncated, fact_text, sizeof(truncated) - 1);
         truncated[sizeof(truncated) - 1] = '\0';

         if (memory_filter_check(truncated)) {
            skipped_blocked++;
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
         }

         if (is_fact_duplicate(conn->auth_user_id, truncated)) {
            skipped_dupes++;
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
         }

         json_object *preview_item = json_object_new_object();
         json_object_object_add(preview_item, "type", json_object_new_string("fact"));
         json_object_object_add(preview_item, "text", json_object_new_string(truncated));
         json_object_object_add(preview_item, "confidence", json_object_new_double(0.7));
         json_object_array_add(preview_arr, preview_item);

         if (commit) {
            memory_db_fact_create(conn->auth_user_id, truncated, 0.7f, "import", NULL, NULL, NULL);
         }
         imported_facts++;

         line = strtok_r(NULL, "\n", &saveptr);
      }

      free(text_copy);
   }

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "committed", json_object_new_boolean(commit));
   json_object_object_add(resp_payload, "imported_facts", json_object_new_int(imported_facts));
   json_object_object_add(resp_payload, "imported_prefs", json_object_new_int(imported_prefs));
   json_object_object_add(resp_payload, "skipped_dupes", json_object_new_int(skipped_dupes));
   json_object_object_add(resp_payload, "skipped_empty", json_object_new_int(skipped_empty));
   json_object_object_add(resp_payload, "skipped_blocked", json_object_new_int(skipped_blocked));
   if (!commit) {
      json_object_object_add(resp_payload, "preview", preview_arr);
   } else {
      json_object_put(preview_arr);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   if (commit) {
      OLOG_INFO(
          "WebUI: User %d imported memories (format=%s, facts=%d, prefs=%d, dupes=%d, blocked=%d)",
          conn->auth_user_id, format, imported_facts, imported_prefs, skipped_dupes,
          skipped_blocked);
   }
}

/* =============================================================================
 * Entity-merge alias surface (v43) — WebSocket handlers per design §14.
 *
 * All five share the same response envelope shape: {type, payload:{success,
 * error?, ...}}.  Underlying logic comes from memory_db_alias.c — no new
 * SQL is added here.
 * ============================================================================= */

/* Helper: render a {success: false, error: ...} envelope and send it. */
static void send_alias_error(ws_connection_t *conn, const char *type, const char *err) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string(type));
   json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
   json_object_object_add(resp_payload, "error", json_object_new_string(err));
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_entity_aliases_request(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn))
      return;
   json_object *id_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "entity_id", &id_obj)) {
      send_alias_error(conn, "entity_aliases_response", "Missing entity_id");
      return;
   }
   int64_t entity_id = json_object_get_int64(id_obj);
   if (entity_id <= 0) {
      send_alias_error(conn, "entity_aliases_response", "Invalid entity_id");
      return;
   }

   memory_alias_listing_row_t rows[64];
   int count = 0;
   int rc = memory_db_entity_alias_list(conn->auth_user_id, entity_id, rows, 64, &count);

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("entity_aliases_response"));
   json_object *resp_payload = json_object_new_object();
   if (rc != MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list aliases"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "entity_id", json_object_new_int64(entity_id));
      json_object *arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *row = json_object_new_object();
         json_object_object_add(row, "link_id", json_object_new_int64(rows[i].link_id));
         json_object_object_add(row, "source_entity_id",
                                json_object_new_int64(rows[i].source_entity_id));
         json_object_object_add(row, "source_canonical_name",
                                json_object_new_string(rows[i].source_canonical_name));
         json_object_object_add(row, "link_kind", json_object_new_string(rows[i].link_kind));
         json_object_object_add(row, "composite_score",
                                json_object_new_double(rows[i].composite_score));
         json_object_object_add(row, "linked_at", json_object_new_int64(rows[i].linked_at));
         json_object_array_add(arr, row);
      }
      json_object_object_add(resp_payload, "aliases", arr);
   }
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_entity_merge_proposal_list_request(ws_connection_t *conn, struct json_object *payload) {
   (void)payload; /* No filtering args today — the user's pending-only list. */
   if (!conn_require_auth(conn))
      return;

   memory_alias_proposal_row_t rows[64];
   int count = 0;
   int rc = memory_db_proposal_list_pending(conn->auth_user_id, rows, 64, &count);

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("entity_merge_proposal_list_response"));
   json_object *resp_payload = json_object_new_object();
   if (rc != MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list proposals"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object *arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *row = json_object_new_object();
         json_object_object_add(row, "proposal_id", json_object_new_int64(rows[i].proposal_id));
         json_object_object_add(row, "source_entity_id",
                                json_object_new_int64(rows[i].source_entity_id));
         json_object_object_add(row, "target_entity_id",
                                json_object_new_int64(rows[i].target_entity_id));
         json_object_object_add(row, "source_canonical_name",
                                json_object_new_string(rows[i].source_canonical_name));
         json_object_object_add(row, "target_canonical_name",
                                json_object_new_string(rows[i].target_canonical_name));
         json_object_object_add(row, "composite_score",
                                json_object_new_double(rows[i].composite_score));
         json_object_object_add(row, "proposed_at", json_object_new_int64(rows[i].proposed_at));
         json_object_array_add(arr, row);
      }
      json_object_object_add(resp_payload, "proposals", arr);
   }
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_entity_link_request(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn))
      return;
   json_object *src_obj = NULL, *tgt_obj = NULL, *reason_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "source_entity_id", &src_obj) ||
       !json_object_object_get_ex(payload, "target_entity_id", &tgt_obj)) {
      send_alias_error(conn, "entity_link_response",
                       "Missing source_entity_id or target_entity_id");
      return;
   }
   int64_t source_id = json_object_get_int64(src_obj);
   int64_t target_id = json_object_get_int64(tgt_obj);
   /* Cap the operator-supplied reason at 128 bytes server-side and run it
    * through the prompt-injection filter before it lands in the
    * memory_entity_aliases audit row.  Free-form strings reaching DB +
    * downstream consumers without a length cap or filter pass is the
    * Ckpt 5 fold-in for sec-M2. */
   char reason_buf[129] = "operator";
   if (json_object_object_get_ex(payload, "reason", &reason_obj)) {
      const char *r = json_object_get_string(reason_obj);
      if (r && *r) {
         strncpy(reason_buf, r, sizeof(reason_buf) - 1);
         reason_buf[sizeof(reason_buf) - 1] = '\0';
         if (memory_filter_check(reason_buf)) {
            send_alias_error(conn, "entity_link_response", "Reason failed prompt-injection filter");
            return;
         }
      }
   }

   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(conn->auth_user_id, source_id, target_id, "soft",
                                        reason_buf, -1.0f, NULL, &link_id);
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("entity_link_response"));
   json_object *resp_payload = json_object_new_object();
   if (rc == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "link_id", json_object_new_int64(link_id));
      json_object_object_add(resp_payload, "source_entity_id", json_object_new_int64(source_id));
      json_object_object_add(resp_payload, "target_entity_id", json_object_new_int64(target_id));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Link failed (entity not found, "
                                                    "self-link, or source has dependents)"));
   }
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_entity_unlink_request(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn))
      return;
   json_object *id_obj = NULL, *reason_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "link_id", &id_obj)) {
      send_alias_error(conn, "entity_unlink_response", "Missing link_id");
      return;
   }
   int64_t link_id = json_object_get_int64(id_obj);
   /* Cap + filter the unlink reason — same rationale as entity_link_request. */
   char reason_buf[129] = "split-by-operator";
   if (json_object_object_get_ex(payload, "reason", &reason_obj)) {
      const char *r = json_object_get_string(reason_obj);
      if (r && *r) {
         strncpy(reason_buf, r, sizeof(reason_buf) - 1);
         reason_buf[sizeof(reason_buf) - 1] = '\0';
         if (memory_filter_check(reason_buf)) {
            send_alias_error(conn, "entity_unlink_response",
                             "Reason failed prompt-injection filter");
            return;
         }
      }
   }

   int rc = memory_db_entity_alias_unlink(conn->auth_user_id, link_id, reason_buf);
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("entity_unlink_response"));
   json_object *resp_payload = json_object_new_object();
   if (rc == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "link_id", json_object_new_int64(link_id));
   } else if (rc == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Link not found or already unlinked"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(
          resp_payload, "error",
          json_object_new_string(
              "Hard merges cannot be split — use 'dawn-admin memory reextract'"));
   }
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_entity_proposal_resolve_request(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *id_obj = NULL, *res_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "proposal_id", &id_obj) ||
       !json_object_object_get_ex(payload, "resolution", &res_obj)) {
      send_alias_error(conn, "entity_proposal_resolve_response",
                       "Missing proposal_id or resolution");
      return;
   }
   int64_t proposal_id = json_object_get_int64(id_obj);
   const char *resolution = json_object_get_string(res_obj);
   if (proposal_id <= 0 || !resolution) {
      send_alias_error(conn, "entity_proposal_resolve_response",
                       "Invalid proposal_id or resolution");
      return;
   }
   bool approved;
   if (strcmp(resolution, "approved") == 0) {
      approved = true;
   } else if (strcmp(resolution, "rejected") == 0) {
      approved = false;
   } else {
      send_alias_error(conn, "entity_proposal_resolve_response",
                       "resolution must be 'approved' or 'rejected'");
      return;
   }

   int64_t link_id = 0;
   int rc = memory_db_proposal_resolve(conn->auth_user_id, proposal_id, approved, &link_id);

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("entity_proposal_resolve_response"));
   json_object *resp_payload = json_object_new_object();
   if (rc == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "proposal_id", json_object_new_int64(proposal_id));
      json_object_object_add(resp_payload, "resolution", json_object_new_string(resolution));
      if (approved && link_id > 0) {
         json_object_object_add(resp_payload, "link_id", json_object_new_int64(link_id));
      }
   } else if (rc == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Proposal not found or already resolved"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Approve failed (entity not found, "
                                                    "self-link, or source has dependents); "
                                                    "proposal still pending"));
   }
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   /* Broadcast new pending count so the memory-icon dot toggles off when
    * this was the last open proposal (or stays lit for remaining ones). */
   if (rc == MEMORY_DB_SUCCESS) {
      webui_broadcast_memory_proposals_changed(conn->auth_user_id);
   }
}
