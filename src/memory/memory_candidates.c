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
 * What an extraction reply proposes, read once (memory_candidates_parse), and
 * storing a proposed fact with its subject and relations
 * (memory_store_candidate), for both ways a batch is stored.
 */

#include "memory/memory_candidates.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/buf_printf.h"
#include "core/iso8601.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_consolidation.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_embeddings_internal.h"
#include "memory/memory_predicate_dedup.h"
#include "utils/string_utils.h"

#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* =============================================================================
 * A reply's JSON
 *
 * LLMs often wrap JSON in markdown code blocks or include preamble text.
 * This function extracts the JSON object from such responses.
 * ============================================================================= */

struct json_object *memory_extraction_parse_json(const char *response) {
   if (!response || response[0] == '\0') {
      return NULL;
   }

   struct json_object *root = NULL;

   /* First try direct parse (pure JSON) */
   root = json_tokener_parse(response);
   if (root) {
      return root;
   }

   /* Look for JSON in markdown code block: ```json ... ``` or ``` ... ``` */
   const char *json_block_start = strstr(response, "```json");
   if (json_block_start) {
      json_block_start += 7; /* Skip "```json" */
   } else {
      json_block_start = strstr(response, "```");
      if (json_block_start) {
         json_block_start += 3; /* Skip "```" */
      }
   }

   if (json_block_start) {
      /* Skip any newline after opening ``` */
      while (*json_block_start == '\n' || *json_block_start == '\r') {
         json_block_start++;
      }

      const char *json_block_end = strstr(json_block_start, "```");
      if (json_block_end) {
         size_t len = json_block_end - json_block_start;
         char *json_str = malloc(len + 1);
         if (json_str) {
            memcpy(json_str, json_block_start, len);
            json_str[len] = '\0';
            root = json_tokener_parse(json_str);
            free(json_str);
            if (root) {
               return root;
            }
         }
      }
   }

   /* Last resort: find first '{' or '[' and try to parse from there */
   const char *brace = strchr(response, '{');
   const char *bracket = strchr(response, '[');
   const char *start = NULL;
   if (brace && bracket)
      start = (brace < bracket) ? brace : bracket;
   else if (brace)
      start = brace;
   else
      start = bracket;

   if (start) {
      root = json_tokener_parse(start);
      if (root) {
         return root;
      }
   }

   return NULL;
}

/* =============================================================================
 * Parsing
 * ============================================================================= */

static const char *str_field(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) ? json_object_get_string(v) : NULL;
}

/* Room for @p more items of @p size in *@p arr (holding @p n of @p *cap). */
static int grow(void **arr, int *cap, int n, int more, size_t size) {
   if (n + more <= *cap) {
      return SUCCESS;
   }
   int want = *cap ? *cap * 2 : 16;
   while (want < n + more) {
      want *= 2;
   }
   void *bigger = realloc(*arr, (size_t)want * size);
   if (!bigger) {
      return FAILURE;
   }
   *arr = bigger;
   *cap = want;
   return SUCCESS;
}

/* "replaces": the old value the user said a new one replaced, or that it
 * replaced one without saying which. */
static const char *read_replaces(struct json_object *rel) {
   struct json_object *repl_obj = NULL;
   if (!json_object_object_get_ex(rel, "replaces", &repl_obj)) {
      return NULL;
   }
   const char *v = json_object_is_type(repl_obj, json_type_string)
                       ? json_object_get_string(repl_obj)
                       : NULL;
   /* "unknown" too: a model may spell "can't say which" that way. */
   const bool unsure = v && (strcasecmp(v, "true") == 0 || strcasecmp(v, "unknown") == 0);
   if (v && !unsure && strcasecmp(v, "false") != 0) {
      return v[0] ? v : NULL;
   }
   if (v ? unsure : json_object_get_boolean(repl_obj)) {
      return MEMORY_RELATION_REPLACES_UNKNOWN;
   }
   return NULL;
}

static int parse_relations(struct json_object *fact, int user_id, memory_candidates_t *out) {
   struct json_object *rels_arr = NULL;
   if (!json_object_object_get_ex(fact, "relations", &rels_arr) ||
       !json_object_is_type(rels_arr, json_type_array)) {
      return SUCCESS;
   }
   const int rel_count = (int)json_object_array_length(rels_arr);
   for (int r = 0; r < rel_count; r++) {
      struct json_object *rel = json_object_array_get_idx(rels_arr, r);
      /* "predicate"; the older "relation" key is still accepted. */
      const char *pred = str_field(rel, "predicate");
      if (!pred) {
         pred = str_field(rel, "relation");
      }
      const char *subj = str_field(rel, "subject");
      const char *obj = str_field(rel, "object");
      if (!subj || !pred || !obj) {
         continue;
      }
      if (grow((void **)&out->rels, &out->rel_cap, out->n_rels, 1, sizeof(*out->rels)) != SUCCESS) {
         return FAILURE;
      }
      memory_candidate_rel_t *cr = &out->rels[out->n_rels];
      memset(cr, 0, sizeof(*cr));
      cr->subject = subj;
      cr->predicate = pred;
      memory_predicate_canonicalize(user_id, pred, cr->canon, sizeof(cr->canon));
      cr->object = obj;
      const char *vf = str_field(rel, "valid_from");
      const char *vt = str_field(rel, "valid_to");
      cr->valid_from = vf ? iso8601_parse_date_utc(vf) : 0;
      cr->valid_to = vt ? iso8601_parse_date_utc(vt) : 0;
      /* Only an INVERTED range is bad data: valid_to == valid_from is a
       * single-day event (a bare date resolves to UTC midnight), and the range
       * is inclusive downstream. */
      if (cr->valid_from != 0 && cr->valid_to != 0 && cr->valid_to < cr->valid_from) {
         OLOG_WARNING("memory_extraction: dropping inverted validity range for "
                      "(%s, %s, %s): [%ld..%ld]",
                      subj, pred, obj, (long)cr->valid_from, (long)cr->valid_to);
         cr->valid_from = 0;
         cr->valid_to = 0;
      }
      cr->replaces = read_replaces(rel);
      out->n_rels++;
   }
   return SUCCESS;
}

static int parse_entities(struct json_object *root, memory_candidates_t *out) {
   struct json_object *arr = NULL;
   if (!json_object_object_get_ex(root, "entities", &arr) ||
       !json_object_is_type(arr, json_type_array)) {
      return SUCCESS;
   }
   static const char *const k_types[] = { "person", "pet", "place", "org", "thing", NULL };
   const int count = (int)json_object_array_length(arr);
   for (int i = 0; i < count; i++) {
      struct json_object *entity = json_object_array_get_idx(arr, i);
      const char *name = str_field(entity, "name");
      const char *type = str_field(entity, "type");
      if (!name || !type) {
         continue;
      }
      bool valid = false;
      for (int t = 0; k_types[t]; t++) {
         valid = valid || strcmp(type, k_types[t]) == 0;
      }
      if (grow((void **)&out->entities, &out->entity_cap, out->n_entities, 1,
               sizeof(*out->entities)) != SUCCESS) {
         return FAILURE;
      }
      out->entities[out->n_entities].name = name;
      out->entities[out->n_entities].type = valid ? type : "thing";
      out->n_entities++;
   }
   return SUCCESS;
}

int memory_candidates_parse(struct json_object *root, int user_id, memory_candidates_t *out) {
   if (!root || !out) {
      return FAILURE;
   }
   if (parse_entities(root, out) != SUCCESS) {
      return FAILURE;
   }
   struct json_object *facts_arr = NULL;
   if (!json_object_object_get_ex(root, "facts", &facts_arr) ||
       !json_object_is_type(facts_arr, json_type_array)) {
      return SUCCESS;
   }
   const int count = (int)json_object_array_length(facts_arr);
   for (int i = 0; i < count; i++) {
      struct json_object *fact = json_object_array_get_idx(facts_arr, i);
      const char *text = str_field(fact, "text");
      if (!text || !*text) {
         continue;
      }
      if (grow((void **)&out->items, &out->cap, out->count, 1, sizeof(*out->items)) != SUCCESS) {
         return FAILURE;
      }
      memory_candidate_t *c = &out->items[out->count];
      memset(c, 0, sizeof(*c));
      c->text = text;
      c->subject = str_field(fact, "subject");
      /* Only the two values extraction may claim; anything else the model
       * writes is stored as inferred, so the word surfaces show is true. */
      const char *claimed = str_field(fact, "source");
      c->told = claimed && strcmp(claimed, "explicit") == 0;
      struct json_object *conf_obj = NULL;
      c->confidence = json_object_object_get_ex(fact, "confidence", &conf_obj)
                          ? (float)json_object_get_double(conf_obj)
                          : 0.7f;
      c->expires_at = str_field(fact, "expires_at");
      c->json = fact;
      c->first_rel = out->n_rels;
      if (parse_relations(fact, user_id, out) != SUCCESS) {
         return FAILURE;
      }
      c->n_rels = out->n_rels - c->first_rel;
      out->count++;
   }
   return SUCCESS;
}

/* Whether @p name is the assistant's (its name, or "assistant"). */
static bool is_assistant(const char *name, const char *ai_canonical) {
   if (!name || !*name) {
      return false;
   }
   char canonical[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(name, canonical, sizeof(canonical));
   if (strcmp(canonical, "dawn") == 0) {
      return false;
   }
   return strcmp(canonical, "assistant") == 0 || strcmp(canonical, "the assistant") == 0 ||
          (ai_canonical[0] && strcmp(canonical, ai_canonical) == 0);
}

bool memory_candidates_about_assistant(const memory_candidates_t *c, int idx, const char *ai_name) {
   if (!c || idx < 0 || idx >= c->count) {
      return false;
   }
   char ai_canonical[MEMORY_ENTITY_NAME_MAX] = { 0 };
   if (ai_name) {
      memory_make_canonical_name(ai_name, ai_canonical, sizeof(ai_canonical));
   }
   return is_assistant(c->items[idx].subject, ai_canonical);
}

int memory_candidates_drop_assistant(memory_candidates_t *c, const char *ai_name) {
   if (!c) {
      return 0;
   }
   char ai_canonical[MEMORY_ENTITY_NAME_MAX] = { 0 };
   if (ai_name) {
      memory_make_canonical_name(ai_name, ai_canonical, sizeof(ai_canonical));
   }
   int dropped = 0;
   for (int r = 0; r < c->n_rels; r++) {
      if (!c->rels[r].dropped && is_assistant(c->rels[r].subject, ai_canonical)) {
         c->rels[r].dropped = true;
         dropped++;
      }
   }
   int kept = 0;
   for (int i = 0; i < c->count; i++) {
      if (is_assistant(c->items[i].subject, ai_canonical)) {
         dropped++;
         continue;
      }
      c->items[kept++] = c->items[i];
   }
   c->count = kept;
   if (dropped > 0) {
      OLOG_INFO("memory_extraction: %d fact(s)/relation(s) about the assistant itself not stored",
                dropped);
   }
   return dropped;
}

void memory_candidates_free(memory_candidates_t *c) {
   if (!c) {
      return;
   }
   free(c->items);
   free(c->rels);
   free(c->entities);
   memset(c, 0, sizeof(*c));
}

/* =============================================================================
 * The batch's store
 * ============================================================================= */

/* Entities a batch resolves held in memory: the entities[] array plus the
 * subjects its facts and relations name.  Beyond this an entity still resolves,
 * by an upsert per lookup. */
#define STORE_ENTITY_MAX 128

struct memory_store {
   int user_id;
   int64_t conversation_id;
   const memory_provenance_t *prov;
   bool agent;
   int64_t created_at; /* the conversation's creation: what every row is stored with */
   /* Once a user-self entity exists, a new "user" entity is aliased to it
    * rather than promoted (see memory_store_entities). */
   bool user_self_exists;
   struct {
      char canonical[MEMORY_ENTITY_NAME_MAX];
      int64_t id;
   } entity_map[STORE_ENTITY_MAX];
   int entity_count;
   int64_t fresh[STORE_ENTITY_MAX]; /* entities the batch created: the merge check's */
   int fresh_count;
   int facts_total;
   int facts_null_subject;
};

memory_store_t *memory_store_open(int user_id,
                                  int64_t conversation_id,
                                  const memory_provenance_t *prov,
                                  bool agent) {
   memory_store_t *st = calloc(1, sizeof(*st));
   if (!st) {
      return NULL;
   }
   st->user_id = user_id;
   st->conversation_id = conversation_id;
   st->prov = prov;
   st->agent = agent;

   /* The paraphrase check's first call hits a warm cache: the cold load is
    * paid here, on the extraction thread, not mid-loop. */
   if (!agent && g_config.memory.paraphrase_dedup_enabled && memory_embeddings_available()) {
      memory_embeddings_warm_cache(user_id);
   }

   /* Rows take the conversation's creation time, so a reextract keeps the
    * natural spread recency ranking relies on.  0 (no conversation, or none
    * found) stores with the current time. */
   if (conversation_id > 0) {
      int lookup_rc = conv_db_get_created_at(conversation_id, &st->created_at);
      if (lookup_rc != AUTH_DB_SUCCESS && lookup_rc != AUTH_DB_NOT_FOUND) {
         OLOG_WARNING("memory_extraction: conv_db_get_created_at failed for conv %lld (rc=%d) — "
                      "facts/summaries will use NOW() instead of conv time",
                      (long long)conversation_id, lookup_rc);
      }
   }

   int64_t self_id = 0;
   st->user_self_exists = memory_db_entity_get_user_self_id(user_id, &self_id) ==
                              MEMORY_DB_SUCCESS &&
                          self_id > 0;
   return st;
}

int memory_store_fresh_entities(const memory_store_t *st, const int64_t **ids) {
   *ids = st ? st->fresh : NULL;
   return st ? st->fresh_count : 0;
}

int64_t memory_store_created_at(const memory_store_t *st) {
   return st ? st->created_at : 0;
}

static void map_add(memory_store_t *st, const char *canonical, int64_t id) {
   if (st->entity_count < STORE_ENTITY_MAX) {
      safe_strscpy(st->entity_map[st->entity_count].canonical, canonical);
      st->entity_map[st->entity_count].id = id;
      st->entity_count++;
   }
}

static int64_t map_find(const memory_store_t *st, const char *canonical) {
   for (int m = 0; m < st->entity_count; m++) {
      if (strcmp(st->entity_map[m].canonical, canonical) == 0) {
         return st->entity_map[m].id;
      }
   }
   return 0;
}

static void fresh_add(memory_store_t *st, int64_t id, const char *name) {
   if (st->fresh_count < STORE_ENTITY_MAX) {
      st->fresh[st->fresh_count++] = id;
   } else {
      OLOG_WARNING("memory_extraction: fresh-entity gate array full (max %d), merge check "
                   "skipped for '%s'",
                   STORE_ENTITY_MAX, name);
   }
}

void memory_store_entities(memory_store_t *st, const memory_candidates_t *c) {
   if (!st || !c) {
      return;
   }
   for (int i = 0; i < c->n_entities; i++) {
      const char *ent_name = c->entities[i].name;
      const char *ent_type = c->entities[i].type;
      char canonical[MEMORY_ENTITY_NAME_MAX];
      memory_make_canonical_name(ent_name, canonical, sizeof(canonical));

      bool was_created = false;
      int64_t eid = 0;
      if (memory_db_entity_upsert_at(st->user_id, ent_name, ent_type, canonical, st->created_at,
                                     st->created_at, &was_created, &eid) != MEMORY_DB_SUCCESS ||
          eid == 0) {
         continue;
      }
      OLOG_INFO("memory_extraction: entity: %s (%s) id=%ld %s", ent_name, ent_type, (long)eid,
                was_created ? "[new]" : "[updated]");

      if (was_created && memory_embeddings_available()) {
         memory_embeddings_embed_and_store_entity(eid, st->user_id, ent_name);
      }
      if (was_created) {
         fresh_add(st, eid, ent_name);
      }

      /* The user's own entity: promoted when its name is the user's real name
       * and none exists yet.  Errors are logged and extraction goes on. */
      if (was_created && !st->user_self_exists) {
         bool promoted = false;
         int prom_rc = memory_db_entity_maybe_auto_promote_user_self(st->user_id, eid, canonical,
                                                                     &promoted);
         if (prom_rc != MEMORY_DB_SUCCESS) {
            OLOG_WARNING("memory_extraction: auto-promote-user-self DB error for entity %ld "
                         "(rc=%d)",
                         (long)eid, prom_rc);
         }
         if (promoted) {
            st->user_self_exists = true;
         }
      }
      /* The abstract "user" entity is the user-self entity: linked to it. */
      if (was_created && st->user_self_exists) {
         int alias_rc = memory_db_entity_maybe_alias_user_to_self(st->user_id, eid, canonical,
                                                                  NULL);
         if (alias_rc != MEMORY_DB_SUCCESS) {
            OLOG_WARNING("memory_extraction: alias-user-to-self DB error for entity %ld "
                         "(rc=%d)",
                         (long)eid, alias_rc);
         }
      }

      if (st->entity_count < STORE_ENTITY_MAX) {
         map_add(st, canonical, eid);
      } else {
         OLOG_WARNING("memory_extraction: entity map full (max %d), skipping '%s'",
                      STORE_ENTITY_MAX, ent_name);
      }
   }
}

/* @p name's entity: one the batch resolved, else upserted as a "thing" (0 on
 * failure). */
static int64_t resolve_subject(memory_store_t *st, const char *name) {
   char canonical[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(name, canonical, sizeof(canonical));
   int64_t id = map_find(st, canonical);
   if (id != 0) {
      return id;
   }
   bool created = false;
   if (memory_db_entity_upsert_at(st->user_id, name, "thing", canonical, st->created_at,
                                  st->created_at, &created, &id) != MEMORY_DB_SUCCESS ||
       id <= 0) {
      return 0;
   }
   map_add(st, canonical, id);
   if (created && st->fresh_count < STORE_ENTITY_MAX) {
      st->fresh[st->fresh_count++] = id;
   }
   return id;
}

void memory_store_reinforce(memory_store_t *st,
                            int64_t fact_id,
                            bool now_told,
                            const memory_provenance_t *prov) {
   if (!st || fact_id <= 0) {
      return;
   }
   if (!prov) {
      prov = st->prov;
   }
   memory_fact_t existing;
   const bool read = memory_db_fact_get(fact_id, st->user_id, &existing) == MEMORY_DB_SUCCESS;
   /* Unreadable: as if it were at the default 0.7. */
   const float new_conf = read ? existing.confidence + 0.1f : 0.8f;
   memory_db_fact_update_confidence(fact_id, st->user_id, new_conf > 1.0f ? 1.0f : new_conf);
   /* Outside text can't make a fact the user's own word. */
   if (now_told && read && strcmp(existing.source, "explicit") != 0 &&
       !(prov && outside_is_tainted(prov->outside))) {
      memory_db_fact_mark_explicit(fact_id, st->user_id);
   }
   if (prov && prov->conv_id > 0) {
      memory_db_fact_provenance_extend(fact_id, st->user_id, prov->conv_id, prov->msg_id_start,
                                       prov->msg_id_end);
   }
}

/* Store @p c's relation @p r as stated by fact @p fact_id. */
static void store_relation(memory_store_t *st,
                           const memory_candidate_rel_t *r,
                           int64_t fact_id,
                           const char *fact_id_origin,
                           const char *text,
                           const memory_provenance_t *prov,
                           memory_store_result_t *out) {
   const char *canon_pred = r->canon;

   const int64_t r_subj_id = resolve_subject(st, r->subject);
   if (r_subj_id == 0) {
      return;
   }
   /* The object: an entity when one has its name, else a literal value (no
    * upsert: objects are often dates or numbers). */
   char r_obj_canon[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(r->object, r_obj_canon, sizeof(r_obj_canon));
   int64_t r_obj_id = map_find(st, r_obj_canon);
   if (r_obj_id == 0) {
      memory_entity_t found;
      if (memory_db_entity_get_by_name(st->user_id, r_obj_canon, &found) == MEMORY_DB_SUCCESS) {
         r_obj_id = found.id;
      }
   }

   /* Deleted meanwhile (a forget): the relation goes with it, as it would
    * have had it been stored first. */
   memory_fact_t fact_check;
   if (memory_db_fact_get(fact_id, st->user_id, &fact_check) != MEMORY_DB_SUCCESS) {
      OLOG_WARNING("memory_extraction: fact_id=%lld vanished before relation "
                   "supersede (subj=%s pred=%s obj=%s) — relation not stored "
                   "[diag origin=%s conv=%lld fact_text='%.80s']",
                   (long long)fact_id, r->subject, canon_pred, r->object, fact_id_origin,
                   prov ? (long long)prov->conv_id : -1L, text);
      return;
   }

   if (st->agent) {
      /* The consolidator closes what a new value ends (close_relation). */
      int64_t rel_id = 0;
      bool created = false;
      memory_db_relation_create_ex(st->user_id, r_subj_id, canon_pred, r_obj_id,
                                   (r_obj_id == 0) ? r->object : NULL, fact_id, 0.8f, r->valid_from,
                                   r->valid_to, prov, &rel_id, &created);
      if (created && out->n_new_relations < MEMORY_STORE_NEW_RELATIONS_MAX) {
         out->new_relation_ids[out->n_new_relations++] = rel_id;
      } else if (created) {
         out->relations_overflow = true;
      }
   } else {
      int64_t old_fact_id = 0;
      int rel_rc = memory_db_relation_supersede_ex(st->user_id, r_subj_id, canon_pred, r_obj_id,
                                                   (r_obj_id == 0) ? r->object : NULL, fact_id,
                                                   0.8f, r->valid_from, r->valid_to, r->replaces,
                                                   prov, &old_fact_id);
      if (rel_rc == MEMORY_DB_SUCCESS && old_fact_id > 0) {
         /* Superseded in the same transaction; the cache keeps current facts. */
         memory_embeddings_invalidate_cache_for_user(st->user_id);
         OLOG_INFO("memory_extraction: contradiction — fact %ld superseded by %ld "
                   "(relation: %s)",
                   (long)old_fact_id, (long)fact_id, canon_pred);
      }
   }
   if (r->valid_from || r->valid_to) {
      OLOG_INFO("memory_extraction: relation: (%s, %s, %s) valid [%ld..%ld]", r->subject,
                canon_pred, r->object, (long)r->valid_from, (long)r->valid_to);
   } else {
      OLOG_INFO("memory_extraction: relation: (%s, %s, %s)", r->subject, canon_pred, r->object);
   }
}

/* Store @p text as a new fact, or find the stored fact it says again (exact
 * text, or with the prompt path's paraphrase check), into @p out; the origin
 * tag is for the vanished-fact diagnostic. */
static const char *store_fact_row(memory_store_t *st,
                                  const char *text,
                                  const memory_candidate_t *c,
                                  const memory_provenance_t *prov,
                                  const memory_store_opts_t *opts,
                                  memory_fact_t *seen,
                                  memory_store_result_t *out) {
   const char *source = c->told ? "explicit" : "inferred";
   int64_t repeat_id = 0;
   if (memory_db_fact_find_exact(st->user_id, text, &repeat_id) == MEMORY_DB_SUCCESS) {
      /* Stored before word for word: reinforce it, as a paraphrase hit is
       * (unless the caller reinforces it itself). */
      if (memory_db_fact_get(repeat_id, st->user_id, seen) != MEMORY_DB_SUCCESS) {
         memset(seen, 0, sizeof(*seen));
      }
      out->fact_id = repeat_id;
      out->reused = true;
      if (opts && opts->keep_exact) {
         return "hash_match";
      }
      memory_store_reinforce(st, repeat_id, c->told, prov);
      OLOG_INFO("memory_extraction: repeat of fact_id=%lld: %s", (long long)repeat_id, text);
      return "hash_match";
   }
   float own[MAX_EMBEDDING_DIMS];
   const float *vec = opts && opts->vec && opts->dims > 0 && !opts->text ? opts->vec : NULL;
   int dims = vec ? opts->dims : 0;
   const bool check = (!st->agent && g_config.memory.paraphrase_dedup_enabled) ||
                      (opts && opts->paraphrase);
   if (!vec && check && memory_embeddings_available() &&
       memory_embeddings_embed(text, own, &dims) == SUCCESS && dims > 0) {
      vec = own;
   }
   if (check && vec) {
      int64_t matched_id = 0;
      float score = 0.0f;
      memory_embeddings_nearest_fact(st->user_id, vec, dims,
                                     g_config.memory.paraphrase_dedup_threshold, &matched_id,
                                     &score);
      /* An expired fact the prune hasn't removed yet is no match. */
      const bool read = matched_id > 0 &&
                        memory_db_fact_get(matched_id, st->user_id, seen) == MEMORY_DB_SUCCESS;
      if (read && memory_db_fact_expiry_hidden(seen->expires_at)) {
         matched_id = 0;
      }
      if (matched_id > 0) {
         if (!(opts && opts->keep_exact)) {
            memory_store_reinforce(st, matched_id, false, prov);
         }
         OLOG_INFO("memory_extraction: paraphrase merged into fact_id=%lld "
                   "score=%.3f: %s",
                   (long long)matched_id, (double)score, text);
         out->fact_id = matched_id;
         out->reused = true;
         return "paraphrase_match";
      }
      memory_db_fact_create_at(st->user_id, text, c->confidence, source, prov, st->created_at,
                               &out->fact_id);
      OLOG_INFO("memory_extraction: stored fact: %s", text);
      if (out->fact_id > 0) {
         memory_embeddings_store_precomputed(st->user_id, out->fact_id, vec, dims);
      }
      return "paraphrase_create";
   }
   /* No paraphrase check, or no embedding: store it.  A failed embed is
    * covered by the embedding backfill. */
   memory_db_fact_create_at(st->user_id, text, c->confidence, source, prov, st->created_at,
                            &out->fact_id);
   OLOG_INFO("memory_extraction: stored fact: %s", text);
   if (out->fact_id > 0 && vec) {
      memory_embeddings_store_precomputed(st->user_id, out->fact_id, vec, dims);
   } else if (out->fact_id > 0 && memory_embeddings_available()) {
      memory_embeddings_embed_and_store(st->user_id, out->fact_id, text);
   }
   return "create";
}

int memory_store_candidate(memory_store_t *st,
                           const memory_candidates_t *c,
                           int idx,
                           const memory_store_opts_t *opts,
                           memory_store_result_t *out) {
   memory_store_result_t local = { 0 };
   if (!out) {
      out = &local;
   }
   memset(out, 0, sizeof(*out));
   if (!st || !c || idx < 0 || idx >= c->count) {
      return FAILURE;
   }
   const memory_candidate_t *cand = &c->items[idx];
   const char *text = (opts && opts->text) ? opts->text : cand->text;
   const memory_provenance_t *prov = opts && opts->prov ? opts->prov : st->prov;

   memory_fact_t seen = { 0 }; /* a stored fact said again, as stored */
   const char *origin = store_fact_row(st, text, cand, prov, opts, &seen, out);
   if (out->fact_id <= 0) {
      out->reused = false;
      return FAILURE; /* fact insert/match failed */
   }
   /* Said again and left for the caller to reinforce: as it is now, before
    * its subject, expiry and relations below change it. */
   if (out->reused && opts && opts->keep_exact) {
      out->has_prior = memory_db_consolidation_fact_state(st->user_id, out->fact_id,
                                                          prov ? prov->conv_id : 0,
                                                          &out->prior) == MEMORY_DB_SUCCESS;
   }
   /* Expiry the extraction model attached, for a new fact and a stored one
    * said again alike (a transient fact is often stored first by `remember`
    * and then restated): only ever set, never cleared.  A batch that read
    * outside text never sets it on a stored fact no such batch taught. */
   if (g_config.memory.expire_enabled &&
       !(out->reused && prov && outside_is_tainted(prov->outside) &&
         !outside_is_tainted(seen.outside))) {
      const int64_t ref = (cand->expires_at && *cand->expires_at)
                              ? iso8601_parse_date_utc(cand->expires_at)
                              : 0;
      if (ref > 0) {
         const int64_t cutoff = ref + (int64_t)g_config.memory.expire_grace_days * 86400;
         memory_db_fact_set_expires_at(out->fact_id, st->user_id, cutoff);
         OLOG_INFO("memory_extraction: fact %lld set expires_at=%ld "
                   "(ref=%s + grace=%dd, origin=%s): %s",
                   (long long)out->fact_id, (long)cutoff, cand->expires_at,
                   g_config.memory.expire_grace_days, origin, text);
      }
   }

   /* The subject the fact is about, as the fact's entity.  Without one the
    * fact stays reachable by similarity, not through the graph. */
   int64_t subject_entity_id = 0;
   if (cand->subject && *cand->subject) {
      subject_entity_id = resolve_subject(st, cand->subject);
   }
   st->facts_total++;
   if (subject_entity_id > 0) {
      memory_db_fact_set_subject_entity(out->fact_id, st->user_id, subject_entity_id);
   } else {
      st->facts_null_subject++;
      OLOG_WARNING("memory_extraction: fact %lld stored with NULL subject_entity_id "
                   "(subject_text='%s')",
                   (long long)out->fact_id, cand->subject ? cand->subject : "(missing)");
   }

   if (!(opts && opts->no_relations)) {
      for (int r = 0; r < cand->n_rels; r++) {
         if (!c->rels[cand->first_rel + r].dropped) {
            store_relation(st, &c->rels[cand->first_rel + r], out->fact_id, origin, text, prov,
                           out);
         }
      }
   }
   return SUCCESS;
}

/* The merge check on each entity the batch created, after its relations are
 * stored (so shared one-at-a-time values count): auto-merged, proposed for the
 * panel, or left alone; one notice for all proposals. */
static void merge_gate(int user_id, const int64_t *fresh_ids, int fresh_count) {
   if (!g_config.memory.entity_merge_enabled || fresh_count <= 0) {
      return;
   }
   bool any_proposed = false;
   for (int i = 0; i < fresh_count; i++) {
      const int64_t fid = fresh_ids[i];
      memory_alias_evaluate_t eval = { 0 };
      const int merge_rc = memory_db_entity_consider_auto_merge(user_id, fid, &eval);
      if (merge_rc == MEMORY_DB_SUCCESS) {
         /* source → target as linked or proposed (a longer name may have
          * swapped the direction). */
         if (eval.outcome == MEMORY_ALIAS_OUTCOME_AUTO_MERGED) {
            OLOG_INFO("memory_extraction: alias auto-merged %ld → %ld (composite=%.2f)",
                      (long)eval.source_entity_id, (long)eval.target_entity_id,
                      (double)eval.evidence.composite_score);
         } else if (eval.outcome == MEMORY_ALIAS_OUTCOME_PROPOSED) {
            OLOG_INFO("memory_extraction: alias proposed %ld → %ld (composite=%.2f)",
                      (long)eval.source_entity_id, (long)eval.target_entity_id,
                      (double)eval.evidence.composite_score);
            any_proposed = true;
         } else if (eval.outcome == MEMORY_ALIAS_OUTCOME_REJECTED && eval.target_entity_id > 0) {
            OLOG_INFO("memory_extraction: alias considered %ld → %ld but below "
                      "threshold (composite=%.2f)",
                      (long)fid, (long)eval.target_entity_id,
                      (double)eval.evidence.composite_score);
         }
      } else if (merge_rc != MEMORY_DB_NOT_FOUND) {
         /* NOT_FOUND: deleted since; anything else is a resolver problem. */
         OLOG_WARNING("memory_extraction: consider_auto_merge failed for entity %ld", (long)fid);
      }
   }
   if (any_proposed) {
#ifdef ENABLE_WEBUI
      webui_broadcast_memory_proposals_changed(user_id);
#endif
   }
}

void memory_store_close(memory_store_t *st) {
   if (!st) {
      return;
   }
   if (st->facts_total > 0) {
      OLOG_INFO("memory_extraction: store summary — %d facts processed, %d with NULL "
                "subject_entity_id (%.1f%%)",
                st->facts_total, st->facts_null_subject,
                100.0 * (double)st->facts_null_subject / (double)st->facts_total);
   }
   if (!st->agent) {
      merge_gate(st->user_id, st->fresh, st->fresh_count);
   }
   if (st->entity_count > 0) {
      memory_embeddings_invalidate_entity_cache_for_user(st->user_id);
   }
   free(st);
}

int64_t memory_extraction_store_summary(int user_id,
                                        int64_t conversation_id,
                                        const char *session_id,
                                        struct json_object *root,
                                        int message_count,
                                        int duration_seconds,
                                        const memory_provenance_t *prov,
                                        int64_t created_at) {
   int64_t summary_id = 0;
   if (!root) {
      return 0;
   }
   /* Process summary */
   struct json_object *summary_obj, *topics_arr;
   if (json_object_object_get_ex(root, "summary", &summary_obj)) {
      const char *summary = json_object_get_string(summary_obj);

      /* Build topics string */
      char topics[MEMORY_TOPICS_MAX] = { 0 };
      if (json_object_object_get_ex(root, "topics", &topics_arr)) {
         int count = json_object_array_length(topics_arr);
         size_t toff = 0;
         size_t trem = MEMORY_TOPICS_MAX;
         for (int i = 0; i < count && trem > 1; i++) {
            const char *topic = json_object_get_string(json_object_array_get_idx(topics_arr, i));
            /* No substring filter — LLM-emitted topic string. */
            if (topic && topic[0] != '\0') {
               if (toff > 0) {
                  BUF_PRINTF(topics, toff, trem, ", ");
               }
               BUF_PRINTF(topics, toff, trem, "%s", topic);
            }
         }
      }

      /* No substring filter on summary — LLM-paraphrased timeline of the
       * conversation.  See trust-tier rationale in atlas
       * INJECTION_FILTER.md §"Filtering at the trust boundary, not after
       * paraphrase". */
      {
         int crc = memory_db_summary_create_at(user_id, session_id, summary, topics, "neutral",
                                               message_count, duration_seconds, prov, created_at,
                                               &summary_id);
         if (crc == MEMORY_DB_SUCCESS && summary_id > 0) {
            /* Embed-at-create so the semantic summary adapter sees this
             * row immediately.  Failure is silent — the recompute worker
             * picks up unembedded rows on next boot. */
            (void)memory_embeddings_embed_and_store_summary(user_id, summary_id, summary);
         }
         OLOG_INFO("memory_extraction: stored summary for session %s", session_id);
      }
   }

   /* Process title — auto-rename conversation if not locked (atomic check-and-set) */
   struct json_object *title_obj;
   if (json_object_object_get_ex(root, "title", &title_obj)) {
      const char *title = json_object_get_string(title_obj);
      if (title && title[0] != '\0' && conversation_id > 0) {
         char safe_title[48];
         safe_strscpy(safe_title, title);
         utf8_truncate(safe_title, 40);

         int rc = conv_db_auto_title(conversation_id, user_id, safe_title);
         if (rc == AUTH_DB_SUCCESS) {
#ifdef ENABLE_WEBUI
            webui_broadcast_conversation_renamed(user_id, conversation_id, safe_title);
#endif
            OLOG_INFO("memory_extraction: auto-titled conversation %lld: %s",
                      (long long)conversation_id, safe_title);
         }
      }
   }

   return summary_id;
}
