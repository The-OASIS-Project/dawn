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
 * The consolidator's rounds: the extraction's candidates, triage (code alone
 * stores what needs no judgment), what the model is shown, the plan and its
 * one repair round, and when to stop.  The plan itself is read, checked and
 * applied in memory_consolidate_ops.c.
 */

#define MEMORY_CONSOLIDATE_INTERNAL_ALLOWED
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/prompt_parts.h"
#include "dawn_error.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "memory/memory_consolidate_internal.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_extraction.h"
#include "memory/memory_fact_search.h"
#include "memory/memory_similarity.h"
#include "prompts.h"
#include "utils/string_utils.h"

#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* =============================================================================
 * The model call
 * ============================================================================= */

static char *call_model(struct json_object *history,
                        const llm_resolved_config_t *cfg,
                        int *prompt_tokens,
                        int *output_tokens) {
   llm_cache_monitor_take_usage(NULL, NULL);
   /* Text from a conversation: no tools, as for the extraction itself. */
   llm_tools_suppress_push();
   char *reply = llm_chat_completion_with_config(history, NULL, cfg);
   llm_tools_suppress_pop();
   llm_cache_monitor_take_usage(prompt_tokens, output_tokens);
   return reply;
}

/* A no-op unless the WebUI's strong override is linked in. */
__attribute__((weak)) void memory_consolidation_batch_notify(int user_id,
                                                             int64_t conversation_id,
                                                             int64_t batch_id) {
   (void)user_id;
   (void)conversation_id;
   (void)batch_id;
}

static memory_consolidate_model_fn s_model = call_model;
static int s_samples = MEMORY_CONSOLIDATE_SAMPLES;

void memory_consolidate_set_model_fn(memory_consolidate_model_fn fn) {
   s_model = fn ? fn : call_model;
}

void memory_consolidate_set_samples(int n) {
   s_samples = n == 2 ? 2 : MEMORY_CONSOLIDATE_SAMPLES;
}

/* One call on @p history, counted into the result; its usage line says
 * @p iteration (0: an extraction sample, 1 and up: the rounds). */
static char *ask(mc_batch_t *b,
                 struct json_object *history,
                 int iteration,
                 int *output,
                 int *input) {
   int prompt = 0;
   int out = 0;
   const int kind_prev = llm_cache_monitor_push_kind(LLM_CALL_CONSOLIDATION);
   const int iter_prev = llm_cache_monitor_get_iteration();
   llm_cache_monitor_set_iteration(iteration);
   char *reply = s_model(history, b->in->cfg, &prompt, &out);
   llm_cache_monitor_set_iteration(iter_prev);
   llm_cache_monitor_pop_kind(kind_prev);
   b->res->calls++;
   b->res->input_tokens += prompt;
   if (output) {
      *output = out;
   }
   if (input) {
      *input = prompt;
   }
   return reply;
}

/* =============================================================================
 * Stopping
 * ============================================================================= */

const char *memory_consolidate_stop_name(memory_consolidate_stop_t stop) {
   switch (stop) {
      case MEMORY_CONSOLIDATE_FINISHED:
         return "finished";
      case MEMORY_CONSOLIDATE_TRIAGE:
         return "triage";
      case MEMORY_CONSOLIDATE_REPAIR_FAILED:
         return "repair_failed";
      case MEMORY_CONSOLIDATE_CANCELLED:
         return "cancelled";
      case MEMORY_CONSOLIDATE_TOKENS:
         return "tokens";
      case MEMORY_CONSOLIDATE_FULL:
         return "full";
      case MEMORY_CONSOLIDATE_GO_ON:
         break;
   }
   return "go_on";
}

memory_consolidate_stop_t memory_consolidate_should_stop(const memory_consolidate_progress_t *p) {
   if (!p) {
      return MEMORY_CONSOLIDATE_CANCELLED;
   }
   if (p->complete) {
      return MEMORY_CONSOLIDATE_FINISHED;
   }
   if (p->repaired) {
      return MEMORY_CONSOLIDATE_REPAIR_FAILED;
   }
   if (p->cancelled) {
      return MEMORY_CONSOLIDATE_CANCELLED;
   }
   const int request_max = p->request_max > 0 ? p->request_max : MEMORY_CONSOLIDATE_REQUEST_MAX;
   if (p->input_tokens >= MEMORY_CONSOLIDATE_INPUT_BUDGET ||
       p->max_output > MEMORY_CONSOLIDATE_OUTPUT_BUDGET || p->next_request > request_max) {
      return MEMORY_CONSOLIDATE_TOKENS;
   }
   return MEMORY_CONSOLIDATE_GO_ON;
}

/* =============================================================================
 * The candidates: one list (from each sample taken)
 * ============================================================================= */

/* Add the reply's candidates and entities; text a candidate already has (as
 * the forgotten checks fold it) and an entity already named are left out. */
static int take_reply(mc_batch_t *b, struct json_object *root) {
   const int first_entity = b->cands.n_entities;
   if (memory_candidates_parse(root, b->in->user_id, &b->cands) != SUCCESS) {
      return FAILURE;
   }
   int kept_entities = first_entity;
   for (int i = first_entity; i < b->cands.n_entities; i++) {
      char ci[MEMORY_ENTITY_NAME_MAX];
      memory_make_canonical_name(b->cands.entities[i].name, ci, sizeof(ci));
      bool dup = false;
      for (int j = 0; j < kept_entities && !dup; j++) {
         char cj[MEMORY_ENTITY_NAME_MAX];
         memory_make_canonical_name(b->cands.entities[j].name, cj, sizeof(cj));
         dup = strcmp(ci, cj) == 0;
      }
      if (!dup) {
         b->cands.entities[kept_entities++] = b->cands.entities[i];
      }
   }
   b->cands.n_entities = kept_entities;
   return SUCCESS;
}

static void unshown(mc_batch_t *b, const memory_candidate_t *c, const char *op, const char *why) {
   if (b->n_unshown == b->cap_unshown) {
      const int cap = b->cap_unshown ? b->cap_unshown * 2 : 16;
      mc_unshown_t *grown = realloc(b->unshown, (size_t)cap * sizeof(*grown));
      if (!grown) {
         OLOG_WARNING("memory_consolidate: conv %lld: out of memory, a candidate not recorded",
                      (long long)b->in->conversation_id);
         return;
      }
      b->unshown = grown;
      b->cap_unshown = cap;
   }
   const mc_unshown_t u = { c->text, c->json, why, op };
   b->unshown[b->n_unshown++] = u;
}

/* b->c: the candidates, the same text once.  One about the assistant and a
 * repeat of another's text are dropped, one past MC_MAX_CANDIDATES set aside,
 * each recorded. */
static void list_candidates(mc_batch_t *b) {
   for (int i = 0; i < b->cands.count; i++) {
      if (memory_candidates_about_assistant(&b->cands, i, b->ai_name)) {
         unshown(b, &b->cands.items[i], "dropped", MC_REASON_ASSISTANT);
      }
   }
   memory_candidates_drop_assistant(&b->cands, b->ai_name);
   char *folded[MC_MAX_CANDIDATES] = { 0 };
   for (int i = 0; i < b->cands.count; i++) {
      char *f = memory_text_fold(b->cands.items[i].text);
      bool dup = false;
      for (int j = 0; f && j < b->n_c && !dup; j++) {
         dup = folded[j] && strcmp(folded[j], f) == 0;
      }
      if (dup || b->n_c == MC_MAX_CANDIDATES) {
         unshown(b, &b->cands.items[i], dup ? "dropped" : "deferred",
                 dup ? MC_REASON_SAME_CANDIDATE : MC_REASON_NOT_REACHED);
         free(f);
         continue;
      }
      mc_cand_t *mc = &b->c[b->n_c];
      memset(mc, 0, sizeof(*mc));
      mc->cand = i;
      mc->disp_op = -1;
      folded[b->n_c++] = f;
   }
   for (int i = 0; i < b->n_c; i++) {
      free(folded[i]);
   }
}

/* =============================================================================
 * What is stored near each candidate
 * ============================================================================= */

static int add_stored(mc_batch_t *b, const memory_fact_t *f) {
   for (int i = 0; i < b->n_s; i++) {
      if (b->s[i].id == f->id) {
         return SUCCESS;
      }
   }
   if (b->n_s >= MC_MAX_STORED) {
      return FAILURE;
   }
   b->s[b->n_s++] = *f;
   return SUCCESS;
}

/* E: @p entity_id's class, once; its index in @p idx_out. */
static int add_entity(mc_batch_t *b, int64_t entity_id, int *idx_out) {
   for (int i = 0; i < b->n_e; i++) {
      if (b->e[i].entity.id == entity_id) {
         *idx_out = i;
         return SUCCESS;
      }
   }
   if (b->n_e >= MC_MAX_ENTITIES ||
       memory_db_entity_get_with_aliases(b->in->user_id, entity_id, &b->e[b->n_e]) !=
           MEMORY_DB_SUCCESS) {
      return FAILURE;
   }
   /* The canonical may be another row already listed. */
   for (int i = 0; i < b->n_e; i++) {
      if (b->e[i].entity.id == b->e[b->n_e].entity.id) {
         *idx_out = i;
         return SUCCESS;
      }
   }
   *idx_out = b->n_e++;
   return SUCCESS;
}

/* @p name's class (its canonical's id), or 0. */
static int64_t root_of(int user_id, const char *name) {
   if (!name || !*name) {
      return 0;
   }
   char canonical[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(name, canonical, sizeof(canonical));
   int64_t root = 0;
   if (memory_db_entity_root_by_name(user_id, canonical, &root) != MEMORY_DB_SUCCESS) {
      return 0;
   }
   return root;
}

/* Up to MC_STORED_PER_C current facts like @p mc's text, above the search
 * floor, into S.  How many were found. */
static int stored_like(mc_batch_t *b, const mc_cand_t *mc) {
   const char *text = b->cands.items[mc->cand].text;
   memory_fact_t found[MC_STORED_PER_C + 3];
   float scores[MC_STORED_PER_C + 3];
   int n = 0;
   const float gate = 1.0f; /* any non-NULL: hybrid when embeddings are on */
   const int rc = mc->vec ? memory_fact_search_hybrid_ex(b->in->user_id, text, mc->vec, mc->norm, 0,
                                                         found, scores, MC_STORED_PER_C + 3, &n)
                          : memory_fact_search_hybrid(b->in->user_id, text, &gate, 0, 0, found,
                                                      scores, MC_STORED_PER_C + 3, &n);
   if (rc != SUCCESS) {
      return 0;
   }
   n = memory_search_apply_score_floor(found, scores, n, g_config.memory.search_score_floor);
   int kept = 0;
   for (int i = 0; i < n && kept < MC_STORED_PER_C; i++) {
      /* A note's gloss is never merged or replaced; an expired fact is gone. */
      if (found[i].superseded_by != 0 || found[i].note_doc_id > 0 ||
          found[i].user_id != b->in->user_id) {
         continue;
      }
      memory_fact_t full;
      if (memory_db_fact_get(found[i].id, b->in->user_id, &full) != MEMORY_DB_SUCCESS ||
          full.superseded_by != 0 || memory_db_fact_expiry_hidden(full.expires_at)) {
         continue;
      }
      kept++;
      add_stored(b, &full);
   }
   return kept;
}

/* The open relations of @p rel's subject (any of its names) with the same
 * predicate, into R.  How many there are. */
static int open_relations_like(mc_batch_t *b, const memory_candidate_rel_t *rel) {
   const int user_id = b->in->user_id;
   const int64_t root = root_of(user_id, rel->subject);
   if (root <= 0) {
      return 0;
   }
   int64_t members[MEMORY_ENTITY_CLASS_MAX];
   int n_members = 0;
   if (memory_db_entity_class_members(user_id, root, members, MEMORY_ENTITY_CLASS_MAX,
                                      &n_members) != MEMORY_DB_SUCCESS) {
      return 0;
   }
   int found = 0;
   memory_relation_t rels[MC_MAX_RELATIONS];
   for (int m = 0; m < n_members; m++) {
      int n = 0;
      if (memory_db_relation_list_open(user_id, members[m], rel->canon, rels, MC_MAX_RELATIONS,
                                       &n) != MEMORY_DB_SUCCESS) {
         continue;
      }
      found += n;
      for (int i = 0; i < n; i++) {
         bool listed = false;
         for (int k = 0; k < b->n_r && !listed; k++) {
            listed = b->r[k].rel.id == rels[i].id;
         }
         if (listed || b->n_r >= MC_MAX_RELATIONS) {
            continue;
         }
         b->r[b->n_r].rel = rels[i];
         int e = 0;
         safe_strscpy(b->r[b->n_r].subject,
                      add_entity(b, root, &e) == SUCCESS ? b->e[e].entity.name : rel->subject);
         b->n_r++;
      }
   }
   return found;
}

/* Whether @p ci may say what another candidate says (no embeddings: any may). */
static bool has_sibling(const mc_batch_t *b, int ci) {
   const mc_cand_t *a = &b->c[ci];
   for (int j = 0; j < b->n_c; j++) {
      if (j == ci) {
         continue;
      }
      const mc_cand_t *o = &b->c[j];
      if (!a->vec || !o->vec || a->dims != o->dims || a->norm <= 0.0f || o->norm <= 0.0f ||
          memory_embeddings_cosine_with_norms(a->vec, o->vec, a->dims, a->norm, o->norm) >=
              MC_SIBLING_COSINE) {
         return true;
      }
   }
   return false;
}

static void embed_candidates(mc_batch_t *b) {
   if (!memory_embeddings_available()) {
      return;
   }
   for (int i = 0; i < b->n_c; i++) {
      float vec[MAX_EMBEDDING_DIMS];
      int dims = 0;
      if (memory_embeddings_embed(b->cands.items[b->c[i].cand].text, vec, &dims) != SUCCESS ||
          dims <= 0) {
         continue;
      }
      b->c[i].vec = malloc((size_t)dims * sizeof(float));
      if (b->c[i].vec) {
         memcpy(b->c[i].vec, vec, (size_t)dims * sizeof(float));
         b->c[i].dims = dims;
         b->c[i].norm = memory_embeddings_l2_norm(vec, dims);
      }
   }
}

/* What the model sees of @p mc: its subject and the entities its relations
 * name. */
static void entities_of(mc_batch_t *b, const memory_candidate_t *cand) {
   int e = 0;
   const int64_t subject = root_of(b->in->user_id, cand->subject);
   if (subject > 0) {
      add_entity(b, subject, &e);
   }
   for (int r = 0; r < cand->n_rels; r++) {
      const memory_candidate_rel_t *rel = &b->cands.rels[cand->first_rel + r];
      const int64_t obj = rel->dropped ? 0 : root_of(b->in->user_id, rel->object);
      if (obj > 0) {
         add_entity(b, obj, &e);
      }
   }
}

/* Code alone decides what needs no judgment; the rest goes to the model, with
 * what is stored near it (S and R filled in the same pass). */
static void triage(mc_batch_t *b) {
   memory_forgotten_list_t *fl = b->in->forgotten;
   memory_forgotten_prepare(fl);
   embed_candidates(b);
   for (int i = 0; i < b->n_c; i++) {
      mc_cand_t *mc = &b->c[i];
      const memory_candidate_t *cand = &b->cands.items[mc->cand];
      int64_t exact = 0;
      if (memory_db_fact_find_exact(b->in->user_id, cand->text, &exact) == MEMORY_DB_SUCCESS &&
          exact > 0) {
         mc->triage = MC_TRI_REINFORCE;
         mc->exact_id = exact;
         b->res->triaged_reinforced++;
         continue;
      }
      if (memory_forgotten_names(fl, cand->text)) {
         mc->triage = MC_TRI_FORGOTTEN;
         b->res->triaged_forgotten++;
         continue;
      }
      /* Near a forgotten fact: never stored as new without a decision. */
      mc->near_forgotten = memory_forgotten_near(fl, cand->text);
      bool neighbour = stored_like(b, mc) > 0;
      for (int r = 0; r < cand->n_rels; r++) {
         const memory_candidate_rel_t *rel = &b->cands.rels[cand->first_rel + r];
         neighbour = (!rel->dropped && open_relations_like(b, rel) > 0) || neighbour;
      }
      neighbour = neighbour || mc->near_forgotten || has_sibling(b, i);
      if (!neighbour) {
         mc->triage = MC_TRI_NEW;
         b->res->triaged_new++;
         continue;
      }
      mc->triage = MC_TRI_MODEL;
      b->shown[b->n_shown++] = i;
      entities_of(b, cand);
   }
   b->res->to_model = b->n_shown;
}

/* The entities this batch created and those the merge cascade finds they may
 * be: the pairs a link can name. */
static void entity_pairs(mc_batch_t *b) {
   if (!g_config.memory.entity_merge_enabled) {
      return; /* nothing for a link to name */
   }
   const int64_t *fresh = NULL;
   const int n = memory_store_fresh_entities(b->st, &fresh);
   for (int i = 0; i < n && b->n_e < MC_MAX_ENTITIES; i++) {
      int64_t like[4];
      int n_like = 0;
      if (memory_db_entity_link_candidates(b->in->user_id, fresh[i], like, 4, &n_like) !=
              MEMORY_DB_SUCCESS ||
          n_like == 0) {
         continue;
      }
      int e = 0;
      if (add_entity(b, fresh[i], &e) != SUCCESS) {
         continue;
      }
      const int src = e;
      bool best = true; /* like[] is best first */
      for (int k = 0; k < n_like; k++) {
         if (add_entity(b, like[k], &e) != SUCCESS) {
            continue;
         }
         b->n_pairs++;
         if (best) {
            b->best_pair[b->n_best][0] = src;
            b->best_pair[b->n_best++][1] = e;
            best = false;
         }
      }
   }
}

/* F: the forgotten facts near something the model decides (a C or a D, at
 * MEMORY_FORGOTTEN_CANDIDATE).  The whole list would make it drop what else
 * the conversation said about the same people and topics; code checks every
 * forgotten fact, exact and near, either way. */
static void load_forgotten(mc_batch_t *b) {
   memory_forgotten_list_t *fl = b->in->forgotten;
   for (int i = 0; i < b->n_d; i++) {
      b->d[i].near_forgotten = memory_forgotten_near(fl, b->d[i].c.items[0].text);
   }
   for (int i = 0; fl && i < fl->count && b->n_f < MC_MAX_FORGOTTEN; i++) {
      if (!fl->items[i].known && fl->items[i].asked) {
         b->f[b->n_f++] = i;
      }
   }
}

static void load_deferred(mc_batch_t *b) {
   memory_consolidation_deferred_t rows[MC_MAX_DEFERRED];
   int n = 0;
   const int64_t since = (int64_t)time(NULL) - (int64_t)MEMORY_CONSOLIDATE_DEFERRED_DAYS * 86400;
   if (memory_db_consolidation_deferred_list(b->in->user_id, since, rows, MC_MAX_DEFERRED, &n) !=
       MEMORY_DB_SUCCESS) {
      return;
   }
   for (int i = 0; i < n; i++) {
      mc_deferred_t *d = &b->d[b->n_d];
      memset(d, 0, sizeof(*d));
      d->row = rows[i];
      d->root = json_tokener_parse(rows[i].candidate);
      if (!d->root || !json_object_is_type(d->root, json_type_object)) {
         json_object_put(d->root);
         free(rows[i].candidate);
         continue;
      }
      /* The candidate JSON is one fact: read it as a reply holding only it. */
      struct json_object *wrap = json_object_new_object();
      struct json_object *arr = json_object_new_array();
      json_object_array_add(arr, json_object_get(d->root));
      json_object_object_add(wrap, "facts", arr);
      json_object_put(d->root);
      d->root = wrap;
      if (memory_candidates_parse(wrap, b->in->user_id, &d->c) != SUCCESS ||
          (memory_candidates_drop_assistant(&d->c, b->ai_name), d->c.count != 1)) {
         memory_candidates_free(&d->c);
         json_object_put(d->root);
         free(rows[i].candidate);
         continue;
      }
      d->outside = outside_combine(rows[i].outside, b->outside);
      /* Stored, it is learned from the batch that set it aside. */
      d->prov.conv_id = rows[i].conversation_id;
      d->prov.msg_id_start = rows[i].msg_id_start;
      d->prov.msg_id_end = rows[i].msg_id_end;
      d->prov.outside = d->outside;
      d->disp_op = -1;
      b->n_d++;
   }
}

/* =============================================================================
 * The message
 * ============================================================================= */

static void date_of(int64_t t, char *out, size_t size) {
   out[0] = '\0';
   if (t <= 0) {
      return;
   }
   struct tm tm;
   const time_t tt = (time_t)t;
   if (gmtime_r(&tt, &tm)) {
      strftime(out, size, "%Y-%m-%d", &tm);
   }
}

static void render_relation(strbuf_t *sb, const memory_candidate_rel_t *r) {
   char from[16];
   char to[16];
   date_of(r->valid_from, from, sizeof(from));
   date_of(r->valid_to, to, sizeof(to));
   /* The canonical predicate, as the R lines show stored ones. */
   strbuf_appendf(sb, "   - %s %s %s", r->subject, r->canon[0] ? r->canon : r->predicate,
                  r->object);
   if (from[0] || to[0]) {
      strbuf_appendf(sb, " [%s..%s]", from, to);
   }
   strbuf_append(sb, "\n");
}

static void render_candidate(strbuf_t *sb,
                             const char *id,
                             const memory_candidates_t *c,
                             int idx,
                             bool tainted) {
   const memory_candidate_t *cand = &c->items[idx];
   strbuf_appendf(sb, "%s [%s%s]", id, cand->told ? "told" : "inferred",
                  tainted ? OUTSIDE_MARKER_KIND_SUFFIX : "");
   if (cand->subject && *cand->subject) {
      strbuf_appendf(sb, " (%s)", cand->subject);
   }
   strbuf_appendf(sb, " %s\n", cand->text);
   for (int r = 0; r < cand->n_rels; r++) {
      if (!c->rels[cand->first_rel + r].dropped) {
         render_relation(sb, &c->rels[cand->first_rel + r]);
      }
   }
}

static char *step_message(mc_batch_t *b) {
   char values_base[CONFIG_DESCRIPTION_MAX];
   llm_persona_half(PERSONA_VALUES, values_base, sizeof(values_base));
   auth_user_settings_t settings;
   const bool have = auth_db_get_user_settings(b->in->user_id, &settings) == AUTH_DB_SUCCESS;
   char *values = prompt_persona_half(values_base, have ? settings.persona_values : NULL,
                                      have ? settings.persona_values_mode : NULL);
   strbuf_t sb;
   strbuf_init(&sb, 8192);
   const bool links = g_config.memory.entity_merge_enabled;
   strbuf_appendf(&sb, MEMORY_CONSOLIDATE_LEAD, values ? values : values_base,
                  links ? MEMORY_CONSOLIDATE_LINK_RULE : "",
                  links ? MEMORY_CONSOLIDATE_LINK_OP : "");
   free(values);

   char id[16];
   if (b->n_shown > 0) {
      strbuf_append(&sb, MEMORY_CONSOLIDATE_HEAD_C);
      for (int i = 0; i < b->n_shown; i++) {
         snprintf(id, sizeof(id), "C%d", i + 1);
         render_candidate(&sb, id, &b->cands, b->c[b->shown[i]].cand, b->tainted);
      }
   }
   if (b->n_s > 0) {
      strbuf_append(&sb, MEMORY_CONSOLIDATE_HEAD_S);
      for (int i = 0; i < b->n_s; i++) {
         char date[16];
         date_of(b->s[i].created_at, date, sizeof(date));
         const char *word = memory_fact_kind_word(&b->s[i]);
         strbuf_appendf(&sb, "S%d [%s%s%s] %s\n", i + 1, word ? word : "",
                        word && date[0] ? " " : "", date, b->s[i].fact_text);
      }
   }
   if (b->n_r > 0) {
      strbuf_append(&sb, MEMORY_CONSOLIDATE_HEAD_R);
      for (int i = 0; i < b->n_r; i++) {
         const memory_relation_t *r = &b->r[i].rel;
         char from[16];
         date_of(r->valid_from, from, sizeof(from));
         strbuf_appendf(&sb, "R%d %s %s %s%s%s%s\n", i + 1, b->r[i].subject, r->relation,
                        r->object_name, from[0] ? " (since " : "", from, from[0] ? ")" : "");
         if (outside_is_tainted(r->outside)) {
            strbuf_append(&sb, "   " MEMORY_OUTSIDE_MARK "\n");
         }
      }
   }
   if (b->n_e > 0 && links) { /* E is what a link names */
      strbuf_append(&sb, MEMORY_CONSOLIDATE_HEAD_E);
      const int64_t *fresh = NULL;
      const int n_fresh = memory_store_fresh_entities(b->st, &fresh);
      for (int i = 0; i < b->n_e; i++) {
         const memory_entity_names_t *e = &b->e[i];
         strbuf_appendf(&sb, "E%d %s (%s", i + 1, e->entity.name, e->entity.entity_type);
         for (int k = 0; k < e->n_names; k++) {
            strbuf_appendf(&sb, "%s%s", k == 0 ? "; also " : ", ", e->names[k]);
         }
         bool is_new = false;
         for (int k = 0; k < n_fresh && !is_new; k++) {
            is_new = fresh[k] == e->entity.id;
         }
         strbuf_appendf(&sb, ")%s\n", is_new ? " new" : "");
      }
   }
   if (b->n_f > 0) {
      strbuf_append(&sb, MEMORY_CONSOLIDATE_HEAD_F);
      for (int i = 0; i < b->n_f; i++) {
         strbuf_appendf(&sb, "F%d %s\n", i + 1, b->in->forgotten->items[b->f[i]].text);
      }
   }
   if (b->n_d > 0) {
      strbuf_append(&sb, MEMORY_CONSOLIDATE_HEAD_D);
      for (int i = 0; i < b->n_d; i++) {
         snprintf(id, sizeof(id), "D%d", i + 1);
         render_candidate(&sb, id, &b->d[i].c, 0, outside_is_tainted(b->d[i].outside));
      }
   }
   return strbuf_steal_or_null(&sb);
}

static void push_message(struct json_object *history, const char *role, const char *content) {
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string(role));
   json_object_object_add(msg, "content", json_object_new_string(content));
   json_object_array_add(history, msg);
}

/* A request's size in tokens, estimated: @p known measured, plus @p added
 * bytes of new text (a conservative 3 bytes a token). */
static int estimate(int known, size_t added) {
   const size_t total = (size_t)(known > 0 ? known : 0) + added / 3;
   return total > (size_t)INT32_MAX ? INT32_MAX : (int)total;
}

/* The largest request a round may send: MEMORY_CONSOLIDATE_REQUEST_MAX, or
 * less when the model's window minus its reply is smaller. */
static int request_ceiling(const llm_resolved_config_t *cfg) {
   int ceiling = MEMORY_CONSOLIDATE_REQUEST_MAX;
   const int window = llm_context_get_size(cfg->type, cfg->cloud_provider, cfg->model);
   /* Room for the reply too; a window no larger than max_tokens is itself the
    * bound (its reply then has whatever is left). */
   const int room = window - g_config.llm.max_tokens;
   const int bound = room > 0 ? room : window;
   if (window > 0 && bound < ceiling) {
      ceiling = bound;
   }
   return ceiling;
}

/* The rounds after the extraction: the plan, and at most one repair. */
static memory_consolidate_stop_t run_rounds(mc_batch_t *b) {
   char *message = step_message(b);
   if (!message) {
      return MEMORY_CONSOLIDATE_REPAIR_FAILED; /* out of memory: as a plan that never came */
   }
   /* The rounds' own budget starts here; the samples were extraction's. */
   memory_consolidate_progress_t p = { 0 };
   p.cancelled = memory_extraction_stopping();
   p.next_request = estimate(b->in->input_tokens, strlen(b->in->reply) + strlen(message));
   p.request_max = request_ceiling(b->in->cfg);
   memory_consolidate_stop_t stop = memory_consolidate_should_stop(&p);
   if (stop != MEMORY_CONSOLIDATE_GO_ON) {
      free(message);
      return stop;
   }
   /* The extraction's own request, continued: the prompt, its reply, then this. */
   struct json_object *history = json_object_new_array();
   for (size_t i = 0; i < json_object_array_length(b->in->history); i++) {
      json_object_array_add(history, json_object_get(json_object_array_get_idx(b->in->history, i)));
   }
   push_message(history, "assistant", b->in->reply);
   push_message(history, "user", message);
   free(message);

   for (int round = 0;; round++) {
      int output = 0;
      int input = 0;
      char *reply = ask(b, history, round + 1, &output, &input);
      b->res->rounds++;
      p.max_output = output > p.max_output ? output : p.max_output;
      p.input_tokens += input;
      p.next_request = 0;
      int n_read = 0;
      const bool read = reply && mc_plan_read(b, reply, &n_read) == SUCCESS;
      if (!read) {
         OLOG_WARNING("memory_consolidate: conv %lld: round %d reply wasn't the plan asked for",
                      (long long)b->in->conversation_id, round + 1);
      }
      /* A repair that answers nothing failed, whatever the plan holds. */
      p.complete = mc_plan_complete(b) && (round == 0 || (read && n_read > 0));
      p.repaired = round > 0;
      p.cancelled = memory_extraction_stopping();
      stop = memory_consolidate_should_stop(&p);
      if (stop != MEMORY_CONSOLIDATE_GO_ON) {
         free(reply);
         break;
      }
      /* The one repair: what was rejected and what has no disposition. */
      strbuf_t problems;
      strbuf_init(&problems, 1024);
      mc_plan_problems(b, &problems);
      mc_plan_retire_rejected(b);
      const char *listed = strbuf_str(&problems);
      const int len = snprintf(NULL, 0, MEMORY_CONSOLIDATE_REPAIR_TEMPLATE, listed ? listed : "");
      char *repair = len > 0 ? malloc((size_t)len + 1) : NULL;
      if (repair) {
         snprintf(repair, (size_t)len + 1, MEMORY_CONSOLIDATE_REPAIR_TEMPLATE,
                  listed ? listed : "");
      }
      strbuf_free(&problems);
      if (!repair) {
         free(reply);
         stop = MEMORY_CONSOLIDATE_REPAIR_FAILED;
         break;
      }
      /* The repair resends this round's request, its reply and the repair. */
      p.next_request = estimate(input, (reply ? strlen(reply) : 0) + strlen(repair));
      stop = memory_consolidate_should_stop(&p);
      if (stop != MEMORY_CONSOLIDATE_GO_ON) {
         free(reply);
         free(repair);
         break;
      }
      push_message(history, "assistant", reply && reply[0] ? reply : "(no reply)");
      free(reply);
      push_message(history, "user", repair);
      free(repair);
   }
   json_object_put(history);
   return stop;
}

/* =============================================================================
 * The batch
 * ============================================================================= */

static void batch_free(mc_batch_t *b) {
   for (int i = 0; i < b->n_c; i++) {
      free(b->c[i].vec);
   }
   for (int i = 0; i < b->n_d; i++) {
      memory_candidates_free(&b->d[i].c);
      json_object_put(b->d[i].root);
      free(b->d[i].row.candidate);
   }
   memory_candidates_free(&b->cands);
   free(b->unshown);
   for (int i = 0; i < b->n_roots; i++) {
      json_object_put(b->roots[i]);
   }
   free(b);
}

/* Whether @p in's conversation may still be learned from. */
static bool allowed(const memory_consolidate_input_t *in) {
   return !in->allowed || in->allowed(in->user_id, in->conversation_id);
}

/* The batch's candidates: the extraction's reply, and, when a benchmark asks
 * for one (memory_consolidate_set_samples), a second sample of the same
 * request, unioned with it.  FAILURE: the extraction's reply isn't JSON, or
 * out of memory. */
static int take_samples(mc_batch_t *b) {
   const memory_consolidate_input_t *in = b->in;
   b->roots[0] = memory_extraction_parse_json(in->reply);
   if (!b->roots[0]) {
      OLOG_WARNING("memory_consolidate: conv %lld: the extraction reply isn't JSON",
                   (long long)in->conversation_id);
      return FAILURE;
   }
   b->n_roots = 1;
   char *second = s_samples > 1 ? ask(b, in->history, 0, NULL, NULL) : NULL;
   if (second) {
      if (in->on_reply) {
         in->on_reply(in->conversation_id, second, 1);
      }
      b->roots[1] = memory_extraction_parse_json(second);
      b->n_roots += b->roots[1] ? 1 : 0;
      free(second);
   }
   bool ok = take_reply(b, b->roots[0]) == SUCCESS;
   ok = ok && (b->n_roots < 2 || take_reply(b, b->roots[1]) == SUCCESS);
   return ok ? SUCCESS : FAILURE;
}

/* Nothing of the batch is stored: the entities it created go too. */
static int revoke(mc_batch_t *b) {
   const int64_t *fresh = NULL;
   const int n = memory_store_fresh_entities(b->st, &fresh);
   for (int i = 0; i < n; i++) {
      /* not-a-removal: created by this batch moments ago, never shown anywhere */
      memory_db_entity_delete(fresh[i], b->in->user_id);
   }
   OLOG_INFO("memory_consolidate: conv %lld: not stored (the conversation can't be learned "
             "from now)",
             (long long)b->in->conversation_id);
   memory_store_close(b->st);
   b->st = NULL;
   batch_free(b);
   return MEMORY_CONSOLIDATE_REVOKED;
}

int memory_consolidate_batch(const memory_consolidate_input_t *in,
                             memory_consolidate_result_t *out) {
   memory_consolidate_result_t local;
   memory_consolidate_result_t *res = out ? out : &local;
   memset(res, 0, sizeof(*res));
   if (!in || !in->reply || !in->history || !in->cfg) {
      return FAILURE;
   }
   mc_batch_t *b = calloc(1, sizeof(*b));
   if (!b) {
      return FAILURE;
   }
   b->in = in;
   b->res = res;
   b->outside = in->prov ? in->prov->outside : OUTSIDE_NONE;
   b->tainted = outside_is_tainted(b->outside);
   safe_strscpy(b->ai_name, g_config.general.ai_name);
   res->calls = 1;
   res->input_tokens = in->input_tokens;

   if (take_samples(b) != SUCCESS) {
      batch_free(b);
      return FAILURE;
   }
   if (!allowed(in)) {
      batch_free(b);
      return MEMORY_CONSOLIDATE_REVOKED;
   }
   b->st = memory_store_open(in->user_id, in->conversation_id, in->prov, true);
   if (!b->st) {
      OLOG_WARNING("memory_consolidate: conv %lld: out of memory", (long long)in->conversation_id);
      batch_free(b);
      return FAILURE;
   }
   list_candidates(b);
   res->candidates = b->n_c;
   memory_store_entities(b->st, &b->cands);

   triage(b);
   entity_pairs(b);
   load_deferred(b);
   load_forgotten(b);

   memory_consolidate_stop_t stop = MEMORY_CONSOLIDATE_TRIAGE;
   if (b->n_shown > 0 || b->n_d > 0 || b->n_pairs > 0) {
      stop = run_rounds(b);
   }
   const bool partial = stop != MEMORY_CONSOLIDATE_FINISHED && stop != MEMORY_CONSOLIDATE_TRIAGE;
   const bool capped = !partial && !mc_plan_within_loss_cap(b);
   if (capped) {
      OLOG_WARNING("memory_consolidate: conv %lld: the plan retires more stored items than the "
                   "loss cap allows; only additions applied",
                   (long long)in->conversation_id);
   }
   /* The model's round took time: the conversation may have gone private. */
   if (!allowed(in)) {
      return revoke(b);
   }
   res->stop = stop;
   res->partial = partial || capped;
   mc_apply(b, stop, res->partial);
   /* No round, so nothing linked: the extraction gate's pairs, recorded. */
   if (res->rounds == 0) {
      mc_link_unjudged(b);
   }
   if (b->full) {
      /* Recorded to its cap: what it couldn't record wasn't applied (a set-aside
       * candidate stays open for the next batch). */
      OLOG_WARNING("memory_consolidate: conv %lld: batch %lld is full (%d ops); the rest isn't "
                   "applied",
                   (long long)in->conversation_id, (long long)b->batch_id,
                   MEMORY_CONSOLIDATION_OPS_MAX);
      res->stop = MEMORY_CONSOLIDATE_FULL;
      res->partial = true;
      memory_db_consolidation_batch_set_stop(b->batch_id, in->user_id, "agent_partial",
                                             memory_consolidate_stop_name(MEMORY_CONSOLIDATE_FULL));
   }
#ifdef ENABLE_WEBUI
   if (res->proposed > 0) {
      webui_broadcast_memory_proposals_changed(in->user_id);
   }
#endif

   const int64_t created_at = memory_store_created_at(b->st);
   memory_store_close(b->st);
   b->st = NULL;
   const int64_t summary_id = memory_extraction_store_summary(
       in->user_id, in->conversation_id, in->session_id, b->roots[0], in->message_count,
       in->duration_seconds, in->prov, created_at);
   if (summary_id > 0 && b->batch_id > 0) {
      memory_db_consolidation_batch_set_summary(b->batch_id, in->user_id, summary_id);
   }
   if (b->batch_id > 0) {
      memory_consolidation_batch_notify(in->user_id, in->conversation_id, b->batch_id);
   }
   res->batch_id = b->batch_id;
   OLOG_INFO("memory_consolidate: conv %lld: stop=%s%s rounds=%d calls=%d input=%d candidates=%d "
             "triage=%d/%d/%d to_model=%d added=%d reinforced=%d merged=%d replaced=%d closed=%d "
             "linked=%d proposed=%d deferred=%d dropped=%d refused=%d rejected=%d",
             (long long)in->conversation_id, memory_consolidate_stop_name(stop),
             res->partial ? " (partial)" : "", res->rounds, res->calls, res->input_tokens,
             res->candidates, res->triaged_reinforced, res->triaged_forgotten, res->triaged_new,
             res->to_model, res->added, res->reinforced, res->merged, res->replaced, res->closed,
             res->linked, res->proposed, res->deferred, res->dropped, res->refused, res->rejected);
   batch_free(b);
   return SUCCESS;
}

/* =============================================================================
 * Keep
 * ============================================================================= */

/* The kept candidate into b->d[0], as load_deferred reads one (FAILURE: it
 * can't be stored: unreadable, or about the assistant). */
static int keep_candidate(mc_batch_t *b, const memory_consolidation_keep_t *k) {
   mc_deferred_t *d = &b->d[0];
   memset(d, 0, sizeof(*d));
   struct json_object *fact = json_tokener_parse(k->candidate);
   if (!fact || !json_object_is_type(fact, json_type_object)) {
      json_object_put(fact);
      return FAILURE;
   }
   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, fact);
   d->root = json_object_new_object();
   json_object_object_add(d->root, "facts", arr);
   b->n_d = 1; /* freed with the batch from here */
   if (memory_candidates_parse(d->root, b->in->user_id, &d->c) != SUCCESS ||
       (memory_candidates_drop_assistant(&d->c, b->ai_name), d->c.count != 1)) {
      return FAILURE;
   }
   d->outside = k->outside;
   d->prov = *b->in->prov;
   d->taken = true; /* its status is the Keep's to change */
   d->disp_op = -1;
   return SUCCESS;
}

/* keep_known(): nothing decided yet; store the candidate. */
#define KEEP_STORE (-2)

/* A Keep decided before any write: the batch's record full
 * (MEMORY_CONSOLIDATION_FULL), or the same text a current fact already says
 * (the op kept, pointing at it: SUCCESS, nothing reinforced, so nothing a
 * failed record would have to take back).  KEEP_STORE to store it. */
static int keep_known(mc_batch_t *b, int64_t *fact_id_out) {
   const int user_id = b->in->user_id;
   int room = 0;
   if (memory_db_consolidation_batch_room(user_id, b->batch_id, &room) != MEMORY_DB_SUCCESS) {
      return FAILURE;
   }
   if (room <= 0) {
      return MEMORY_CONSOLIDATION_FULL;
   }
   int64_t same = 0;
   memory_fact_t f;
   if (memory_db_fact_find_exact(user_id, b->d[0].c.items[0].text, &same) != MEMORY_DB_SUCCESS ||
       memory_db_fact_get(same, user_id, &f) != MEMORY_DB_SUCCESS || f.superseded_by != 0) {
      return KEEP_STORE;
   }
   if (memory_db_consolidation_keep_record(user_id, b->batch_id, b->keep_op_id, NULL, NULL) !=
       MEMORY_DB_SUCCESS) {
      return FAILURE;
   }
   *fact_id_out = same;
   return SUCCESS;
}

int memory_consolidate_keep(int user_id, int64_t batch_id, int64_t op_id, int64_t *fact_id_out) {
   *fact_id_out = 0;
   memory_consolidation_keep_t k;
   int rc = memory_db_consolidation_keep_get(user_id, batch_id, op_id, &k);
   if (rc != MEMORY_DB_SUCCESS) {
      return rc;
   }
   mc_batch_t *b = calloc(1, sizeof(*b));
   memory_forgotten_list_t forgotten;
   memset(&forgotten, 0, sizeof(forgotten));
   /* Every forgotten record of the user's (since the epoch). */
   if (!b || memory_forgotten_load(user_id, 0, 0, 1, &forgotten) != MEMORY_DB_SUCCESS) {
      memory_db_consolidation_keep_free(&k);
      free(b);
      return FAILURE;
   }
   /* Learned from the conversation it came from, with its outside text: a
    * Keep is the user's choice to store it, not its words made theirs. */
   const memory_provenance_t prov = { .conv_id = k.conversation_id,
                                      .msg_id_start = k.msg_id_start,
                                      .msg_id_end = k.msg_id_end,
                                      .outside = k.outside };
   memory_consolidate_result_t res;
   memset(&res, 0, sizeof(res));
   const memory_consolidate_input_t in = { .user_id = user_id,
                                           .conversation_id = k.conversation_id,
                                           .prov = &prov,
                                           .forgotten = &forgotten };
   b->in = &in;
   b->res = &res;
   b->outside = k.outside;
   b->tainted = outside_is_tainted(k.outside);
   b->batch_id = batch_id;
   b->keep_op_id = op_id;
   safe_strscpy(b->ai_name, g_config.general.ai_name);
   rc = keep_candidate(b, &k);
   memory_db_consolidation_keep_free(&k);
   if (rc != SUCCESS) {
      rc = MEMORY_CONSOLIDATION_NOTHING;
   } else if (memory_forgotten_names(&forgotten, b->d[0].c.items[0].text)) {
      rc = MEMORY_CONSOLIDATE_KEEP_FORGOTTEN;
   } else if ((rc = keep_known(b, fact_id_out)) != KEEP_STORE) {
      /* full, said already, or the database: decided without storing */
   } else if (!(b->st = memory_store_open(user_id, prov.conv_id, &prov, true))) {
      rc = FAILURE;
   } else {
      *fact_id_out = mc_keep_store(b);
      memory_store_close(b->st);
      b->st = NULL;
      rc = *fact_id_out > 0 && b->kept ? SUCCESS : FAILURE;
      if (rc != SUCCESS && b->keep_created > 0) {
         /* not-a-removal: the fact this Keep just made, its record refused */
         memory_db_fact_delete(b->keep_created, user_id);
         *fact_id_out = 0;
      }
   }
   OLOG_INFO("memory_consolidate: user %d kept op %lld of batch %lld: %s", user_id,
             (long long)op_id, (long long)batch_id,
             rc == SUCCESS                             ? "stored"
             : rc == MEMORY_CONSOLIDATE_KEEP_FORGOTTEN ? "refused (forgotten)"
                                                       : "not stored");
   batch_free(b);
   memory_forgotten_free(&forgotten);
   return rc;
}
