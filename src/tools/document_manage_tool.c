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
 * LLM document-management tool (v61): the LLM-facing document ingestion +
 * management API.  Lets the assistant SAVE authored reference text as a note
 * (single-chunk, filed under a label, retrievable exactly) or as a general
 * (chunked) document, LIST what it has stored, and DELETE — with a mandatory
 * two-step user-approval flow for deletion (stage on 'delete', execute only on
 * 'confirm_delete'), mirroring the email trash confirmation pattern.
 *
 * Retrieval is NOT here: use document_search (hybrid) / document_read (verbatim
 * by label).  is_global is always false — the LLM cannot publish to all users.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <time.h>

#include "auth/auth_db_withdraw.h"
#include "core/embedding_engine.h"
#include "core/pending_slots.h"
#include "core/session_prefix.h"
#include "core/strbuf.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "llm/llm_tools.h" /* LLM_TOOLS_ARGS_LEN — the upstream tool-arg cap DOCMGMT_SAVE_TEXT_MAX mirrors */
#include "logging.h"
#include "memory/memory_note_bridge.h"
#include "tools/document_db.h"
#include "tools/document_index_pipeline.h"
#include "tools/document_manage.h"
#include "tools/toml.h"
#include "tools/tool_pending.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Config — TOOL_CAP_DANGEROUS tools must supply a config struct + parser, and
 * the registry reads the first `bool enabled` field to honor an operator on/off
 * in the [document_manage] TOML section.  Defaults to ENABLED (the destructive
 * action — delete — is already gated behind a two-step user confirmation), so
 * the tool works out of the box; set `[document_manage] enabled = false` to
 * remove the LLM's document write/delete access.
 * ============================================================================= */

typedef struct {
   bool enabled; /**< MUST be the first field (dangerous-tool convention) */
} document_manage_config_t;

static document_manage_config_t s_config = { .enabled = true };

static void doc_manage_parse_config(toml_table_t *table, void *config) {
   document_manage_config_t *cfg = (document_manage_config_t *)config;
   if (!table)
      return; /* no [document_manage] section — keep the enabled-by-default */
   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok)
      cfg->enabled = enabled.u.b;
}

/* =============================================================================
 * Two-step delete approval — a staged deletion per session, named by its id
 * ============================================================================= */

#define DOCMGMT_MAX_PENDING 16
#define DOCMGMT_PENDING_EXPIRY_SEC 300 /* as long as a reply code, when one confirms it */
/* Upper bound on the save_text overwrite sweep — how many same-named duplicate
 * documents we'll delete before re-indexing.  A backstop against a delete that
 * keeps reporting success without removing the row; in practice 1-2. */
#define DOCMGMT_MAX_OVERWRITE_SWEEP 64
#define DOCMGMT_CONFIRM_MSG_MAX 768 /* prompt text + up to DOC_FILENAME_MAX label */

/* Max text accepted by save_note/save_text.  save_text stores MULTI-chunk
 * documents, so the per-chunk DOC_CHUNK_TEXT_MAX (4096) would silently clip
 * anything bigger than one chunk.  Size this to the upstream tool-arg buffer
 * (LLM_TOOLS_ARGS_LEN) — the practical ceiling on what can arrive in a tool
 * call; over-long args are already rejected at the executor (args_truncated) so
 * the value reaching here never exceeds this.  The _Static_assert below pins the
 * relationship so a future LLM_TOOLS_ARGS_LEN bump can't silently re-clip. */
#define DOCMGMT_SAVE_TEXT_MAX 16384
_Static_assert(DOCMGMT_SAVE_TEXT_MAX >= LLM_TOOLS_ARGS_LEN,
               "save_text buffer must hold the full tool-arg value or docs clip again");

/* Advertised per-call text budget — ADVISORY, and deliberately NOT enforced.
 *
 * The buffer above must stay >= LLM_TOOLS_ARGS_LEN so a value that survived the
 * transport is never re-clipped here (that is what the assert guards).  This is
 * the different question the model needs answered: how much text can it send
 * before the *transport* rejects the call?  The arg buffer holds the whole JSON
 * object — action, label, quoting, and every newline and quote escaped to two
 * bytes — so the usable text is meaningfully less than LLM_TOOLS_ARGS_LEN, and
 * discovering that by being rejected is ruinously expensive: the model streams
 * the entire document first, so an over-long note costs a full generation that
 * is then thrown away.  Observed live: three attempts, ~20k output tokens
 * discarded, 92% of a 4-minute job spent re-writing the same report.
 *
 * 12 KB measured against real assistant output (escapes at 1.02-1.06x) and
 * synthetic quote/newline-dense markdown (1.125x, the worst case found): 12288 x
 * 1.125 + envelope still leaves ~2.4 KB of headroom.  Not enforced because a
 * call that DID fit should never be refused for exceeding a guideline. */
#define DOCMGMT_SAVE_TEXT_BUDGET 12288
_Static_assert(DOCMGMT_SAVE_TEXT_BUDGET < LLM_TOOLS_ARGS_LEN,
               "the advertised budget must leave room for the JSON envelope + escaping");

/* Put the budget in the tool description as a NUMBER — "keep it short" is not
 * something a model can act on precisely, and a stale hand-typed constant here
 * would send it straight back into the discard loop. */
#define DOCMGMT_STR_HELPER(x) #x
#define DOCMGMT_STR(x) DOCMGMT_STR_HELPER(x)

/* Length is deliberately NOT capped at LLM_TOOLS_DESC_LEN.  The 512-byte buffers
 * in tool_param_t / tool_definition_t are the CACHED copies — used for the WebUI
 * tools panel and the legacy non-registry fallback — while the schema the model
 * actually receives is built live from the registry, where descriptions are
 * `const char *` straight to the literal (build_parameters_schema_from_treg, and
 * tool_effective_description for the tool-level text; the comment there records
 * that scheduler's ~5.8 KB description is what drove it).  So write what the
 * model needs and let it run long. */
#define DOCMGMT_TEXT_PARAM_DESC                                                                      \
   "The text to store. Required by save_note and save_text (the full verbatim content the user "     \
   "wants kept) and by append (the text to add to the end of the note). Must be the LAST "           \
   "argument for those actions. Not used by edit / list / delete / confirm_delete. "                 \
   "Keep each call under about " DOCMGMT_STR(                                                        \
       DOCMGMT_SAVE_TEXT_BUDGET) " bytes: that limit "                                               \
                                 "covers the whole argument object, including the JSON escaping "    \
                                 "of every newline and quote, "                                      \
                                 "and a call over it is rejected BEFORE it runs, so the entire "     \
                                 "write is wasted. To store more, "                                  \
                                 "send the first part with save_text, then add each following "      \
                                 "part with append to the SAME "                                     \
                                 "label. Do not create 'Part 1'/'Part 2' documents — that splits " \
                                 "one piece of writing across "                                      \
                                 "separate records."

/* A deletion awaiting its confirm, one per session (core/pending_slots.h):
 * the confirm must come from that session, in the user's next turn. */
#define DOCMGMT_PENDING_DELETE 1

typedef struct {
   pending_slot_t hdr;
   int64_t doc_id;
   char label[DOC_FILENAME_MAX];
   bool is_note;
} docmgmt_pending_t;

PENDING_ITEM_CHECK(docmgmt_pending_t);
static docmgmt_pending_t s_pending[DOCMGMT_MAX_PENDING];
static pthread_mutex_t s_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
PENDING_ARRAY_CHECK(s_pending);
static const pending_slots_t s_pending_slots = PENDING_SLOTS_TABLE(s_pending,
                                                                   DOCMGMT_PENDING_EXPIRY_SEC);

/* Serializes edit/append read→modify→write so two concurrent edits of the same
 * note can't clobber each other (the tool worker + a WebUI editor are different
 * threads).  Coarse but correct; edits are infrequent.  Tool-vs-WebUI-direct
 * races on the same note remain documented-acceptable for single-user until the
 * B1b CAS-by-hash upgrade. */
static pthread_mutex_t s_edit_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Stage (replace) this session's pending deletion.  Its pending_id, or 0
 * when it wasn't staged (@p rc says why). */
static uint32_t stage_pending(const turn_origin_t *origin,
                              int user_id,
                              int64_t doc_id,
                              const char *label,
                              bool is_note,
                              pending_stage_rc_t *rc) {
   pthread_mutex_lock(&s_pending_mutex);
   docmgmt_pending_t *p = (docmgmt_pending_t *)pending_slots_stage(&s_pending_slots, origin,
                                                                   user_id, DOCMGMT_PENDING_DELETE,
                                                                   pending_slots_now(), rc);
   const uint32_t pending_id = p ? p->hdr.item_id : 0;
   if (p) {
      p->doc_id = doc_id;
      snprintf(p->label, sizeof(p->label), "%s", label ? label : "");
      p->is_note = is_note;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   return pending_id;
}

/* Take this session's pending deletion, the one the confirm names, for its
 * confirm: copied to @p out and consumed.  NULL on success, else the message
 * to return (one named wrong, or confirmed in the wrong turn, stays). */
static char *take_pending(const turn_origin_t *origin,
                          int user_id,
                          uint32_t pending_id,
                          docmgmt_pending_t *out) {
   if (pending_id == 0)
      return tool_pending_missing_id("deletion");
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   pthread_mutex_lock(&s_pending_mutex);
   const pending_find_rc_t rc = pending_slots_take(&s_pending_slots, origin, user_id,
                                                   DOCMGMT_PENDING_DELETE, pending_id,
                                                   pending_slots_now(), out, sizeof(*out), &orc);
   pthread_mutex_unlock(&s_pending_mutex);
   if (rc == PENDING_FOUND)
      return NULL;
   if (rc == PENDING_NOT_NOW)
      OLOG_WARNING("document_manage: confirm_delete refused (%s)", turn_origin_refusal(orc));
   return tool_pending_take_refusal(rc, orc, "deletion");
}

static int resolve_owned_doc(int user_id, const char *label, int64_t id, document_t *out);

/* The note or document a write acts on, named as execution resolves it (an
 * id first, else the exact label): "the note 'X' (#42)", and whether a save
 * replaces one.  Refused (with why in @p out) when nothing resolves. */
static int describe_write_target(int user_id,
                                 const char *action,
                                 const char *label,
                                 int64_t id,
                                 char *out,
                                 size_t out_len) {
   int n;
   if (strcmp(action, "save_note") == 0 || strcmp(action, "save_text") == 0) {
      if (!label[0]) {
         snprintf(out, out_len, "it doesn't name the note or document to save");
         return FAILURE;
      }
      /* As do_save_note / do_save_text decide: a save over the user's own
       * item of that name (a note; a document that isn't one) replaces it. */
      const bool note = strcmp(action, "save_note") == 0;
      document_t existing;
      const bool overwrite = document_db_find_by_label_exact(user_id, label, note, &existing) ==
                                 SUCCESS &&
                             existing.user_id == user_id &&
                             (note || strcmp(existing.filetype, "note") != 0);
      n = snprintf(out, out_len, "%s %s '%s'", overwrite ? "overwrite the" : "save a new",
                   note ? "note" : "document", label);
      return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
   }
   /* The item it acts on, shown by its own name: an id wins over a label, as
    * in execution. */
   document_t doc;
   if (resolve_owned_doc(user_id, label, id, &doc) == SUCCESS) {
      const bool is_note = strcmp(doc.filetype, "note") == 0 && doc.num_chunks == 1;
      /* recover on a live item undoes its last change (do_recover). */
      n = snprintf(out, out_len, "%s the %s '%s' (#%lld)",
                   strcmp(action, "recover") == 0 ? "undo the last change to" : action,
                   is_note ? "note" : "document", doc.filename, (long long)doc.id);
   } else if (strcmp(action, "recover") == 0 && label[0]) {
      n = snprintf(out, out_len, "recover the deleted note or document '%s'", label);
   } else {
      snprintf(out, out_len, "no note or document of the user's matches it");
      return FAILURE;
   }
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}

/* What a call that waits for the user's reply code does (tool_metadata_t
 * describe_call): a confirm_delete from the deletion it carries out; a write
 * with the item it acts on and what it writes (its text, the change, the new
 * name). */
static int doc_manage_describe_call(const char *action,
                                    const char *value,
                                    char *out,
                                    size_t out_len,
                                    int *valid_for_sec) {
   const int user_id = tool_get_current_user_id();
   int n = -1;
   if (strcmp(action, "confirm_delete") == 0) {
      char id_str[16] = "";
      long long id = 0;
      if (tool_param_extract_custom(value, "pending_id", id_str, sizeof(id_str)) && id_str[0]) {
         id = strtoll(id_str, NULL, 10);
      }
      turn_origin_t origin;
      if (id <= 0 || id > UINT32_MAX || !turn_origin_capture(&origin)) {
         return FAILURE;
      }
      pthread_mutex_lock(&s_pending_mutex);
      pending_slot_t *slot = NULL;
      if (pending_slots_find(&s_pending_slots, &origin, user_id, DOCMGMT_PENDING_DELETE,
                             (uint32_t)id, pending_slots_now(), &slot) == PENDING_FOUND) {
         const docmgmt_pending_t *p = (const docmgmt_pending_t *)slot;
         n = snprintf(out, out_len, "delete the %s '%s' (#%lld)", p->is_note ? "note" : "document",
                      p->label, (long long)p->doc_id);
         *valid_for_sec = pending_slots_valid_for(&s_pending_slots, slot, pending_slots_now());
      }
      pthread_mutex_unlock(&s_pending_mutex);
   } else {
      char label[DOC_FILENAME_MAX] = "";
      tool_param_extract_base(value, label, sizeof(label));
      char id_str[24] = "";
      int64_t id = 0;
      if (tool_param_extract_custom(value, "id", id_str, sizeof(id_str)) && id_str[0]) {
         id = (int64_t)strtoll(id_str, NULL, 10);
      }
      char target[DOC_FILENAME_MAX + 128];
      if (describe_write_target(user_id, action, label, id, target, sizeof(target)) != SUCCESS) {
         snprintf(out, out_len, "%s", target);
         return FAILURE;
      }
      /* What it writes: the one field its action reads (a decoy in another
       * field never shows). */
      const char *field_name = strcmp(action, "edit") == 0      ? "change"
                               : strcmp(action, "rename") == 0  ? "new_name"
                               : strcmp(action, "recover") == 0 ? NULL
                                                                : "text";
      char what[320] = "";
      if (field_name) {
         char *field = malloc(LLM_TOOLS_ARGS_LEN);
         if (!field) {
            return FAILURE;
         }
         if (tool_param_extract_custom(value, field_name, field, LLM_TOOLS_ARGS_LEN) && field[0]) {
            str_excerpt_line(field, 200, what, sizeof(what));
         }
         free(field);
      }
      n = snprintf(out, out_len, "%s%s%s%s", target, what[0] ? ": \"" : "", what,
                   what[0] ? "\"" : "");
   }
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}

/* =============================================================================
 * Tool metadata
 * ============================================================================= */

static char *doc_manage_callback(const char *action, char *value, int *should_respond);
static bool doc_manage_is_available(void);

static const treg_param_t doc_manage_params[] = {
   {
       .name = "action",
       .description =
           "Document management action: 'save_note' (file a short piece of authored reference "
           "text — a bio, pitch, address, canned answer — under a LABEL so it can be retrieved "
           "EXACTLY later; re-saving the same label OVERWRITES it; if it's too long for one note, "
           "use save_text instead), 'save_text' (store a LONGER or multi-paragraph piece of text "
           "as a normal searchable document under a title — use this for anything that won't fit "
           "as a single note), 'edit' (change part of "
           "an existing note OR document WITHOUT resending the whole thing — supply the exact text "
           "to find and "
           "what to replace it with), 'append' (add text to the end of an existing note or "
           "document), 'list' "
           "(show the documents and notes the user has stored), 'delete' (request deletion of a "
           "note or document by its label — this does NOT delete immediately; it asks the user to "
           "confirm), 'confirm_delete' (carry out the deletion the user just approved), "
           "'list_deleted' (show recently-deleted notes/documents that can still be recovered), "
           "'recover' (UNDO — restore the previous version of an existing note/document, or bring "
           "back a recently-deleted one, by its label), 'rename' (change the name/label of an "
           "existing note or document WITHOUT changing its content — give the current name (or id) "
           "and the 'new_name'). Prefer "
           "'edit'/'append' over re-saving when updating living text the user keeps current.",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "save_note", "save_text", "edit", "append", "list", "delete",
                        "confirm_delete", "list_deleted", "recover", "rename" },
       .enum_count = 10,
   },
   {
       .name = "label",
       .description = "For save_note: the label/name to file the text under (e.g. 'Public Bio'). "
                      "For save_text: the document title. For edit / append / delete / rename / "
                      "recover: the exact current label/name of the note or document to act on "
                      "(or give its 'id' instead). Not used by list / confirm_delete.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "id",
       .description = "Optional numeric document id (from a 'list' result) — an alternative to "
                      "'label' for identifying the target of edit / append / delete / rename / "
                      "recover. Prefer it when a name is ambiguous or the user gave you an id.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "id",
   },
   {
       .name = "pending_id",
       .description = "Required by 'confirm_delete': the pending_id the 'delete' preview "
                      "returned. Only that deletion is carried out.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "pending_id",
   },
   {
       .name = "new_name",
       .description = "Required by 'rename': the new name/label to give the note or document. "
                      "The content is unchanged — only the name changes.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "new_name",
   },
   {
       .name = "text",
       .description = DOCMGMT_TEXT_PARAM_DESC,
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "text",
   },
   {
       .name = "change",
       .description =
           "Required by 'edit'. A JSON object given as a string: "
           "{\"find\": \"<exact text already in the note>\", \"replace\": \"<new text>\"}. "
           "The 'find' text must appear in the note EXACTLY ONCE — read the note first "
           "(document_read) and copy the snippet verbatim. If it could match more than one place, "
           "include more surrounding text to make it unique. Must be the LAST argument for 'edit'.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "change",
   },
};

static const tool_action_kind_entry_t s_doc_manage_action_kinds[] = {
   { "list", TOOL_KIND_READ, NULL },
   { "list_deleted", TOOL_KIND_READ, NULL },
   { "delete", TOOL_KIND_PREPARE, "confirm_delete" },
   { "confirm_delete", TOOL_KIND_ACT, NULL },
};

static const tool_metadata_t doc_manage_metadata = {
   .name = "document_manage",
   .describe_call = doc_manage_describe_call,
   .action_kinds = s_doc_manage_action_kinds,
   .action_kind_count = TOOL_KIND_COUNT(s_doc_manage_action_kinds),
   .device_string = "document manager",
   .description =
       "Save, edit, list, and delete the user's stored documents and notes. Use 'save_note' to "
       "file exact reference text (a bio, an elevator pitch, an address, a saved answer) under a "
       "label the user can ask for later by name; 'save_text' for longer documents; 'edit' / "
       "'append' to update an existing note OR document in place WITHOUT resending the whole "
       "thing; 'list' to see what's stored; 'rename' to change a note/document's name without "
       "touching its content; and 'delete' to remove something (which always asks "
       "the user to confirm first). This is the home for verbatim, living text the user maintains "
       "and retrieves exactly — when such a value changes, EDIT it rather than storing the new "
       "value as a memory (memory is for facts learned in passing, not authored reference text). "
       "Edits, overwrites, and deletes are AUTOMATICALLY version-snapshotted, so changes are safe "
       "to make: a deleted note/document can be brought back with 'list_deleted' then 'recover', "
       "and earlier versions are retained for a short window (the user can also restore them from "
       "the WebUI). To READ or SEARCH stored content, use document_read / document_search "
       "instead.",
   .params = doc_manage_params,
   .param_count = TOOL_PARAM_COUNT(doc_manage_params),
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_DANGEROUS, /* mutates + deletes user data */
   .default_local = true,
   .default_remote = true,
   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = doc_manage_parse_config,
   .config_section = "document_manage",
   .callback = doc_manage_callback,
   .is_available = doc_manage_is_available,
};

static bool doc_manage_is_available(void) {
   return embedding_engine_available();
}

/* =============================================================================
 * Action handlers
 * ============================================================================= */

static char *do_save_note(int user_id, const char *label, const char *text) {
   if (!label || !label[0] || !text || !text[0])
      return strdup("To save a note, provide both a label and the text.");

   /* Exact-label overwrite routing (M-5): re-saving a label edits in place.
    * Only treat it as an overwrite when the existing note is the CALLER'S own —
    * find_by_label_exact also matches global notes, and editing one of those
    * isn't ours to do (the DB gate would reject it anyway). */
   document_t existing;
   bool overwrite = (document_db_find_by_label_exact(user_id, label, true, &existing) == SUCCESS &&
                     existing.user_id == user_id);

   doc_index_result_t res;
   int rc;
   if (overwrite) {
      rc = document_note_update(user_id, existing.id, label, text, strlen(text), &res);
   } else {
      rc = document_index_note(user_id, label, text, strlen(text), false, &res);
   }
   if (rc != DOC_INDEX_SUCCESS) {
      char msg[256];
      snprintf(msg, sizeof(msg), TOOL_RESULT_ERROR_MARK "Couldn't save the note: %s",
               res.error_msg);
      return strdup(msg);
   }
   /* Memory→note bridge (v61): refresh the gloss pointer so a fuzzy recall of
    * this note routes to document_read.  Best-effort — never fail the save. */
   if (res.doc_id > 0)
      (void)memory_note_bridge_upsert_gloss(user_id, res.doc_id, label);
   char msg[DOCMGMT_CONFIRM_MSG_MAX];
   snprintf(msg, sizeof(msg), "%s note '%s'.", overwrite ? "Updated (overwrote existing)" : "Saved",
            label);
   return strdup(msg);
}

static char *do_save_text(int user_id, const char *title, const char *text) {
   if (!title || !title[0] || !text || !text[0])
      return strdup("To save a document, provide both a title and the text.");

   document_t existing;
   bool overwrite = false;

   /* In-place overwrite of a previously-saved text document keeps its doc_id
    * stable, so the version history + undo (recover) survive the overwrite.
    * Delete+recreate would strand the prior content's version chain under the
    * dead id and leave a "Recently deleted" ghost that restores to a duplicate
    * (#3).  Only a "text" doc with stored full text (v63) qualifies; uploaded
    * files (other filetypes) keep the delete+recreate sweep below. */
   if (document_db_find_by_label_exact(user_id, title, false, &existing) == SUCCESS &&
       existing.user_id == user_id && strcmp(existing.filetype, "text") == 0) {
      char *probe = NULL;
      if (document_db_full_text_get(existing.id, user_id, &probe) == SUCCESS) {
         free(probe);
         doc_index_result_t res;
         int rc = document_doc_update(user_id, existing.id, text, strlen(text), &res);
         if (rc != DOC_INDEX_SUCCESS) {
            char msg[256];
            snprintf(msg, sizeof(msg), TOOL_RESULT_ERROR_MARK "Couldn't save the document: %s",
                     res.error_msg);
            return strdup(msg);
         }
         char msg[256];
         snprintf(msg, sizeof(msg), "Updated (overwrote existing) document '%s' (%d chunk%s).",
                  title, res.num_chunks, res.num_chunks == 1 ? "" : "s");
         return strdup(msg);
      }
   }

   /* Fallback overwrite-by-label: re-saving an existing document of the same
    * name replaces it instead of creating a duplicate (do_save_note does the
    * same for notes).  Delete EVERY same-named non-note document the CALLER owns
    * so repeated saves converge to one (also self-heals pre-existing
    * duplicates).  Scoped tight: a same-named note (different kind, owns a gloss)
    * and global/other-user docs are never touched.  The bound guards against a
    * delete that keeps failing. */
   /* The replaced document is the user removing it: its passages leave the
    * conversations they were sent into. */
   conv_db_withdraw_intent_begin(user_id);
   for (int guard = 0; guard < DOCMGMT_MAX_OVERWRITE_SWEEP; guard++) {
      if (document_db_find_by_label_exact(user_id, title, false, &existing) != SUCCESS ||
          existing.user_id != user_id || strcmp(existing.filetype, "note") == 0)
         break;
      if (document_db_delete_indexed(existing.id) != SUCCESS)
         break;
      overwrite = true;
   }
   conv_db_withdraw_intent_end();
   if (overwrite) {
      session_withdraw_forgotten_async(user_id, false);
   }

   doc_index_result_t res;
   int rc = document_index_text(user_id, title, "text", text, strlen(text), false, NULL, &res);
   if (rc != DOC_INDEX_SUCCESS) {
      char msg[256];
      snprintf(msg, sizeof(msg), TOOL_RESULT_ERROR_MARK "Couldn't save the document: %s",
               res.error_msg);
      return strdup(msg);
   }
   char msg[256];
   snprintf(msg, sizeof(msg), "%s document '%s' (%d chunk%s).",
            overwrite ? "Updated (overwrote existing)" : "Saved", title, res.num_chunks,
            res.num_chunks == 1 ? "" : "s");
   return strdup(msg);
}

/* Resolve a note/document the caller OWNS, by numeric id (preferred when id > 0)
 * or exact label.  Both document_db_get and find_by_label_exact can return GLOBAL
 * items (no ownership filter), so the explicit user_id check is what keeps a user
 * from acting on someone else's / an admin-published doc.  SUCCESS → *out filled. */
static int resolve_owned_doc(int user_id, const char *label, int64_t id, document_t *out) {
   if (id > 0)
      return (document_db_get(id, out) == SUCCESS && out->user_id == user_id) ? SUCCESS : FAILURE;
   if (label && label[0])
      return (document_db_find_by_label_exact(user_id, label, false, out) == SUCCESS &&
              out->user_id == user_id)
                 ? SUCCESS
                 : FAILURE;
   return FAILURE;
}

/* Resolve an editable note OR document the caller owns, by id (preferred) or exact
 * label.  Loads its current text into *text_out (heap, caller frees) and sets
 * *is_note:
 *   - single-chunk note → in-place note update (filetype "note", num_chunks 1);
 *   - else a multi-chunk document with stored full text (v63) → in-place replace.
 * FAILURE with *err for: not found / not owned / global / a multi-chunk doc that
 * predates full-text storage (must be re-saved). */
static int load_editable_text(int user_id,
                              const char *label,
                              int64_t id,
                              document_t *doc_out,
                              char **text_out,
                              bool *is_note,
                              const char **err) {
   *text_out = NULL;
   *is_note = false;
   if (id <= 0 && (!label || !label[0])) {
      *err = "Tell me which note or document (its label/name, or its id).";
      return FAILURE;
   }
   if (resolve_owned_doc(user_id, label, id, doc_out) != SUCCESS) {
      *err = TOOL_RESULT_ERROR_MARK
          "No editable note or document was found (it may be global or not yours).";
      return FAILURE;
   }

   *is_note = (strcmp(doc_out->filetype, "note") == 0 && doc_out->num_chunks == 1);
   if (*is_note) {
      document_chunk_t chunk;
      int n = 0;
      if (document_db_chunk_read(doc_out->id, &chunk, 1, 0, &n) != SUCCESS || n < 1) {
         *err = TOOL_RESULT_ERROR_MARK "Couldn't read the note's current text.";
         return FAILURE;
      }
      *text_out = strdup(chunk.text);
   } else {
      /* Multi-chunk document — editable only if its canonical full text is stored
       * (v63).  Pre-v63 uploads have none and must be re-saved first. */
      if (document_db_full_text_get(doc_out->id, user_id, text_out) != SUCCESS) {
         *err = TOOL_RESULT_ERROR_MARK
             "That document predates editable storage — re-save it (save_text) to enable "
             "editing.";
         return FAILURE;
      }
   }
   if (!*text_out) {
      *err = TOOL_RESULT_ERROR_MARK "Out of memory.";
      return FAILURE;
   }
   return SUCCESS;
}

/* Commit edited text: a single-chunk note goes through the stable-id note update;
 * a multi-chunk document is re-chunked + re-embedded in place.  Both keep doc_id
 * stable and archive the prior content.  Returns the user-facing message. */
static char *commit_edit(int user_id,
                         const document_t *doc,
                         bool is_note,
                         const char *new_text,
                         const char *ok_msg) {
   doc_index_result_t res;
   int rc = is_note ? document_note_update(user_id, doc->id, doc->filename, new_text,
                                           strlen(new_text), &res)
                    : document_doc_update(user_id, doc->id, new_text, strlen(new_text), &res);
   if (rc != DOC_INDEX_SUCCESS) {
      char msg[256];
      snprintf(msg, sizeof(msg), TOOL_RESULT_ERROR_MARK "Couldn't apply the edit: %s",
               res.error_msg);
      return strdup(msg);
   }
   return strdup(ok_msg);
}

/* edit: find/replace in a note.  `change` is a JSON object given as a string:
 * {"find": "...", "replace": "..."}; the JSON escaping is what makes
 * find/replace safe regardless of content. */
static char *do_edit(int user_id, const char *label, int64_t id, const char *change_json) {
   if (!change_json || !change_json[0])
      return strdup("To edit, provide 'change' as {\"find\": \"...\", \"replace\": \"...\"}.");

   struct json_object *change = json_tokener_parse(change_json);
   if (!change || !json_object_is_type(change, json_type_object)) {
      if (change)
         json_object_put(change);
      return strdup("'change' must be a JSON object: {\"find\": \"...\", \"replace\": \"...\"}.");
   }
   struct json_object *find_obj = NULL, *replace_obj = NULL;
   const char *find = json_object_object_get_ex(change, "find", &find_obj)
                          ? json_object_get_string(find_obj)
                          : NULL;
   const char *replace = json_object_object_get_ex(change, "replace", &replace_obj)
                             ? json_object_get_string(replace_obj)
                             : NULL;
   if (!find || !find[0] || !replace_obj) { /* replace may legitimately be "" (a deletion) */
      json_object_put(change);
      return strdup("'change' needs a non-empty 'find' and a 'replace' (use \"\" to delete).");
   }

   char *result = NULL;
   pthread_mutex_lock(&s_edit_mutex);

   document_t doc;
   char *old = NULL;
   bool is_note = false;
   const char *err = NULL;
   if (load_editable_text(user_id, label, id, &doc, &old, &is_note, &err) != SUCCESS) {
      result = strdup(err);
   } else {
      /* Unique-match contract (mirrors str_replace): 0 / >1 are errors that leave
       * the note/document untouched, never a guess. */
      char *new_text = NULL;
      int occ = docmgmt_find_replace_once(old, find, replace, &new_text);
      if (occ == 0) {
         result = strdup("That exact text isn't there — use document_read to get the current text "
                         "and copy the snippet verbatim.");
      } else if (occ >= 2) {
         result = strdup("That text appears more than once — include more surrounding text in "
                         "'find' to make it unique.");
      } else if (!new_text) {
         result = strdup(TOOL_RESULT_ERROR_MARK "Out of memory.");
      } else {
         char ok[DOCMGMT_CONFIRM_MSG_MAX];
         snprintf(ok, sizeof(ok), "Edited %s '%s' (replaced 1 occurrence).",
                  is_note ? "note" : "document", doc.filename);
         result = commit_edit(user_id, &doc, is_note, new_text, ok);
         OLOG_INFO("document_manage edit: %s id=%lld find_len=%zu", is_note ? "note" : "doc",
                   (long long)doc.id, strlen(find));
         free(new_text);
      }
   }
   pthread_mutex_unlock(&s_edit_mutex);
   free(old);
   json_object_put(change);
   return result;
}

/* append: add text to the end of an existing note or document (newline-joined). */
static char *do_append(int user_id, const char *label, int64_t id, const char *text) {
   if (!text || !text[0])
      return strdup("To append, provide the text to add.");

   char *result = NULL;
   pthread_mutex_lock(&s_edit_mutex);

   document_t doc;
   char *old = NULL;
   bool is_note = false;
   const char *err = NULL;
   if (load_editable_text(user_id, label, id, &doc, &old, &is_note, &err) != SUCCESS) {
      result = strdup(err); /* nonexistent target → error, never auto-create */
   } else {
      char *new_text = docmgmt_append_text(old, text);
      if (!new_text) {
         result = strdup(TOOL_RESULT_ERROR_MARK "Out of memory.");
      } else {
         char ok[DOCMGMT_CONFIRM_MSG_MAX];
         snprintf(ok, sizeof(ok), "Appended to %s '%s'.", is_note ? "note" : "document",
                  doc.filename);
         result = commit_edit(user_id, &doc, is_note, new_text, ok);
         OLOG_INFO("document_manage append: %s id=%lld add_len=%zu", is_note ? "note" : "doc",
                   (long long)doc.id, strlen(text));
         free(new_text);
      }
   }
   pthread_mutex_unlock(&s_edit_mutex);
   free(old);
   return result;
}

static char *do_list(int user_id) {
   document_t docs[DOC_MAX_RESULTS];
   int count = 0;
   if (document_db_list(user_id, docs, DOC_MAX_RESULTS, 0, &count) != SUCCESS)
      return strdup(TOOL_RESULT_ERROR_MARK "Couldn't list your documents.");
   if (count == 0)
      return strdup("You have no saved documents or notes.");

   strbuf_t sb;
   strbuf_init(&sb, 1024);
   strbuf_appendf(&sb, "Stored documents and notes (%d):\n", count);
   for (int i = 0; i < count; i++) {
      bool is_note = (strcmp(docs[i].filetype, "note") == 0);
      strbuf_appendf(&sb, "- [id %lld] %s (%s)\n", (long long)docs[i].id, docs[i].filename,
                     is_note ? "note" : docs[i].filetype);
   }
   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup(TOOL_RESULT_ERROR_MARK "Your document list is too long to display in full.");
   }
   char *out = strbuf_steal(&sb);
   return out ? out : strdup(TOOL_RESULT_ERROR_MARK "Couldn't list your documents.");
}

/* list_deleted: show recently-deleted notes/documents still recoverable. */
static char *do_list_deleted(int user_id) {
   document_version_meta_t v[DOC_VERSION_MAX_LIST];
   int n = 0;
   if (document_db_version_list_deleted(user_id, v, DOC_VERSION_MAX_LIST, &n) != SUCCESS)
      return strdup(TOOL_RESULT_ERROR_MARK "Couldn't check recently deleted items.");
   if (n == 0)
      return strdup("Nothing recoverable — no items were deleted within the retention window.");

   strbuf_t sb;
   strbuf_init(&sb, 512);
   strbuf_appendf(&sb, "Recently deleted (%d) — recover any by its name:\n", n);
   for (int i = 0; i < n; i++)
      strbuf_appendf(&sb, "- '%s'\n", v[i].filename);
   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup(TOOL_RESULT_ERROR_MARK "The deleted-items list is too long to show in full.");
   }
   char *out = strbuf_steal(&sb);
   return out ? out : strdup(TOOL_RESULT_ERROR_MARK "Couldn't list deleted items.");
}

/* recover: undo the last change to an item, OR bring a deleted item back.
 *   - If the item STILL EXISTS, restore its newest saved version in place (undo
 *     the last edit/overwrite).  The restore is itself snapshotted, so repeating
 *     'recover' toggles between the two most-recent states (undo / redo).
 *   - If the item was DELETED, re-create it from its surviving snapshot (as a
 *     note, or a document if the snapshot is too long for one note). */
static char *do_recover(int user_id, const char *label, int64_t id) {
   if (id <= 0 && (!label || !label[0]))
      return strdup(
          "Which item should I undo or recover? Give its name or id (use 'list_deleted').");

   /* Existing item → undo its last change by restoring the newest version. */
   document_t doc;
   if (resolve_owned_doc(user_id, label, id, &doc) == SUCCESS) {
      document_version_meta_t ev[DOC_VERSION_MAX_LIST];
      int en = 0;
      document_db_version_list(user_id, doc.id, ev, DOC_VERSION_MAX_LIST, &en);
      if (en == 0)
         return strdup(
             "There's no earlier version to undo — it hasn't changed since it was saved.");
      char *vtext = NULL;
      if (document_db_version_get_text(ev[0].id, user_id, &vtext, NULL, 0, NULL) != SUCCESS ||
          !vtext)
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Couldn't read the saved version to undo the change.");
      bool is_note = (strcmp(doc.filetype, "note") == 0 && doc.num_chunks == 1);
      char ok[DOCMGMT_CONFIRM_MSG_MAX];
      snprintf(ok, sizeof(ok), "Undid the last change to %s '%s' (restored the previous version).",
               is_note ? "note" : "document", doc.filename);
      pthread_mutex_lock(&s_edit_mutex);
      char *result = commit_edit(user_id, &doc, is_note, vtext, ok);
      pthread_mutex_unlock(&s_edit_mutex);
      free(vtext);
      return result;
   }

   /* Otherwise the item was deleted — re-create it from its surviving snapshot.
    * A deleted item has no live id, so recovery is by name; an id-only request
    * that didn't resolve to a live doc can't reach a deleted snapshot. */
   if (!label || !label[0])
      return strdup("That id isn't a live note or document. To bring back a deleted one, give its "
                    "name (use 'list_deleted').");

   document_version_meta_t v[DOC_VERSION_MAX_LIST];
   int n = 0;
   document_db_version_list_deleted(user_id, v, DOC_VERSION_MAX_LIST, &n);
   int64_t version_id = 0;
   int64_t old_doc_id = 0;
   char fname[DOC_FILENAME_MAX] = "";
   for (int i = 0; i < n; i++) {
      if (strcasecmp(v[i].filename, label) == 0) {
         version_id = v[i].id;
         old_doc_id = v[i].document_id;
         snprintf(fname, sizeof(fname), "%s", v[i].filename);
         break;
      }
   }
   if (version_id == 0)
      return strdup("No recently-deleted item by that name (use 'list_deleted' to see what's "
                    "recoverable).");

   char *text = NULL;
   if (document_db_version_get_text(version_id, user_id, &text, fname, sizeof(fname), NULL) !=
           SUCCESS ||
       !text)
      return strdup(TOOL_RESULT_ERROR_MARK "Couldn't read the saved version to recover it.");

   doc_index_result_t res;
   int rc = document_index_note(user_id, fname, text, strlen(text), false, &res);
   bool as_note = (rc == DOC_INDEX_SUCCESS);
   if (!as_note) /* too long for a note → it was a document */
      rc = document_index_text(user_id, fname, "text", text, strlen(text), false, NULL, &res);
   free(text);

   if (rc != DOC_INDEX_SUCCESS) {
      char msg[DOC_FILENAME_MAX + 160]; /* label + error_msg + prefix */
      snprintf(msg, sizeof(msg), TOOL_RESULT_ERROR_MARK "Couldn't recover '%s': %s", fname,
               res.error_msg);
      return strdup(msg);
   }
   if (as_note && res.doc_id > 0)
      (void)memory_note_bridge_upsert_gloss(user_id, res.doc_id, fname);

   /* Move the deleted item's surviving snapshots onto the re-created doc so it
    * leaves "Recently deleted" and a repeat restore can't duplicate it (#5). */
   if (res.doc_id > 0)
      (void)document_db_version_reattach(user_id, old_doc_id, res.doc_id);

   char msg[DOCMGMT_CONFIRM_MSG_MAX];
   snprintf(msg, sizeof(msg), "Recovered '%s'.", fname);
   return strdup(msg);
}

/* delete: resolve the target and STAGE it — never deletes here.  The user must
 * approve, after which the model calls confirm_delete. */
static char *do_delete_request(int user_id,
                               const char *label,
                               int64_t id,
                               const turn_origin_t *origin) {
   if (id <= 0 && (!label || !label[0]))
      return strdup("To delete, give the exact label/name (or the id from 'list').");

   /* Owner-scoped resolve: document_db_get / find_by_label_exact also match GLOBAL
    * docs, so without the ownership check inside resolve_owned_doc a user could
    * stage an admin-published global doc for deletion (IDOR — delete_indexed
    * performs no ownership check of its own). */
   document_t doc;
   if (resolve_owned_doc(user_id, label, id, &doc) != SUCCESS)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "No note or document by that name was found (it may not be yours).");

   bool is_note = (strcmp(doc.filetype, "note") == 0);
   pending_stage_rc_t src = PENDING_STAGED;
   const uint32_t pending_id = stage_pending(origin, user_id, doc.id, doc.filename, is_note, &src);
   if (pending_id == 0)
      return tool_pending_stage_refusal(src, "deletion");

   char msg[DOCMGMT_CONFIRM_MSG_MAX]; /* base copy + up to DOC_FILENAME_MAX label */
   snprintf(msg, sizeof(msg),
            "This will delete the %s '%s' (recoverable with 'recover' for a short window if "
            "version history is on). Ask the user to confirm; if they say yes in their reply, call "
            "document_manage with action 'confirm_delete' and pending_id %u (this deletion only).",
            is_note ? "note" : "document", doc.filename, (unsigned)pending_id);
   return strdup(msg);
}

static char *do_confirm_delete(int user_id, const turn_origin_t *origin, const char *value) {
   char id_str[16] = "";
   uint32_t pending_id = 0;
   if (tool_param_extract_custom(value, "pending_id", id_str, sizeof(id_str)) && id_str[0]) {
      const long long n = strtoll(id_str, NULL, 10);
      pending_id = (n > 0 && n <= UINT32_MAX) ? (uint32_t)n : 0;
   }
   docmgmt_pending_t pending;
   char *err = take_pending(origin, user_id, pending_id, &pending);
   if (err)
      return err;
   const int64_t doc_id = pending.doc_id;
   const char *label = pending.label;
   const bool is_note = pending.is_note;

   /* Re-validate ownership at confirm time: the staged doc could have been
    * deleted and its rowid reused by a DIFFERENT doc in the up-to-5-minute window
    * (documents.id is not AUTOINCREMENT).  Confirm we still own this exact id
    * before deleting (TOCTOU / CWE-367 guard). */
   document_t doc;
   if (document_db_get(doc_id, &doc) != SUCCESS || doc.user_id != user_id)
      return strdup(TOOL_RESULT_ERROR_MARK "That item is no longer available to delete.");

   /* Drop the memory→note bridge gloss BEFORE the note row is deleted (the FK
    * nulls note_doc_id on delete, after which the gloss can't be found by it).
    * Best-effort + harmless for non-note docs (no gloss exists). */
   conv_db_withdraw_intent_begin(user_id); /* the user removing it, its gloss too */
   (void)memory_note_bridge_delete_gloss(user_id, doc_id);
   const int deleted = document_db_delete_indexed(doc_id);
   conv_db_withdraw_intent_end();
   if (deleted != SUCCESS)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "The deletion failed — the item may have already been removed.");
   /* Its passages leave the conversations they were sent into. */
   session_withdraw_forgotten_async(user_id, false);

   char msg[DOCMGMT_CONFIRM_MSG_MAX];
   snprintf(msg, sizeof(msg), "Deleted the %s '%s'.", is_note ? "note" : "document", label);
   return strdup(msg);
}

/* rename: change a note/document's name in place — content untouched.  Target is
 * identified by id (preferred) or current label; a same-name request is a no-op
 * and a name already in use by another of the user's items is rejected. */
static char *do_rename(int user_id, const char *label, int64_t id, const char *new_name) {
   if (!new_name || !new_name[0])
      return strdup("To rename, give the new name in 'new_name'.");
   if (id <= 0 && (!label || !label[0]))
      return strdup("Tell me which note or document to rename (its current name or id).");

   document_t doc;
   if (resolve_owned_doc(user_id, label, id, &doc) != SUCCESS)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "No note or document by that name was found (it may not be yours).");

   if (strcmp(doc.filename, new_name) == 0) {
      char msg[DOCMGMT_CONFIRM_MSG_MAX];
      snprintf(msg, sizeof(msg), "It's already named '%s' — nothing to rename.", doc.filename);
      return strdup(msg);
   }

   /* Reject a collision with another of the user's own items (mirrors the WebUI
    * note-save guard).  find_by_label_exact also matches GLOBAL items — a clash
    * with one of those is caught here too, since the new name still wouldn't be
    * unambiguously the user's. */
   document_t other;
   if (document_db_find_by_label_exact(user_id, new_name, false, &other) == SUCCESS &&
       other.id != doc.id) {
      char msg[DOC_FILENAME_MAX + 96];
      snprintf(msg, sizeof(msg),
               "You already have a note or document called '%s' — pick a different name.",
               new_name);
      return strdup(msg);
   }

   bool is_note = (strcmp(doc.filetype, "note") == 0);
   if (document_db_rename(user_id, doc.id, new_name) != SUCCESS)
      return strdup(TOOL_RESULT_ERROR_MARK "Couldn't rename it — please try again.");

   /* Memory→note bridge (v61): the gloss maps note_doc_id → label, so a rename
    * must refresh it or fuzzy recall keeps routing to the old name.  Best-effort;
    * harmless for non-note docs (no gloss exists). */
   if (is_note)
      (void)memory_note_bridge_upsert_gloss(user_id, doc.id, new_name);

   OLOG_INFO("document_manage rename: %s id=%lld", is_note ? "note" : "doc", (long long)doc.id);
   char msg[DOC_FILENAME_MAX * 2 + 64];
   snprintf(msg, sizeof(msg), "Renamed the %s to '%s'.", is_note ? "note" : "document", new_name);
   return strdup(msg);
}

/* =============================================================================
 * Callback + registration
 * ============================================================================= */

static char *doc_manage_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;
   int user_id = tool_get_current_user_id();
   if (user_id <= 0)
      return strdup(TOOL_GUEST_REFUSAL);

   char act[32] = "";
   if (action)
      snprintf(act, sizeof(act), "%s", action);

   if (strcmp(act, "list") == 0)
      return do_list(user_id);
   /* Deleting needs the user in a live conversation (not a background job, a
    * re-engaged turn or an MQTT message), and its confirm their next turn in
    * the session that staged it. */
   turn_origin_t origin = { 0 };
   const bool deletes = strcmp(act, "delete") == 0 || strcmp(act, "confirm_delete") == 0;
   if (deletes && !turn_origin_capture(&origin))
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Deleting needs the user in a live conversation, and this request came from "
                    "a background job or an automated turn. Tell the user what you would delete, "
                    "and let them ask for it.");
   if (strcmp(act, "confirm_delete") == 0)
      return do_confirm_delete(user_id, &origin, value);
   if (strcmp(act, "list_deleted") == 0)
      return do_list_deleted(user_id);

   /* The remaining actions read a label (base) and possibly id / text (custom).
    * The numeric id is an alternative target selector for edit / append / delete /
    * rename / recover (see resolve_owned_doc). */
   char label[DOC_FILENAME_MAX] = "";
   tool_param_extract_base(value, label, sizeof(label));

   char id_str[24] = "";
   int64_t id = 0;
   if (tool_param_extract_custom(value, "id", id_str, sizeof(id_str)) && id_str[0])
      id = (int64_t)strtoll(id_str, NULL, 10);

   if (strcmp(act, "recover") == 0)
      return do_recover(user_id, label, id);

   if (strcmp(act, "delete") == 0)
      return do_delete_request(user_id, label, id, &origin);

   if (strcmp(act, "rename") == 0) {
      char new_name[DOC_FILENAME_MAX] = "";
      tool_param_extract_custom(value, "new_name", new_name, sizeof(new_name));
      return do_rename(user_id, label, id, new_name);
   }

   /* edit: the JSON 'change' object. */
   if (strcmp(act, "edit") == 0) {
      char *change = malloc(DOCMGMT_SAVE_TEXT_MAX);
      if (!change)
         return strdup(TOOL_RESULT_ERROR_MARK "Out of memory.");
      change[0] = '\0';
      tool_param_extract_custom(value, "change", change, DOCMGMT_SAVE_TEXT_MAX);
      char *result = do_edit(user_id, label, id, change);
      free(change);
      return result;
   }

   /* save_note / save_text / append: the text, in a buffer of
    * DOCMGMT_SAVE_TEXT_MAX (the tool-arg ceiling), NOT the per-chunk size, so a
    * multi-chunk save_text document isn't clipped at 4 KB. */
   char *text = malloc(DOCMGMT_SAVE_TEXT_MAX);
   if (!text)
      return strdup(TOOL_RESULT_ERROR_MARK "Out of memory.");
   text[0] = '\0';
   tool_param_extract_custom(value, "text", text, DOCMGMT_SAVE_TEXT_MAX);

   char *result;
   if (strcmp(act, "save_note") == 0)
      result = do_save_note(user_id, label, text);
   else if (strcmp(act, "save_text") == 0)
      result = do_save_text(user_id, label, text);
   else if (strcmp(act, "append") == 0)
      result = do_append(user_id, label, id, text);
   else
      result = strdup(TOOL_RESULT_ERROR_MARK "Unknown document_manage action.");

   free(text);
   return result;
}

int document_manage_tool_register(void) {
   return tool_registry_register(&doc_manage_metadata);
}
