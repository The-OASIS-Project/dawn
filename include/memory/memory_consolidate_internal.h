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
 * What memory_consolidate.c (the rounds, what the model is shown) and
 * memory_consolidate_ops.c (the plan: read, checked, applied) share: one
 * batch's state.  Nothing else includes it.
 */

#ifndef MEMORY_CONSOLIDATE_INTERNAL_H
#define MEMORY_CONSOLIDATE_INTERNAL_H

#ifndef MEMORY_CONSOLIDATE_INTERNAL_ALLOWED
#error "memory_consolidate_internal.h is private to the consolidator"
#endif

#include <stdbool.h>
#include <stdint.h>

#include "core/strbuf.h"
#include "memory/memory_candidates.h"
#include "memory/memory_consolidate.h"
#include "memory/memory_db_consolidation.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/* How much one batch shows and plans. */
#define MC_MAX_CANDIDATES 64 /* candidates kept from the samples */
#define MC_MAX_STORED 40     /* S: stored facts shown, all candidates together */
#define MC_STORED_PER_C 3    /* S per candidate */
#define MC_MAX_RELATIONS 24  /* R */
#define MC_MAX_ENTITIES 24   /* E */
#define MC_MAX_FORGOTTEN 40  /* F */
#define MC_MAX_DEFERRED 10   /* D */
#define MC_MAX_OPS 160

/* Reasons code writes on a candidate's row (the model writes its own). */
#define MC_REASON_TRIAGE_NEW "nothing similar stored"
#define MC_REASON_TRIAGE_SAME "the same text is stored"
#define MC_REASON_TRIAGE_FORGOTTEN "the same text was forgotten"
#define MC_REASON_BACKSTOP_FORGOTTEN "its text was forgotten"
#define MC_REASON_NOT_REACHED "not reached"
#define MC_REASON_ASSISTANT "about the assistant"
#define MC_REASON_NOT_STORED "not stored"
#define MC_REASON_SAME_CANDIDATE "the same text as another candidate"
#define MC_REASON_NO_ROUND "no round judged it"
#define MC_REASON_NEAR_FORGOTTEN "near something the user forgot"

/* A loss cap: destructive ops on stored items over max(MC_LOSS_FLOOR,
 * MC_LOSS_SHARE of the stored facts and relations shown) apply nothing
 * destructive. */
#define MC_LOSS_FLOOR 2
#define MC_LOSS_SHARE 0.25

/* Cosine at or above which two candidates of a batch may say the same, so
 * neither is stored by triage alone: bge-small's 5th percentile for a fact
 * and its re-extraction (MEMORY_FORGOTTEN_CANDIDATE's measure). */
#define MC_SIBLING_COSINE MEMORY_FORGOTTEN_CANDIDATE

/* An id the model may name. */
typedef enum {
   MC_REF_NONE = 0,
   MC_REF_C,
   MC_REF_S,
   MC_REF_R,
   MC_REF_E,
   MC_REF_D
} mc_ref_kind_t;

typedef struct {
   mc_ref_kind_t kind;
   int i; /* 0-based index into the batch's list of that kind */
} mc_ref_t;

/* How triage left a candidate. */
typedef enum {
   MC_TRI_MODEL = 0, /* the model decides */
   MC_TRI_REINFORCE, /* the same text is stored: reinforced */
   MC_TRI_FORGOTTEN, /* the same text was forgotten: dropped */
   MC_TRI_NEW,       /* nothing like it stored: added */
} mc_triage_t;

/* What a candidate (or a deferred one) ends as. */
typedef enum {
   MC_DISP_NONE = 0,
   MC_DISP_ADD,     /* stored: add, or the into/by of a merge or replace */
   MC_DISP_MERGE_S, /* says what a stored fact says: that one reinforced */
   MC_DISP_MERGE_C, /* says what another candidate says: dropped as its copy */
   MC_DISP_DEFER,
   MC_DISP_DROP,
} mc_disp_t;

typedef struct {
   int cand; /* index into the batch's candidates */
   mc_triage_t triage;
   int64_t exact_id;    /* MC_TRI_REINFORCE: the stored fact */
   bool near_forgotten; /* within MEMORY_FORGOTTEN_CANDIDATE of an F */
   mc_disp_t disp;
   int disp_op;                     /* the op that gave it (-1: code) */
   mc_ref_t merge_into;             /* MC_DISP_MERGE_S / _C: which */
   char text[MEMORY_FACT_TEXT_MAX]; /* a shortened text (deletion only) */
   bool shortened;
   int64_t fact_id; /* stored as */
   bool stored;     /* stored, or dropped as forgotten (once) */
   float *vec;      /* its embedding (NULL: none) */
   int dims;
   float norm;
} mc_cand_t;

/* A candidate recorded without being decided: its subject is the assistant,
 * it repeats another candidate's text, or the batch had more than
 * MC_MAX_CANDIDATES. */
typedef struct {
   const char *text;
   struct json_object *json;
   const char *reason;
   const char *op; /* "dropped" or "deferred" */
} mc_unshown_t;

typedef struct {
   memory_relation_t rel;
   char subject[MEMORY_ENTITY_NAME_MAX];
} mc_rel_t;

typedef struct {
   memory_consolidation_deferred_t row;
   struct json_object *root; /* its candidate JSON, parsed */
   memory_candidates_t c;    /* one candidate */
   int outside;              /* its taint and the batch's */
   memory_provenance_t prov; /* its own batch's conversation and range, that taint */
   bool near_forgotten;      /* within MEMORY_FORGOTTEN_CANDIDATE of an F */
   mc_disp_t disp;
   int disp_op;
   int64_t fact_id;
   bool done; /* stored, or dropped as forgotten */
} mc_deferred_t;

typedef enum {
   MC_OP_ADD = 0,
   MC_OP_MERGE,
   MC_OP_REPLACE,
   MC_OP_CLOSE,
   MC_OP_LINK,
   MC_OP_DEFER,
   MC_OP_DROP,
   MC_OP_FORGET,
} mc_op_kind_t;

typedef enum {
   MC_OK = 0,
   MC_REJECTED, /* invalid: named in the repair round */
   MC_REFUSED,  /* a guard's refusal: final, recorded */
   MC_REPLACED, /* rejected, then the repair round answered for it */
} mc_status_t;

typedef struct {
   mc_op_kind_t kind;
   mc_ref_t a; /* add/defer/drop: c; merge: c or from; replace: old; close: r; link: e; forget: s */
   mc_ref_t b; /* merge: into; replace: by; close: by; link: same_as */
   int64_t valid_to;
   char text[MEMORY_FACT_TEXT_MAX];
   bool has_text;
   char reason[MEMORY_CONSOLIDATE_REASON_MAX + 1];
   mc_status_t status;
   char why[160];
   bool destructive; /* retires or links a stored item */
} mc_op_t;

typedef struct {
   const memory_consolidate_input_t *in;
   memory_store_t *st;
   int outside;     /* the batch's */
   bool tainted;    /* outside_is_tainted(outside) */
   bool unfinished; /* the plan never finished: nothing near a forgotten fact is stored */
   char ai_name[MEMORY_ENTITY_NAME_MAX];

   struct json_object *roots[2];
   int n_roots;
   memory_candidates_t cands;
   mc_cand_t c[MC_MAX_CANDIDATES];
   int n_c;
   int shown[MC_MAX_CANDIDATES]; /* C number - 1 → index into c */
   int n_shown;

   memory_fact_t s[MC_MAX_STORED];
   int n_s;
   mc_rel_t r[MC_MAX_RELATIONS];
   int n_r;
   memory_entity_names_t e[MC_MAX_ENTITIES];
   int n_e;
   int f[MC_MAX_FORGOTTEN]; /* indexes into the forgotten list */
   int n_f;
   mc_deferred_t d[MC_MAX_DEFERRED];
   int n_d;

   mc_op_t ops[MC_MAX_OPS];
   int n_ops;
   memory_consolidate_result_t *res;
   int64_t batch_id;
   int seq;
   mc_unshown_t *unshown; /* one per candidate at most (grown as needed) */
   int n_unshown;
   int cap_unshown;
   int n_pairs; /* entity pairs a link could name */
   /* Each new entity with the best entity it may be (E indexes): what the
    * extraction gate would decide when no round does. */
   int best_pair[MC_MAX_ENTITIES][2];
   int n_best;
} mc_batch_t;

/* memory_consolidate_ops.c */

/** Read a reply's plan into b->ops, each checked (memory_consolidate_ops.c);
 *  an op the plan already holds is left out.  @p n_read: ops the reply held.
 *  FAILURE: not the JSON asked for. */
int mc_plan_read(mc_batch_t *b, const char *reply, int *n_read);

/** Whether every C has a disposition and no op is rejected. */
bool mc_plan_complete(const mc_batch_t *b);

/** The repair round's list: each rejected op and each C with no disposition. */
void mc_plan_problems(const mc_batch_t *b, strbuf_t *out);

/** Rejected ops stand down for the repair round's answers. */
void mc_plan_retire_rejected(mc_batch_t *b);

/** Whether the plan's destructive ops pass the loss cap. */
bool mc_plan_within_loss_cap(const mc_batch_t *b);

/**
 * @brief Store and record the batch: triage's decisions and the plan's ops;
 *        @p partial applies only what can't lose a stored item
 */
int mc_apply(mc_batch_t *b, memory_consolidate_stop_t stop, bool partial);

/** No round ran: each new entity's best pair through the link ladder (from
 *  outside text, a proposal), recorded like a link the plan asked for. */
void mc_link_unjudged(mc_batch_t *b);

/** "C3", "S1", ... for @p ref into @p out. */
void mc_ref_name(mc_ref_t ref, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CONSOLIDATE_INTERNAL_H */
