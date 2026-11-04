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
 * The consolidator's plan: each operation read from the model's JSON, checked
 * against the guards (ids from the shown set, one disposition per candidate,
 * deletion-only wording, forgotten facts, outside text), then applied through
 * the store's own calls and recorded with what it changed.
 *
 * Two properties every path keeps: outside text never acts as the user's own
 * word (a batch, or a counterpart, that read outside text replaces or closes
 * nothing that didn't; it makes nothing told and links nothing unseen; a
 * merge, which rewrites no text and is recorded and undone by restore, joins
 * only two items of the same class, both clean or UNKNOWN or both from outside
 * text), and every candidate ends with exactly one disposition (added,
 * reinforced, merged as a copy, deferred or dropped), recorded.
 */

#define MEMORY_CONSOLIDATE_INTERNAL_ALLOWED
#include <ctype.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/iso8601.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_consolidate_internal.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_forgotten.h"
#include "memory/memory_similarity.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Ids
 * ============================================================================= */

void mc_ref_name(mc_ref_t ref, char *out, size_t size) {
   static const char k_letters[] = { '?', 'C', 'S', 'R', 'E', 'D' };
   if (ref.kind == MC_REF_NONE) {
      snprintf(out, size, "?");
      return;
   }
   snprintf(out, size, "%c%d", k_letters[ref.kind], ref.i + 1);
}

/* "C3" and the like, against what the batch shows: MC_REF_NONE when absent,
 * unknown or out of range. */
static mc_ref_t ref_parse(const mc_batch_t *b, const char *s) {
   mc_ref_t r = { MC_REF_NONE, 0 };
   if (!s || !s[0] || !isdigit((unsigned char)s[1])) {
      return r;
   }
   char *end = NULL;
   const long n = strtol(s + 1, &end, 10);
   if (*end != '\0' || n < 1 || n > 10000) {
      return r;
   }
   int limit = 0;
   switch (toupper((unsigned char)s[0])) {
      case 'C':
         r.kind = MC_REF_C;
         limit = b->n_shown;
         break;
      case 'S':
         r.kind = MC_REF_S;
         limit = b->n_s;
         break;
      case 'R':
         r.kind = MC_REF_R;
         limit = b->n_r;
         break;
      case 'E':
         r.kind = MC_REF_E;
         limit = b->n_e;
         break;
      case 'D':
         r.kind = MC_REF_D;
         limit = b->n_d;
         break;
      default:
         return r;
   }
   if (n > limit) {
      r.kind = MC_REF_NONE;
      return r;
   }
   r.i = (int)n - 1;
   return r;
}

static const char *str_of(struct json_object *op, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(op, key, &v) && json_object_is_type(v, json_type_string)
              ? json_object_get_string(v)
              : NULL;
}

static bool is_cand(mc_ref_t r) {
   return r.kind == MC_REF_C || r.kind == MC_REF_D;
}

/* A candidate the batch decides on: one of its own (mc) or one an earlier
 * batch set aside (d). */
typedef struct {
   const memory_candidates_t *c;
   int idx;
   mc_cand_t *mc;
   mc_deferred_t *d;
} mc_slot_t;

static mc_slot_t slot_of_cand(mc_batch_t *b, int i) {
   const mc_slot_t s = { &b->cands, b->c[i].cand, &b->c[i], NULL };
   return s;
}

static mc_slot_t slot_of(mc_batch_t *b, mc_ref_t r) {
   if (r.kind == MC_REF_C) {
      return slot_of_cand(b, b->shown[r.i]);
   }
   const mc_slot_t s = { &b->d[r.i].c, 0, NULL, &b->d[r.i] };
   return s;
}

static const memory_candidate_t *slot_cand(mc_slot_t s) {
   return &s.c->items[s.idx];
}

/* The provenance a candidate is stored with: its own batch's when an earlier
 * batch set it aside (NULL: the batch's). */
static const memory_provenance_t *slot_prov(mc_slot_t s) {
   return s.d ? &s.d->prov : NULL;
}

static int slot_outside(const mc_batch_t *b, mc_slot_t s) {
   return s.d ? s.d->outside : b->outside;
}

/* Near something the user forgot, on a plan that never finished (what the
 * model decided may not have been its last word) or with outside text in play
 * (the batch's or its own): never stored, only set aside. */
static bool held_near_forgotten(const mc_batch_t *b, mc_slot_t s) {
   const bool near = s.mc ? s.mc->near_forgotten : s.d->near_forgotten;
   return near && (b->unfinished || b->tainted || outside_is_tainted(slot_outside(b, s)));
}

/* Whether what @p r names came from outside text. */
static bool ref_tainted(const mc_batch_t *b, mc_ref_t r) {
   switch (r.kind) {
      case MC_REF_C:
         return b->tainted;
      case MC_REF_S:
         return outside_is_tainted(b->s[r.i].outside);
      case MC_REF_R:
         return outside_is_tainted(b->r[r.i].rel.outside);
      case MC_REF_D:
         return outside_is_tainted(b->d[r.i].outside);
      default:
         return false;
   }
}

/* A candidate near a forgotten fact, with outside text in play (the batch's
 * or its own): never stored, only set aside. */
static bool near_guarded(const mc_batch_t *b, mc_ref_t r) {
   if (!is_cand(r)) {
      return false;
   }
   const bool near = r.kind == MC_REF_C ? b->c[b->shown[r.i]].near_forgotten
                                        : b->d[r.i].near_forgotten;
   return near && (b->tainted || ref_tainted(b, r));
}

/* =============================================================================
 * Checks
 * ============================================================================= */

/* Words a shortened text may not drop: they negate, date, hedge or attribute
 * what is left. */
static bool kept_word(const char *tok) {
   static const char *const k_words[] = { /* negation */
                                          "not", "no", "never", "without", "cannot", "cant", "dont",
                                          "none", "nothing", "nobody", "nor", "neither",
                                          /* time */
                                          "former", "ex", "used", "until", "previously",
                                          /* doubt and condition */
                                          "might", "may", "maybe", "probably", "perhaps", "likely",
                                          "would", "if", "unless", "considering",
                                          /* intent and attribution */
                                          "wants", "wanted", "plans", "planned", "planning",
                                          "hopes", "says", "said", "claims", "thinks", "believes",
                                          NULL
   };
   for (int i = 0; k_words[i]; i++) {
      if (strcmp(tok, k_words[i]) == 0) {
         return true;
      }
   }
   return strstr(tok, "n't") != NULL || strstr(tok, "n\xe2\x80\x99t") != NULL ||
          strncmp(tok, "ex-", 3) == 0;
}

/* @p tok without trailing punctuation, into @p out. */
static void bare(const char *tok, size_t len, char *out, size_t size) {
   while (len > 0 && strchr(",.;:!?\"')", tok[len - 1])) {
      len--;
   }
   if (len >= size) {
      len = size - 1;
   }
   memcpy(out, tok, len);
   out[len] = '\0';
}

#define MC_WORDS_MAX 160

/* Split folded @p text into words in place; -1 when there are more than @p max. */
static int words(char *text, char **out, int max) {
   int n = 0;
   char *save = NULL;
   for (char *tok = strtok_r(text, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
      if (n == max) {
         return -1;
      }
      out[n++] = tok;
   }
   return n;
}

/* Whether @p shorter is @p text with words deleted (none of them one that
 * negates, hedges or dates the rest). */
static bool deletion_only(const char *text, const char *shorter) {
   char *a = memory_text_fold(text);
   char *s = memory_text_fold(shorter);
   bool ok = a && s && s[0];
   char *wa[MC_WORDS_MAX];
   char *ws[MC_WORDS_MAX];
   const int na = ok ? words(a, wa, MC_WORDS_MAX) : 0;
   const int ns = ok ? words(s, ws, MC_WORDS_MAX) : 0;
   ok = ok && na > 0 && ns > 0 && ns <= na;
   int j = 0;
   for (int i = 0; ok && i < na; i++) {
      char ta[128];
      char ts[128];
      bare(wa[i], strlen(wa[i]), ta, sizeof(ta));
      if (j < ns) {
         bare(ws[j], strlen(ws[j]), ts, sizeof(ts));
      }
      if (j < ns && strcmp(ta, ts) == 0) {
         j++;
      } else if (kept_word(ta)) {
         ok = false; /* dropping "not" or "former" turns a fact around */
      }
   }
   ok = ok && j == ns;
   free(a);
   free(s);
   return ok;
}

static void reject(mc_op_t *op, const char *why) {
   op->status = MC_REJECTED;
   safe_strscpy(op->why, why);
}

static void refuse(mc_op_t *op, const char *why) {
   op->status = MC_REFUSED;
   safe_strscpy(op->why, why);
}

/* The disposition slot @p r (a C or D) holds. */
static mc_disp_t *disp_slot(mc_batch_t *b, mc_ref_t r, int **op_out) {
   if (r.kind == MC_REF_C) {
      *op_out = &b->c[b->shown[r.i]].disp_op;
      return &b->c[b->shown[r.i]].disp;
   }
   *op_out = &b->d[r.i].disp_op;
   return &b->d[r.i].disp;
}

/* Give @p r the disposition @p d from op @p at; FAILURE when it has another.
 * Being stored (add, or the into/by of a merge or replace) agrees with itself. */
static int dispose(mc_batch_t *b, mc_ref_t r, mc_disp_t d, int at) {
   int *op = NULL;
   mc_disp_t *slot = disp_slot(b, r, &op);
   if (*slot == MC_DISP_NONE || (*slot == MC_DISP_ADD && d == MC_DISP_ADD)) {
      *slot = d;
      if (*op < 0) {
         *op = at;
      }
      return SUCCESS;
   }
   return FAILURE;
}

/* A counterpart (into or by) that must be stored: refused, and set aside,
 * when it is near a forgotten fact with outside text in play. */
static bool counterpart_ok(mc_batch_t *b, mc_op_t *op, mc_ref_t r, int at) {
   if (!is_cand(r)) {
      return true;
   }
   if (near_guarded(b, r)) {
      if (dispose(b, r, MC_DISP_DEFER, at) == SUCCESS) {
         refuse(op, "the new value is near something the user asked to forget; set aside");
      } else {
         reject(op, "names a candidate that already has another disposition");
      }
      return false;
   }
   if (dispose(b, r, MC_DISP_ADD, at) != SUCCESS) {
      reject(op, "names a candidate that already has another disposition");
      return false;
   }
   return true;
}

/* Whether the stored fact @p r names is still current (another op may have
 * retired it since it was shown). */
static bool still_current(const mc_batch_t *b, mc_ref_t r) {
   memory_fact_t now;
   return memory_db_fact_get(b->s[r.i].id, b->in->user_id, &now) == MEMORY_DB_SUCCESS &&
          now.superseded_by == 0;
}

/* An op that retires @p target: refused when outside text (the batch, or the
 * counterpart @p by) would retire something that didn't come from it. */
static bool laundering(const mc_batch_t *b, mc_ref_t target, mc_ref_t by) {
   const bool retirer = b->tainted || (by.kind != MC_REF_NONE && ref_tainted(b, by));
   return retirer && !ref_tainted(b, target);
}

static void check_add(mc_batch_t *b, mc_op_t *op, int at) {
   if (!is_cand(op->a)) {
      reject(op, "add names a C or D shown here");
      return;
   }
   const mc_slot_t s = slot_of(b, op->a);
   const char *text = slot_cand(s)->text;
   if (op->has_text) {
      if (!s.mc) {
         reject(op, "only a C's text may be shortened");
         return;
      }
      if (strlen(op->text) >= MEMORY_FACT_TEXT_MAX - 1 || !deletion_only(text, op->text)) {
         reject(op, "text may only delete words of the C (and never one that negates or dates "
                    "the rest: not, no, never, n't, former, until, ...)");
         return;
      }
      text = op->text;
   }
   if (memory_forgotten_names(b->in->forgotten, text)) {
      reject(op, "it says what the user asked you to forget");
      return;
   }
   if (near_guarded(b, op->a)) {
      if (dispose(b, op->a, MC_DISP_DEFER, at) != SUCCESS) {
         reject(op, "it already has another disposition");
         return;
      }
      refuse(op, "near something the user asked to forget, with outside text; set aside");
      return;
   }
   if (dispose(b, op->a, MC_DISP_ADD, at) != SUCCESS) {
      reject(op, "it already has another disposition");
      return;
   }
   if (op->has_text) {
      safe_strscpy(s.mc->text, op->text);
      s.mc->shortened = true;
   }
}

/* A candidate that says what another says: into an S (reinforce it) or a C
 * (dropped as its copy; never into one that is itself a copy). */
static void check_copy(mc_batch_t *b, mc_op_t *op, int at, mc_ref_t into) {
   if (into.kind == MC_REF_C) {
      if (op->a.kind == MC_REF_C && into.i == op->a.i) {
         reject(op, "a C can't be merged into itself");
         return;
      }
      if (b->c[b->shown[into.i]].disp == MC_DISP_MERGE_C) {
         reject(op, "into names a C that is itself merged into another C");
         return;
      }
   }
   const mc_disp_t d = into.kind == MC_REF_S ? MC_DISP_MERGE_S : MC_DISP_MERGE_C;
   if (dispose(b, op->a, d, at) != SUCCESS) {
      reject(op, "it already has another disposition");
      return;
   }
   if (op->a.kind == MC_REF_C) {
      b->c[b->shown[op->a.i]].merge_into = into;
   }
}

static void check_merge(mc_batch_t *b, mc_op_t *op, int at) {
   const mc_ref_t into = op->b;
   if (into.kind != MC_REF_S && into.kind != MC_REF_C) {
      reject(op, "into names an S or a C shown here");
      return;
   }
   if (into.kind == MC_REF_S && !still_current(b, into)) {
      reject(op, "that S was merged or replaced meanwhile");
      return;
   }
   if (is_cand(op->a)) {
      check_copy(b, op, at, into);
      return;
   }
   if (op->a.kind != MC_REF_S) {
      reject(op, "merge needs \"c\" (a C or D) or \"from\" (an S)");
      return;
   }
   if (into.kind == MC_REF_S && into.i == op->a.i) {
      reject(op, "an S can't be merged into itself");
      return;
   }
   if (!still_current(b, op->a)) {
      reject(op, "that S was merged or replaced meanwhile");
      return;
   }
   op->destructive = true;
   /* The fuller C is kept: it is stored whatever becomes of the merge. */
   if (!counterpart_ok(b, op, into, at)) {
      return;
   }
   /* A merge joins two items of one class (a C is the batch's). */
   if (ref_tainted(b, op->a) != ref_tainted(b, into)) {
      refuse(op, "a merge joins only facts that both came from outside text, or both didn't");
   }
}

static void check_replace(mc_batch_t *b, mc_op_t *op, int at) {
   if (op->a.kind != MC_REF_S || !(op->b.kind == MC_REF_S || is_cand(op->b))) {
      reject(op, "replace needs \"old\" (an S) and \"by\" (a C, D or S)");
      return;
   }
   if (op->b.kind == MC_REF_S && op->b.i == op->a.i) {
      reject(op, "an S can't replace itself");
      return;
   }
   if (!still_current(b, op->a) || (op->b.kind == MC_REF_S && !still_current(b, op->b))) {
      reject(op, "that S was merged or replaced meanwhile");
      return;
   }
   op->destructive = true;
   if (!counterpart_ok(b, op, op->b, at)) {
      return;
   }
   if (laundering(b, op->a, op->b)) {
      /* The new value is still stored, beside the old one. */
      refuse(op, "outside text can't retire a stored fact that didn't come from it");
   }
}

static void check_close(mc_batch_t *b, mc_op_t *op, int at) {
   if (op->a.kind != MC_REF_R) {
      reject(op, "close_relation names an R shown here");
      return;
   }
   if (op->b.kind != MC_REF_NONE && op->b.kind != MC_REF_S && !is_cand(op->b)) {
      reject(op, "by names a C, D or S");
      return;
   }
   const memory_relation_t *rel = &b->r[op->a.i].rel;
   const int64_t tomorrow = (int64_t)time(NULL) + 86400;
   if (op->valid_to <= 0) {
      reject(op, "valid_to must be a date, YYYY-MM-DD");
      return;
   }
   if (op->valid_to > tomorrow || (rel->valid_from > 0 && op->valid_to < rel->valid_from)) {
      reject(op, "valid_to must fall between when the relation began and today");
      return;
   }
   op->destructive = true;
   if (!counterpart_ok(b, op, op->b, at)) {
      return;
   }
   if (laundering(b, op->a, op->b)) {
      refuse(op, "outside text can't close a relation that didn't come from it");
   }
}

static void check_op(mc_batch_t *b, mc_op_t *op, int at) {
   switch (op->kind) {
      case MC_OP_ADD:
         check_add(b, op, at);
         break;
      case MC_OP_MERGE:
         check_merge(b, op, at);
         break;
      case MC_OP_REPLACE:
         check_replace(b, op, at);
         break;
      case MC_OP_CLOSE:
         check_close(b, op, at);
         break;
      case MC_OP_LINK:
         if (op->a.kind != MC_REF_E || op->b.kind != MC_REF_E || op->a.i == op->b.i) {
            reject(op, "link names two different E shown here");
         } else {
            op->destructive = !b->tainted; /* a proposal from outside text */
         }
         break;
      case MC_OP_DEFER:
      case MC_OP_DROP:
         if (!is_cand(op->a)) {
            reject(op, "names a C or D shown here");
         } else if (dispose(b, op->a, op->kind == MC_OP_DEFER ? MC_DISP_DEFER : MC_DISP_DROP, at) !=
                    SUCCESS) {
            reject(op, "it already has another disposition");
         }
         break;
      case MC_OP_FORGET:
         if (op->a.kind != MC_REF_S) {
            reject(op, "forget names an S shown here");
         }
         break;
   }
}

/* =============================================================================
 * Reading a plan
 * ============================================================================= */

static int op_kind(const char *name, mc_op_kind_t *out) {
   static const struct {
      const char *name;
      mc_op_kind_t kind;
   } k_ops[] = { { "add", MC_OP_ADD },         { "merge", MC_OP_MERGE },
                 { "replace", MC_OP_REPLACE }, { "close_relation", MC_OP_CLOSE },
                 { "link", MC_OP_LINK },       { "defer", MC_OP_DEFER },
                 { "drop", MC_OP_DROP },       { "forget", MC_OP_FORGET } };
   for (size_t i = 0; name && i < sizeof(k_ops) / sizeof(k_ops[0]); i++) {
      if (strcmp(name, k_ops[i].name) == 0) {
         *out = k_ops[i].kind;
         return SUCCESS;
      }
   }
   return FAILURE;
}

static void read_op(mc_batch_t *b, struct json_object *j, mc_op_t *op) {
   memset(op, 0, sizeof(*op));
   const char *reason = str_of(j, "reason");
   if (reason) {
      safe_strscpy(op->reason, reason);
      utf8_truncate(op->reason, MEMORY_CONSOLIDATE_REASON_MAX);
   }
   if (op_kind(str_of(j, "op"), &op->kind) != SUCCESS) {
      op->kind = MC_OP_FORGET;
      reject(op, "unknown op (add, merge, replace, close_relation, link, defer, drop, forget)");
      return;
   }
   switch (op->kind) {
      case MC_OP_ADD:
      case MC_OP_DEFER:
      case MC_OP_DROP:
         op->a = ref_parse(b, str_of(j, "c"));
         break;
      case MC_OP_MERGE:
         op->a = ref_parse(b, str_of(j, "c") ? str_of(j, "c") : str_of(j, "from"));
         op->b = ref_parse(b, str_of(j, "into"));
         break;
      case MC_OP_REPLACE:
         op->a = ref_parse(b, str_of(j, "old"));
         op->b = ref_parse(b, str_of(j, "by"));
         break;
      case MC_OP_CLOSE:
         op->a = ref_parse(b, str_of(j, "r"));
         op->b = ref_parse(b, str_of(j, "by"));
         if (str_of(j, "by") && op->b.kind == MC_REF_NONE) {
            reject(op, "by names an id not shown here");
            return;
         }
         op->valid_to = str_of(j, "valid_to") ? iso8601_parse_date_utc(str_of(j, "valid_to")) : 0;
         break;
      case MC_OP_LINK:
         if (!g_config.memory.entity_merge_enabled) {
            reject(op, "entity merging is off"); /* no E is shown then */
            return;
         }
         op->a = ref_parse(b, str_of(j, "e"));
         op->b = ref_parse(b, str_of(j, "same_as"));
         break;
      case MC_OP_FORGET:
         op->a = ref_parse(b, str_of(j, "s"));
         break;
   }
   const char *text = str_of(j, "text");
   if (op->kind == MC_OP_ADD && text) {
      safe_strscpy(op->text, text);
      op->has_text = true;
   }
   if (op->a.kind == MC_REF_NONE) {
      reject(op, "names an id not shown here");
   }
}

/* Whether @p op says what an op already in the plan says (a reply restating
 * its plan, or one op twice): nothing to add. */
static bool already_planned(const mc_batch_t *b, const mc_op_t *op) {
   for (int i = 0; i < b->n_ops; i++) {
      const mc_op_t *o = &b->ops[i];
      if ((o->status == MC_OK || o->status == MC_REFUSED) && o->kind == op->kind &&
          o->a.kind == op->a.kind && o->a.i == op->a.i && o->b.kind == op->b.kind &&
          o->b.i == op->b.i && o->valid_to == op->valid_to && o->has_text == op->has_text &&
          (!op->has_text || strcmp(o->text, op->text) == 0)) {
         return true;
      }
   }
   return false;
}

int mc_plan_read(mc_batch_t *b, const char *reply, int *n_read) {
   *n_read = 0;
   struct json_object *root = reply ? memory_extraction_parse_json(reply) : NULL;
   struct json_object *ops = NULL;
   if (root && json_object_is_type(root, json_type_array)) {
      ops = root;
   } else if (!root || !json_object_object_get_ex(root, "ops", &ops) ||
              !json_object_is_type(ops, json_type_array)) {
      json_object_put(root);
      return FAILURE;
   }
   const int n = (int)json_object_array_length(ops);
   for (int i = 0; i < n && b->n_ops < MC_MAX_OPS; i++) {
      mc_op_t *op = &b->ops[b->n_ops];
      read_op(b, json_object_array_get_idx(ops, i), op);
      (*n_read)++;
      if (op->status == MC_OK && already_planned(b, op)) {
         continue; /* the slot is reused by the next op */
      }
      if (op->status == MC_OK) {
         check_op(b, op, b->n_ops);
      }
      b->n_ops++;
   }
   json_object_put(root);
   return SUCCESS;
}

bool mc_plan_complete(const mc_batch_t *b) {
   for (int i = 0; i < b->n_ops; i++) {
      if (b->ops[i].status == MC_REJECTED) {
         return false;
      }
   }
   for (int i = 0; i < b->n_shown; i++) {
      if (b->c[b->shown[i]].disp == MC_DISP_NONE) {
         return false;
      }
   }
   return true;
}

void mc_plan_problems(const mc_batch_t *b, strbuf_t *out) {
   static const char *const k_names[] = { "add",  "merge", "replace", "close_relation",
                                          "link", "defer", "drop",    "forget" };
   for (int i = 0; i < b->n_ops; i++) {
      const mc_op_t *op = &b->ops[i];
      if (op->status != MC_REJECTED) {
         continue;
      }
      char a[16];
      mc_ref_name(op->a, a, sizeof(a));
      strbuf_appendf(out, "- op %d (%s %s): %s\n", i + 1, k_names[op->kind], a, op->why);
   }
   for (int i = 0; i < b->n_shown; i++) {
      if (b->c[b->shown[i]].disp == MC_DISP_NONE) {
         strbuf_appendf(out, "- C%d has no disposition\n", i + 1);
      }
   }
   if (strbuf_len(out) == 0) {
      strbuf_append(out, "- the reply wasn't the JSON plan asked for\n");
   }
}

void mc_plan_retire_rejected(mc_batch_t *b) {
   for (int i = 0; i < b->n_ops; i++) {
      if (b->ops[i].status == MC_REJECTED) {
         b->ops[i].status = MC_REPLACED;
         b->res->rejected++;
      }
   }
}

bool mc_plan_within_loss_cap(const mc_batch_t *b) {
   int destructive = 0;
   for (int i = 0; i < b->n_ops; i++) {
      destructive += b->ops[i].status == MC_OK && b->ops[i].destructive;
   }
   /* Stored facts and relations: entities shown for links don't widen it. */
   const int shown = b->n_s + b->n_r;
   int cap = (int)(MC_LOSS_SHARE * (double)shown);
   if (cap < MC_LOSS_FLOOR) {
      cap = MC_LOSS_FLOOR;
   }
   return destructive <= cap;
}

/* =============================================================================
 * Recording
 * ============================================================================= */

/* A JSON object as a string (heap); @p obj is released. */
static char *image(struct json_object *obj) {
   char *s = strdup(json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN));
   json_object_put(obj);
   return s;
}

static struct json_object *fact_json(int user_id, int64_t fact_id) {
   memory_fact_t f;
   struct json_object *o = json_object_new_object();
   if (memory_db_fact_get(fact_id, user_id, &f) == MEMORY_DB_SUCCESS) {
      json_object_object_add(o, "text", json_object_new_string(f.fact_text));
      json_object_object_add(o, "confidence", json_object_new_double(f.confidence));
      json_object_object_add(o, "source", json_object_new_string(f.source));
      json_object_object_add(o, "outside", json_object_new_int(f.outside));
      json_object_object_add(o, "superseded_by",
                             f.superseded_by ? json_object_new_int64(f.superseded_by) : NULL);
   } else {
      json_object_object_add(o, "absent", json_object_new_boolean(1));
   }
   return o;
}

static void record(mc_batch_t *b, memory_consolidation_op_t *op) {
   if (b->batch_id <= 0) {
      return;
   }
   op->batch_id = b->batch_id;
   op->user_id = b->in->user_id;
   op->seq = ++b->seq;
   if (memory_db_consolidation_op_add(op, NULL) != MEMORY_DB_SUCCESS) {
      OLOG_WARNING("memory_consolidate: batch %lld: op %d not recorded", (long long)b->batch_id,
                   op->seq);
   }
}

static const char *fact_text(int user_id, int64_t fact_id, memory_fact_t *buf) {
   return memory_db_fact_get(fact_id, user_id, buf) == MEMORY_DB_SUCCESS ? buf->fact_text : NULL;
}

/* A candidate an earlier batch set aside is taken up: by this batch, however
 * its decision ends. */
static void consume(mc_batch_t *b, mc_slot_t s) {
   if (s.d) {
      memory_db_consolidation_op_set_status(s.d->row.op_id, b->in->user_id, "consumed");
   }
}

static void record_forgotten(mc_batch_t *b, mc_slot_t s, const char *reason);

/* Whether a D may still be acted on: taken up now (its row consumed) while its
 * own conversation can still be learned from.  Made private or deleted since
 * it was loaded: nothing of it is stored or recorded. */
static bool d_taken(mc_batch_t *b, mc_slot_t s) {
   if (!s.d) {
      return true;
   }
   if (s.d->taken) {
      return true;
   }
   const int rc = memory_db_consolidation_deferred_claim(s.d->row.op_id, b->in->user_id);
   if (rc == MEMORY_DB_SUCCESS) {
      s.d->taken = true;
      return true;
   }
   if (rc == MEMORY_DB_NOT_FOUND) {
      OLOG_INFO("memory_consolidate: batch %lld: a set-aside candidate's conversation can't be "
                "learned from now; left out",
                (long long)b->batch_id);
   } else {
      OLOG_WARNING("memory_consolidate: batch %lld: taking up a set-aside candidate failed "
                   "(database); left out",
                   (long long)b->batch_id);
   }
   s.d->done = true;
   return false;
}

/* The conversation a row about @p s quotes, when it isn't the batch's. */
static int64_t slot_source(mc_slot_t s) {
   return s.d ? s.d->row.conversation_id : 0;
}

/* Record a candidate set aside or dropped (@p what), with its JSON.  A D set
 * aside again keeps its own row (its batch's conversation, its expiry); one
 * whose words the user forgot is recorded without them. */
static void record_candidate(mc_batch_t *b, mc_slot_t s, const char *what, const char *reason) {
   const memory_candidate_t *cand = slot_cand(s);
   const bool forgotten = memory_forgotten_names(b->in->forgotten, cand->text);
   if (s.d && !forgotten && strcmp(what, "deferred") == 0) {
      return;
   }
   if (!d_taken(b, s)) {
      return;
   }
   if (forgotten) {
      record_forgotten(b, s, MC_REASON_BACKSTOP_FORGOTTEN);
      return;
   }
   memory_consolidation_op_t op = {
      .op = what,
      .outside = slot_outside(b, s),
      .source_conversation_id = slot_source(s),
      .text = cand->text,
      .reason = reason,
      .pre_image = cand->json ? json_object_to_json_string_ext(cand->json, JSON_C_TO_STRING_PLAIN)
                              : NULL
   };
   record(b, &op);
   if (strcmp(what, "deferred") == 0) {
      b->res->deferred++;
   } else {
      b->res->dropped++;
   }
   consume(b, s);
}

/* A candidate dropped because it says what the user forgot: recorded without
 * its words (the record must not keep what a forget removed). */
static void record_forgotten(mc_batch_t *b, mc_slot_t s, const char *reason) {
   memory_consolidation_op_t op = { .op = "dropped",
                                    .outside = slot_outside(b, s),
                                    .source_conversation_id = slot_source(s),
                                    .reason = reason };
   record(b, &op);
   b->res->dropped++;
   consume(b, s);
}

/* A stored fact said again: reinforced, recorded with what it was before
 * (confidence, kind, whether this conversation was a source, its source
 * conversation and range, which a reinforce may move). */
static int reinforce(mc_batch_t *b,
                     int64_t fact_id,
                     bool told,
                     const char *said,
                     const char *why,
                     const memory_provenance_t *prov) {
   const int user_id = b->in->user_id;
   memory_fact_t before;
   if (memory_db_fact_get(fact_id, user_id, &before) != MEMORY_DB_SUCCESS ||
       before.superseded_by != 0) {
      return FAILURE;
   }
   const memory_provenance_t *p = prov ? prov : b->in->prov;
   struct json_object *pre = fact_json(user_id, fact_id);
   memory_consolidation_fact_source_t src;
   if (p && p->conv_id > 0 &&
       memory_db_consolidation_fact_source(user_id, fact_id, p->conv_id, &src) ==
           MEMORY_DB_SUCCESS) {
      json_object_object_add(pre, "had_source", json_object_new_boolean(src.had_conversation));
      json_object_object_add(pre, "source_conversation_id",
                             json_object_new_int64(src.source_conversation_id));
      json_object_object_add(pre, "msg_id_start", json_object_new_int64(src.msg_id_start));
      json_object_object_add(pre, "msg_id_end", json_object_new_int64(src.msg_id_end));
   }
   char *pre_s = image(pre);
   memory_store_reinforce(b->st, fact_id, told, prov);
   char *post_s = image(fact_json(user_id, fact_id));
   memory_consolidation_op_t op = { .op = "reinforced",
                                    .fact_id = fact_id,
                                    .outside = p ? p->outside : b->outside,
                                    .source_conversation_id = prov ? prov->conv_id : 0,
                                    .text = before.fact_text,
                                    .target_text = said,
                                    .reason = why,
                                    .pre_image = pre_s,
                                    .post_image = post_s };
   record(b, &op);
   free(pre_s);
   free(post_s);
   b->res->reinforced++;
   return SUCCESS;
}

/* Store a candidate (once): its fact (or the stored fact it repeats,
 * reinforced), recorded; @p unjudged adds that no plan decided first look for
 * a paraphrase stored (paraphrase_dedup_threshold).  Its fact id, 0 when not
 * stored (recorded why). */
static int64_t store_slot(mc_batch_t *b, mc_slot_t s, const char *why, bool unjudged) {
   int64_t *fact_id = s.mc ? &s.mc->fact_id : &s.d->fact_id;
   bool *done = s.mc ? &s.mc->stored : &s.d->done;
   if (*done) {
      return *fact_id;
   }
   if (held_near_forgotten(b, s)) {
      *done = true;
      record_candidate(b, s, "deferred", MC_REASON_NEAR_FORGOTTEN);
      return 0;
   }
   if (!d_taken(b, s)) {
      return 0;
   }
   *done = true; /* one disposition, whatever follows */
   const memory_candidate_t *cand = slot_cand(s);
   const bool shortened = s.mc && s.mc->shortened;
   const char *text = shortened ? s.mc->text : cand->text;
   /* The exact-text backstop: nothing the user forgot is stored again. */
   if (memory_forgotten_names(b->in->forgotten, text)) {
      record_forgotten(b, s, MC_REASON_BACKSTOP_FORGOTTEN);
      return 0;
   }
   const memory_provenance_t *prov = slot_prov(s);
   const memory_store_opts_t opts = { .text = shortened ? s.mc->text : NULL,
                                      .prov = prov,
                                      .keep_exact = true,
                                      .paraphrase = unjudged,
                                      .vec = s.mc ? s.mc->vec : NULL,
                                      .dims = s.mc ? s.mc->dims : 0 };
   memory_store_result_t res;
   if (memory_store_candidate(b->st, s.c, s.idx, &opts, &res) != SUCCESS || res.fact_id <= 0) {
      memory_consolidation_op_t op = { .op = "added",
                                       .outside = slot_outside(b, s),
                                       .source_conversation_id = slot_source(s),
                                       .text = text,
                                       .reason = MC_REASON_NOT_STORED,
                                       .status = "refused" };
      record(b, &op);
      b->res->refused++;
      consume(b, s);
      return 0;
   }
   *fact_id = res.fact_id;
   if (res.reused) {
      if (reinforce(b, res.fact_id, cand->told, text, why, prov) != SUCCESS) {
         *fact_id = 0; /* retired meanwhile */
      }
      consume(b, s);
      return *fact_id;
   }
   char *post = image(fact_json(b->in->user_id, res.fact_id));
   memory_consolidation_op_t op = { .op = "added",
                                    .fact_id = res.fact_id,
                                    .outside = slot_outside(b, s),
                                    .source_conversation_id = slot_source(s),
                                    .text = text,
                                    .reason = why,
                                    .post_image = post };
   record(b, &op);
   free(post);
   b->res->added++;
   consume(b, s);
   return res.fact_id;
}

/* The fact id @p ref stands for: an S's, or a candidate's once stored. */
static int64_t fact_of(mc_batch_t *b, mc_ref_t ref, const char *why) {
   if (ref.kind == MC_REF_S) {
      return b->s[ref.i].id;
   }
   return is_cand(ref) ? store_slot(b, slot_of(b, ref), why, false) : 0;
}

static void record_refused(mc_batch_t *b, const mc_op_t *op, const char *why) {
   static const char *const k_names[] = { "added",  "merged",   "replaced", "closed",
                                          "linked", "deferred", "dropped",  "suggested_forget" };
   memory_fact_t buf;
   const char *text = op->a.kind == MC_REF_S ? fact_text(b->in->user_id, b->s[op->a.i].id, &buf)
                                             : NULL;
   int64_t source = 0;
   if (is_cand(op->a)) {
      const mc_slot_t s = slot_of(b, op->a);
      /* A D whose conversation can't be learned from now: no row quotes it. */
      if (s.d && !s.d->taken &&
          memory_db_consolidation_deferred_open(s.d->row.op_id, b->in->user_id) !=
              MEMORY_DB_SUCCESS) {
         return;
      }
      text = slot_cand(s)->text;
      source = slot_source(s);
   }
   char reason[MEMORY_CONSOLIDATE_REASON_MAX + 1];
   snprintf(reason, sizeof(reason), "%s", why);
   memory_consolidation_op_t row = { .op = k_names[op->kind],
                                     .fact_id = op->a.kind == MC_REF_S ? b->s[op->a.i].id : 0,
                                     .relation_id = op->a.kind == MC_REF_R ? b->r[op->a.i].rel.id
                                                                           : 0,
                                     .outside = b->outside,
                                     .source_conversation_id = source,
                                     .text = text,
                                     .reason = reason,
                                     .status = "refused" };
   record(b, &row);
   b->res->refused++;
}

/* =============================================================================
 * Applying, one op kind each
 * ============================================================================= */

/* Retire stored fact @p from by @p into (merged or replaced), recorded.  The
 * outside-text rules are checked again on the rows as they are now. */
static void apply_retire(mc_batch_t *b, const mc_op_t *op, bool merge) {
   const int user_id = b->in->user_id;
   const int64_t from = b->s[op->a.i].id;
   const int64_t into = fact_of(b, op->b, op->reason);
   if (into <= 0) {
      record_refused(b, op, "the new value wasn't stored");
      return;
   }
   memory_fact_t from_fact;
   memory_fact_t into_fact;
   if (memory_db_fact_get(from, user_id, &from_fact) != MEMORY_DB_SUCCESS ||
       memory_db_fact_get(into, user_id, &into_fact) != MEMORY_DB_SUCCESS) {
      record_refused(b, op, "a fact changed since it was shown");
      return;
   }
   const bool from_out = outside_is_tainted(from_fact.outside);
   const bool into_out = outside_is_tainted(into_fact.outside);
   if (merge ? from_out != into_out : (b->tainted || into_out) && !from_out) {
      record_refused(b, op, "outside text can't retire a stored fact that didn't come from it");
      return;
   }
   const int rc = merge ? memory_db_fact_merge(from, into, user_id)
                        : memory_db_fact_replace(from, into, user_id);
   if (rc != MEMORY_DB_SUCCESS) {
      record_refused(b, op, "a fact changed since it was shown");
      return;
   }
   struct json_object *post = json_object_new_object();
   json_object_object_add(post, "superseded_by", json_object_new_int64(into));
   json_object_object_add(post, "kind", json_object_new_string(merge ? "merged" : "replaced"));
   char *post_s = image(post);
   memory_consolidation_op_t row = { .op = merge ? "merged" : "replaced",
                                     .fact_id = from,
                                     .other_fact_id = into,
                                     .outside = b->outside,
                                     .text = from_fact.fact_text,
                                     .target_text = into_fact.fact_text,
                                     .reason = op->reason,
                                     .post_image = post_s };
   record(b, &row);
   free(post_s);
   if (merge) {
      b->res->merged++;
   } else {
      b->res->replaced++;
   }
}

static void apply_close(mc_batch_t *b, const mc_op_t *op) {
   const mc_rel_t *r = &b->r[op->a.i];
   const int64_t by = op->b.kind != MC_REF_NONE ? fact_of(b, op->b, op->reason) : 0;
   if (is_cand(op->b) && by <= 0) {
      record_refused(b, op, "the new value wasn't stored");
      return;
   }
   memory_fact_t by_fact;
   const bool by_out = by > 0 &&
                       memory_db_fact_get(by, b->in->user_id, &by_fact) == MEMORY_DB_SUCCESS &&
                       outside_is_tainted(by_fact.outside);
   if ((b->tainted || by_out) && !outside_is_tainted(r->rel.outside)) {
      record_refused(b, op, "outside text can't close a relation that didn't come from it");
      return;
   }
   if (memory_db_relation_close(b->in->user_id, r->rel.id, op->valid_to, by) != MEMORY_DB_SUCCESS) {
      record_refused(b, op, "the relation changed since it was shown");
      return;
   }
   char text[MEMORY_FACT_TEXT_MAX];
   snprintf(text, sizeof(text), "%s %s %s", r->subject, r->rel.relation, r->rel.object_name);
   struct json_object *pre = json_object_new_object();
   json_object_object_add(pre, "valid_to", NULL);
   json_object_object_add(pre, "closed_by", NULL);
   struct json_object *post = json_object_new_object();
   json_object_object_add(post, "valid_to", json_object_new_int64(op->valid_to));
   json_object_object_add(post, "closed_by", by > 0 ? json_object_new_int64(by) : NULL);
   char *pre_s = image(pre);
   char *post_s = image(post);
   memory_consolidation_op_t row = { .op = "closed",
                                     .relation_id = r->rel.id,
                                     .other_fact_id = by,
                                     .outside = b->outside,
                                     .text = text,
                                     .reason = op->reason,
                                     .pre_image = pre_s,
                                     .post_image = post_s };
   record(b, &row);
   free(pre_s);
   free(post_s);
   b->res->closed++;
}

/* Entities E @p a and @p b through the link ladder; from a batch that read
 * outside text, a proposal the user decides.  Recorded when linked or
 * proposed; false when the ladder refused the pair. */
static bool link_pair(mc_batch_t *b, int a, int bi, const char *reason) {
   const memory_entity_t *src = &b->e[a].entity;
   const memory_entity_t *tgt = &b->e[bi].entity;
   memory_alias_evaluate_t eval;
   if (memory_db_entity_consider_nominated_merge(b->in->user_id, src->id, tgt->id, b->tainted,
                                                 &eval) != MEMORY_DB_SUCCESS ||
       (eval.outcome != MEMORY_ALIAS_OUTCOME_AUTO_MERGED &&
        eval.outcome != MEMORY_ALIAS_OUTCOME_PROPOSED)) {
      return false;
   }
   const bool linked = eval.outcome == MEMORY_ALIAS_OUTCOME_AUTO_MERGED;
   struct json_object *post = json_object_new_object();
   json_object_object_add(post, "link_id", json_object_new_int64(eval.link_id));
   json_object_object_add(post, "proposal_id", json_object_new_int64(eval.proposal_id));
   char *post_s = image(post);
   memory_consolidation_op_t row = {
      .op = linked ? "linked" : "proposed",
      .entity_id = eval.source_entity_id,
      .other_entity_id = eval.target_entity_id,
      .link_id = eval.link_id,
      .outside = b->outside,
      .text = eval.source_entity_id == src->id ? src->name : tgt->name,
      .target_text = eval.source_entity_id == src->id ? tgt->name : src->name,
      .reason = reason,
      .post_image = post_s
   };
   record(b, &row);
   free(post_s);
   if (linked) {
      b->res->linked++;
   } else {
      b->res->proposed++;
   }
   return true;
}

static void apply_link(mc_batch_t *b, const mc_op_t *op) {
   if (!link_pair(b, op->a.i, op->b.i, op->reason)) {
      record_refused(b, op, "the two entities differ by type or name");
   }
}

void mc_link_unjudged(mc_batch_t *b) {
   if (!g_config.memory.entity_merge_enabled) {
      return;
   }
   /* A pair the ladder refuses is left alone, as the extraction gate does. */
   for (int i = 0; i < b->n_best; i++) {
      link_pair(b, b->best_pair[i][0], b->best_pair[i][1], MC_REASON_NO_ROUND);
   }
}

/* A candidate that says what something else says: into an S (reinforced; if
 * that one was retired meanwhile, stored instead) or another C (dropped as its
 * copy). */
static void apply_copy(mc_batch_t *b, const mc_op_t *op) {
   const mc_slot_t s = slot_of(b, op->a);
   if (op->a.kind == MC_REF_D || s.mc->disp == MC_DISP_MERGE_C) {
      char why[MEMORY_CONSOLIDATE_REASON_MAX + 1];
      if (op->reason[0]) {
         snprintf(why, sizeof(why), "%s", op->reason);
      } else {
         char into[16];
         mc_ref_name(op->b, into, sizeof(into));
         snprintf(why, sizeof(why), "says what %s says", into);
      }
      record_candidate(b, s, "dropped", why);
      return;
   }
   const memory_candidate_t *cand = slot_cand(s);
   s.mc->stored = true;
   if (held_near_forgotten(b, s)) {
      record_candidate(b, s, "deferred", MC_REASON_NEAR_FORGOTTEN);
      return;
   }
   if (reinforce(b, b->s[s.mc->merge_into.i].id, cand->told, cand->text, op->reason, NULL) !=
       SUCCESS) {
      s.mc->stored = false;
      store_slot(b, s, "the stored fact changed since", true);
   }
}

static void apply_suggest(mc_batch_t *b, const mc_op_t *op) {
   memory_fact_t buf;
   memory_consolidation_op_t row = {
      .op = "suggested_forget",
      .fact_id = b->s[op->a.i].id,
      .outside = b->outside,
      .text = fact_text(b->in->user_id, b->s[op->a.i].id, &buf),
      .reason = op->reason,
   };
   record(b, &row);
   b->res->suggested_forget++;
}

/* A refused op (or a destructive one a partial run leaves): recorded refused;
 * what was to absorb or replace the stored item is stored beside it, and a
 * candidate refused for being near a forgotten fact is set aside. */
static void apply_refused(mc_batch_t *b, const mc_op_t *op, const char *why) {
   record_refused(b, op, why);
   for (int k = 0; k < 2; k++) {
      const mc_ref_t r = k == 0 ? op->a : op->b;
      if (!is_cand(r) || (k == 0 && op->kind != MC_OP_ADD)) {
         continue;
      }
      const mc_slot_t s = slot_of(b, r);
      const mc_disp_t disp = s.mc ? s.mc->disp : s.d->disp;
      if (disp == MC_DISP_ADD) {
         store_slot(b, s, "kept beside the stored fact", false);
      } else if (disp == MC_DISP_DEFER && !(s.mc ? s.mc->stored : s.d->done)) {
         *(s.mc ? &s.mc->stored : &s.d->done) = true;
         record_candidate(b, s, "deferred", op->why);
      }
   }
}

static void apply_op(mc_batch_t *b,
                     const mc_op_t *op,
                     memory_consolidate_stop_t stop,
                     bool partial) {
   if (op->status == MC_REFUSED) {
      apply_refused(b, op, op->why);
      return;
   }
   if (partial && op->destructive) {
      char why[96];
      snprintf(why, sizeof(why), "not applied: the run ended %s",
               stop == MEMORY_CONSOLIDATE_FINISHED ? "over the loss cap"
                                                   : memory_consolidate_stop_name(stop));
      apply_refused(b, op, why);
      return;
   }
   switch (op->kind) {
      case MC_OP_ADD:
         store_slot(b, slot_of(b, op->a), op->reason, false);
         break;
      case MC_OP_MERGE:
         if (is_cand(op->a)) {
            apply_copy(b, op);
         } else {
            apply_retire(b, op, true);
         }
         break;
      case MC_OP_REPLACE:
         apply_retire(b, op, false);
         break;
      case MC_OP_CLOSE:
         apply_close(b, op);
         break;
      case MC_OP_LINK:
         apply_link(b, op);
         break;
      case MC_OP_DEFER:
         if (op->a.kind == MC_REF_C) {
            record_candidate(b, slot_of(b, op->a), "deferred", op->reason);
         }
         break;
      case MC_OP_DROP:
         record_candidate(b, slot_of(b, op->a), "dropped", op->reason);
         break;
      case MC_OP_FORGET:
         apply_suggest(b, op);
         break;
   }
}

/* What triage decided alone: reinforced, dropped as forgotten, or added. */
static void apply_triage(mc_batch_t *b) {
   for (int i = 0; i < b->n_c; i++) {
      mc_cand_t *mc = &b->c[i];
      const mc_slot_t s = slot_of_cand(b, i);
      if (mc->triage == MC_TRI_REINFORCE) {
         /* Through the store, so its subject, expiry and relations are kept
          * as the prompt path keeps them; retired meanwhile, it is stored. */
         store_slot(b, s, MC_REASON_TRIAGE_SAME, false);
      } else if (mc->triage == MC_TRI_FORGOTTEN) {
         mc->stored = true;
         record_forgotten(b, s, MC_REASON_TRIAGE_FORGOTTEN);
      } else if (mc->triage == MC_TRI_NEW) {
         store_slot(b, s, MC_REASON_TRIAGE_NEW, false);
      }
   }
}

/* Every C ends with one disposition: one the plan never reached is stored
 * (after a check for a paraphrase stored), unless it is near something the
 * user forgot (then set aside); one named only as a counterpart is stored. */
static void apply_undisposed(mc_batch_t *b) {
   for (int i = 0; i < b->n_shown; i++) {
      mc_cand_t *mc = &b->c[b->shown[i]];
      const mc_slot_t s = slot_of_cand(b, b->shown[i]);
      if (mc->stored) {
         continue;
      }
      if (mc->disp == MC_DISP_NONE) {
         if (mc->near_forgotten) {
            mc->stored = true;
            record_candidate(b, s, "deferred", MC_REASON_NOT_REACHED);
         } else {
            store_slot(b, s, MC_REASON_NOT_REACHED, true);
         }
      } else if (mc->disp == MC_DISP_ADD) {
         store_slot(b, s, NULL, false);
      }
   }
}

/* The candidates recorded without a decision (about the assistant, the same
 * text as another candidate, or past the batch's limit). */
static void apply_unshown(mc_batch_t *b) {
   for (int i = 0; i < b->n_unshown; i++) {
      const mc_unshown_t *u = &b->unshown[i];
      /* Words the user forgot are recorded as a drop, without them. */
      const bool forgotten = memory_forgotten_names(b->in->forgotten, u->text);
      memory_consolidation_op_t op = {
         .op = forgotten ? "dropped" : u->op,
         .outside = b->outside,
         .text = forgotten ? NULL : u->text,
         .reason = forgotten ? MC_REASON_BACKSTOP_FORGOTTEN : u->reason,
         .pre_image = !forgotten && u->json
                          ? json_object_to_json_string_ext(u->json, JSON_C_TO_STRING_PLAIN)
                          : NULL
      };
      record(b, &op);
      if (strcmp(op.op, "deferred") == 0) {
         b->res->deferred++;
      } else {
         b->res->dropped++;
      }
   }
}

/* The batch's preferences, recorded with what they were. */
static void apply_prefs(mc_batch_t *b) {
   const memory_consolidate_input_t *in = b->in;
   struct json_object *prefs = NULL;
   if (!json_object_object_get_ex(b->roots[0], "preferences", &prefs) ||
       !json_object_is_type(prefs, json_type_array)) {
      return;
   }
   for (size_t i = 0; i < json_object_array_length(prefs); i++) {
      struct json_object *p = json_object_array_get_idx(prefs, i);
      const char *category = str_of(p, "category");
      const char *value = str_of(p, "value");
      if (!category || !value) {
         continue;
      }
      struct json_object *conf_obj = NULL;
      const float conf = json_object_object_get_ex(p, "confidence", &conf_obj)
                             ? (float)json_object_get_double(conf_obj)
                             : 0.7f;
      memory_preference_t before;
      const bool had = memory_db_pref_get(in->user_id, category, &before) == MEMORY_DB_SUCCESS;
      memory_db_pref_upsert(in->user_id, category, value, conf, "inferred", in->prov);
      memory_preference_t after;
      if (memory_db_pref_get(in->user_id, category, &after) != MEMORY_DB_SUCCESS ||
          (had && strcmp(after.value, before.value) == 0 &&
           after.confidence == before.confidence)) {
         continue; /* nothing changed (a value outside text may not replace) */
      }
      struct json_object *pre = json_object_new_object();
      if (had) {
         json_object_object_add(pre, "value", json_object_new_string(before.value));
         json_object_object_add(pre, "confidence", json_object_new_double(before.confidence));
      } else {
         json_object_object_add(pre, "absent", json_object_new_boolean(1));
      }
      struct json_object *post = json_object_new_object();
      json_object_object_add(post, "value", json_object_new_string(after.value));
      json_object_object_add(post, "confidence", json_object_new_double(after.confidence));
      char *pre_s = image(pre);
      char *post_s = image(post);
      char text[MEMORY_FACT_TEXT_MAX];
      snprintf(text, sizeof(text), "%s: %s", category, after.value);
      memory_consolidation_op_t row = { .op = "pref_set",
                                        .outside = b->outside,
                                        .text = text,
                                        .pre_image = pre_s,
                                        .post_image = post_s };
      record(b, &row);
      free(pre_s);
      free(post_s);
   }
}

int mc_apply(mc_batch_t *b, memory_consolidate_stop_t stop, bool partial) {
   const memory_consolidate_input_t *in = b->in;
   const memory_consolidation_batch_t row = {
      .user_id = in->user_id,
      .conversation_id = in->conversation_id,
      .msg_id_start = in->first_msg_id,
      .msg_id_end = in->prov ? in->prov->msg_id_end : 0,
      .path = partial ? "agent_partial" : "agent",
      .stop = memory_consolidate_stop_name(stop),
      .outside = b->outside,
      .rounds = b->res->rounds,
      .input_tokens = b->res->input_tokens,
   };
   if (memory_db_consolidation_batch_add(&row, &b->batch_id) != MEMORY_DB_SUCCESS) {
      b->batch_id = 0; /* stored all the same; the record is missing */
   }
   b->unfinished = stop != MEMORY_CONSOLIDATE_FINISHED;
   apply_unshown(b);
   apply_triage(b);
   /* What can't lose a stored item first, then what retires one (all of it
    * refused when the run didn't finish, or past the loss cap). */
   for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < b->n_ops; i++) {
         const mc_op_t *op = &b->ops[i];
         if (op->status != MC_REPLACED && op->status != MC_REJECTED &&
             (pass == 1) == op->destructive) {
            apply_op(b, op, stop, partial);
         }
      }
   }
   apply_undisposed(b);
   apply_prefs(b);
   return SUCCESS;
}
