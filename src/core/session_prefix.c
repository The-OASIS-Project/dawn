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
 * A conversation's request, append-only (session_prefix.h).
 */

#include "core/session_prefix.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_conv_prefix.h"
#include "auth/auth_db_messages.h"
#include "auth/auth_db_withdraw.h"
#include "core/focus/focus_handles.h"
#include "core/prefix_in_force.h"
#include "core/prefix_message.h"
#include "core/session_history.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "llm/llm_context_text.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"

static int cmp_str(const void *a, const void *b) {
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* A turn's record: what its prompt added, and where its question was saved.
 * It is saved with the conversation once it has both (session_prefix_apply_turn
 * and session_prefix_question_saved, in either order), its rows naming its own
 * question.  A session keeps them oldest first (session->prefix_turn): a turn's
 * question can be saved after a later turn began (a new chat's first exchange,
 * adopted once its conversation exists).  Every json reference in it is taken
 * and released under history_mutex. */
struct session_prefix_turn {
   struct session_prefix_turn *next; /* the next newer record */
   struct json_object *hist;         /* the history it belongs to (a reference) */
   bool whole;                      /* its surface saves the whole history at once (a voice save) */
   bool applied;                    /* the turn's prompt was applied */
   bool saved;                      /* its question is saved: row question_id of conv_id */
   struct json_object *appended;    /* messages it appended, in order (refs) */
   struct json_object *question;    /* the question its context went in front of (ref), or NULL */
   struct json_object *context_msg; /* an envelope's context, its own message (ref), or NULL */
   /* Messages earlier turns appended whose questions were never saved (refs):
    * saved with this one's rows, naming no question. */
   struct json_object *carried;
   char *prefix; /* the prefix and tool set it ran under, to store */
   char *tools;
   char *in_force;    /* what is in force after it (prefix_in_force_json), to store */
   bool boundary;     /* it left reasoning behind: the conversation's floor rises */
   int64_t built_at;  /* when its prompt was built */
   int64_t built_seq; /* where withdrawals stood then */
   int64_t question_id;
   int64_t conv_id;
   int user_id;
};

/* A row object's text field. */
static const char *row_field(struct json_object *row, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(row, key, &v) ? json_object_get_string(v) : NULL;
}

/* A message's row id, or 0. */
static int64_t id_of(struct json_object *msg) {
   struct json_object *id = NULL;
   return json_object_object_get_ex(msg, "id", &id) ? json_object_get_int64(id) : 0;
}

/* At most this many records wait for their questions; an older one is given
 * up (a boundary) rather than kept for good. */
#define PREFIX_TURNS_WAITING_MAX 4

static void turn_free_one(struct session_prefix_turn *turn) {
   json_object_put(turn->hist);
   json_object_put(turn->appended);
   json_object_put(turn->question);
   json_object_put(turn->context_msg);
   json_object_put(turn->carried);
   free(turn->prefix);
   free(turn->tools);
   free(turn->in_force);
   free(turn);
}

void session_prefix_turn_free(struct session_prefix_turn *turn) {
   while (turn) {
      struct session_prefix_turn *next = turn->next;
      turn_free_one(turn);
      turn = next;
   }
}

/* Caller holds history_mutex.  Free @p turn (not in the queue), its
 * references released as the history's rules require (session_release_ref). */
static void turn_release_locked(session_t *session, struct session_prefix_turn *turn) {
   if (!turn) {
      return;
   }
   session_release_ref_locked(session, turn->appended);
   session_release_ref_locked(session, turn->question);
   session_release_ref_locked(session, turn->context_msg);
   session_release_ref_locked(session, turn->carried);
   session_release_ref_locked(session, turn->hist);
   turn->appended = turn->question = turn->context_msg = turn->carried = turn->hist = NULL;
   turn_free_one(turn);
}

/* Caller holds history_mutex.  Take @p turn out of the session's queue. */
static void unlink_locked(session_t *session, struct session_prefix_turn *turn) {
   for (struct session_prefix_turn **at = &session->prefix_turn; *at; at = &(*at)->next) {
      if (*at == turn) {
         *at = turn->next;
         turn->next = NULL;
         return;
      }
   }
}

/* Caller holds history_mutex.  The newest record of @p hist, or NULL. */
static struct session_prefix_turn *newest_of_locked(session_t *session, struct json_object *hist) {
   struct session_prefix_turn *found = NULL;
   for (struct session_prefix_turn *t = session->prefix_turn; t; t = t->next) {
      if (t->hist == hist) {
         found = t;
      }
   }
   return found;
}

/* Caller holds history_mutex.  @p from's messages (appended and carried)
 * move to @p into's carried: saved with it, naming no question. */
static void carry_messages_locked(struct session_prefix_turn *into,
                                  struct session_prefix_turn *from) {
   if (!into->carried) {
      into->carried = json_object_new_array();
   }
   struct json_object *const lists[] = { from->carried, from->appended };
   for (size_t l = 0; into->carried && l < sizeof(lists) / sizeof(lists[0]); l++) {
      const size_t n = lists[l] ? json_object_array_length(lists[l]) : 0;
      for (size_t i = 0; i < n; i++) {
         json_object_array_add(into->carried,
                               json_object_get(json_object_array_get_idx(lists[l], i)));
      }
   }
}

/* Caller holds history_mutex.  What belongs to the conversation rather than
 * to one question (the prefix and tool set to store, what is in force, a
 * boundary) moves from @p from to the newer @p into, which saves it. */
static void take_conversation_locked(struct session_prefix_turn *into,
                                     struct session_prefix_turn *from) {
   if (!into->prefix && from->prefix) {
      into->prefix = from->prefix;
      into->tools = from->tools;
      from->prefix = NULL;
      from->tools = NULL;
   }
   if (!into->in_force) {
      into->in_force = from->in_force;
      from->in_force = NULL;
   }
   into->boundary = into->boundary || from->boundary;
   from->boundary = false;
}

/* Caller holds history_mutex.  Records of @p turn's history older than it
 * whose questions weren't saved won't be (a later question was): their
 * messages go with @p turn, and what their questions were sent with is lost
 * from the saved conversation, so it leaves its reasoning behind. */
static void give_up_older_locked(session_t *session, struct session_prefix_turn *turn) {
   struct session_prefix_turn *t = session->prefix_turn;
   while (t && t != turn) {
      struct session_prefix_turn *next = t->next;
      if (t->hist == turn->hist && !t->saved) {
         unlink_locked(session, t);
         carry_messages_locked(turn, t);
         take_conversation_locked(turn, t);
         if (t->applied) {
            turn->boundary = true;
            OLOG_INFO("Session %u: an earlier turn's question was never saved; its request "
                      "context is left behind (boundary)",
                      session->session_id);
         }
         turn_release_locked(session, t);
      }
      t = next;
   }
}

/* Caller holds history_mutex.  Whether @p turn is ready to save: applied, and
 * its question saved (or it has none, and its history's conversation is
 * known).  A surface that saves its whole history saves none. */
static bool ready_locked(const struct session_prefix_turn *turn) {
   return turn && !turn->whole && turn->applied && turn->conv_id > 0 && turn->user_id > 0 &&
          (turn->question ? turn->saved : true);
}

static void save_turn(session_t *session, struct session_prefix_turn *turn);
static bool withdraw_pending_locked(session_t *session);

struct json_object *session_prefix_tool_names(session_t *session) {
   if (!session) {
      return NULL;
   }
   struct json_object *out = NULL;
   pthread_mutex_lock(&session->history_mutex);
   /* Copied element by element (a running turn reads the history unlocked;
    * serializing an object caches into it). */
   struct json_object *names = session_turn_reads_elsewhere_locked(session)
                                   ? NULL
                                   : llm_history_frozen_tools(session->conversation_history);
   const size_t n = names ? json_object_array_length(names) : 0;
   if (names) {
      out = json_object_new_array();
   }
   for (size_t i = 0; out && i < n; i++) {
      const char *name = json_object_get_string(json_object_array_get_idx(names, i));
      if (name) {
         json_object_array_add(out, json_object_new_string(name));
      }
   }
   pthread_mutex_unlock(&session->history_mutex);
   return out;
}

bool session_prefix_tag(session_t *session, char *out, size_t size) {
   if (!session || !out || size == 0) {
      return false;
   }
   out[0] = '\0';
   pthread_mutex_lock(&session->history_mutex);
   struct json_object *hist = session->turn_history ? session->turn_history
                                                    : session->conversation_history;
   const char *tag = llm_history_tag(hist);
   if (tag) {
      snprintf(out, size, "%s", tag);
   }
   pthread_mutex_unlock(&session->history_mutex);
   return out[0] != '\0';
}

char *session_prefix_mask_secret(session_t *session, char *text) {
   char tag[LLM_CONTEXT_TAG_MAX];
   if (!text || !session || !session_prefix_tag(session, tag, sizeof(tag))) {
      return text;
   }
   return llm_context_mask_tag(text, tag);
}

bool session_prefix_is_frozen(struct json_object *history) {
   return json_object_is_type(history, json_type_array) && json_object_array_length(history) > 0 &&
          llm_history_kind_of(json_object_array_get_idx(history, 0)) == MESSAGE_KIND_PREFIX;
}

static struct json_object *system_message(const char *text, message_kind_t kind) {
   struct json_object *msg = json_object_new_object();
   if (msg) {
      json_object_object_add(msg, "role", json_object_new_string("system"));
      json_object_object_add(msg, "content", json_object_new_string(text));
      llm_history_set_kind(msg, kind);
   }
   return msg;
}

/* Caller holds history_mutex.  The history the turn's prompt belongs to: the
 * turn's own when the user has since opened another conversation, else the
 * live one (*pinned_live: the turn's pin is the live history). */
static struct json_object **target_locked(session_t *session, bool *pinned_live) {
   if (!session->conversation_history) {
      session->conversation_history = json_object_new_array();
   }
   const bool elsewhere = session->turn_history != NULL &&
                          session->turn_history != session->conversation_history;
   *pinned_live = session->turn_history != NULL && !elsewhere;
   return elsewhere ? &session->turn_history : &session->conversation_history;
}

/* A declared boundary: every turn in @p hist replays without the reasoning
 * its model gave it (bound to a request that no longer reads the same). */
static int drop_reasoning(struct json_object *hist) {
   return llm_history_drop_turn_blocks(hist);
}

/* Caller holds history_mutex.  Make the history lead with a frozen prefix:
 * @p cp's system prompt (its sections recorded as in force), or with no
 * @p cp the system prompt the history has.  Its other system messages that
 * aren't request context (an older build's per-turn blocks) go; reasoning
 * bound to the prompt it had goes too (text and tool calls stay). */
static bool freeze_locked(session_t *session,
                          struct json_object **slot,
                          bool pinned_live,
                          const composed_prompt_t *cp,
                          bool *boundary) {
   struct json_object *old = *slot;
   const int len = old ? (int)json_object_array_length(old) : 0;
   const char *text = cp ? cp->stable_prefix : NULL;
   for (int i = 0; !text && i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(old, i);
      if (llm_history_role_is(msg, "system") && llm_history_kind_of(msg) == MESSAGE_KIND_NONE) {
         text = llm_history_text(msg);
      }
   }
   if (!text || !*text) {
      return false; /* nothing to freeze yet */
   }
   /* A conversation's own tag, declared by the prompt it freezes. */
   const bool from_cp = cp && text == cp->stable_prefix;
   char tag[LLM_CONTEXT_TAG_MAX];
   prefix_in_force_new_tag(tag, sizeof(tag));
   char *tagged = from_cp ? llm_context_with_tag(text, tag) : NULL;
   struct json_object *prefix = (!from_cp || tagged)
                                    ? prefix_message_new(from_cp ? tagged : text, NULL, NULL)
                                    : NULL;
   free(tagged);
   struct json_object *rebuilt = prefix ? json_object_new_array() : NULL;
   if (!rebuilt) {
      json_object_put(prefix);
      OLOG_ERROR("Session %u: out of memory freezing the system prompt", session->session_id);
      return false;
   }
   if (from_cp) {
      prefix_in_force_init(prefix, cp, tag);
   }
   json_object_array_add(rebuilt, prefix);
   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(old, i);
      if (llm_history_role_is(msg, "system") && llm_history_kind_of(msg) == MESSAGE_KIND_NONE) {
         continue;
      }
      json_object_array_add(rebuilt, json_object_get(msg));
   }
   const int dropped_reasoning = drop_reasoning(rebuilt);
   if (dropped_reasoning > 0) {
      OLOG_INFO("Session %u: prefix boundary (adopted): %d turn(s) replay without their "
                "reasoning",
                session->session_id, dropped_reasoning);
      *boundary = true;
   }
   json_object_put(*slot);
   *slot = rebuilt;
   if (pinned_live) {
      json_object_put(session->turn_history);
      session->turn_history = json_object_get(session->conversation_history);
   }
   return true;
}

/* Caller holds history_mutex.  Fix the conversation's tool set to @p names (a
 * JSON array).  A conversation's first sets it; a change after that (a tool
 * registered or gone: an MCP server connecting) is a declared boundary. */
static bool set_tool_set_locked(session_t *session,
                                struct json_object *hist,
                                const char *names,
                                bool *boundary) {
   struct json_object *want = names ? json_tokener_parse(names) : NULL;
   if (!json_object_is_type(want, json_type_array)) {
      json_object_put(want);
      return false;
   }
   struct json_object *prefix = json_object_array_get_idx(hist, 0);
   struct json_object *had = NULL;
   json_object_object_get_ex(prefix, LLM_HISTORY_TOOLS_KEY, &had);
   if (had && json_object_equal(had, want)) {
      json_object_put(want);
      return false;
   }
   const int dropped = drop_reasoning(hist);
   if (had || dropped > 0) {
      OLOG_INFO("Session %u: prefix boundary (tool_set_changed): %zu tool(s) now, %d turn(s) "
                "replay without their reasoning",
                session->session_id, json_object_array_length(want), dropped);
   }
   *boundary = *boundary || dropped > 0;
   json_object_object_add(prefix, LLM_HISTORY_TOOLS_KEY, want);
   return true;
}

/* Caller holds history_mutex.  The turn's question in @p hist, or NULL. */
static struct json_object *question_locked(session_t *session, struct json_object *hist) {
   const int len = (int)json_object_array_length(hist);
   for (int i = len - 1; i >= 0; i--) {
      if (json_object_array_get_idx(hist, i) == session->turn_user_msg &&
          llm_history_is_question(session->turn_user_msg)) {
         return session->turn_user_msg;
      }
   }
   /* A surface that doesn't mark its question: the newest, unless a turn
    * already sent it with its context (the question this turn meant to add
    * wasn't added: that one stays as it was sent). */
   for (int i = len - 1; i >= 0; i--) {
      struct json_object *msg = json_object_array_get_idx(hist, i);
      if (llm_history_is_question(msg)) {
         return llm_history_has_context_parts(msg) ? NULL : msg;
      }
      if (llm_history_role_is(msg, "assistant") || llm_history_role_is(msg, "tool")) {
         return NULL; /* the last turn's reply: this turn has no question */
      }
   }
   return NULL;
}

/* Put @p parts in front of @p question's own content (replacing any context
 * a failed earlier attempt of this turn put there). */
static bool set_context_parts(struct json_object *question, struct json_object *parts) {
   struct json_object *content = NULL;
   json_object_object_get_ex(question, "content", &content);
   struct json_object *merged = json_object_new_array();
   if (!merged) {
      return false;
   }
   const int np = (int)json_object_array_length(parts);
   for (int i = 0; i < np; i++) {
      json_object_array_add(merged, json_object_get(json_object_array_get_idx(parts, i)));
   }
   if (json_object_is_type(content, json_type_array)) {
      const int n = (int)json_object_array_length(content);
      for (int i = 0; i < n; i++) {
         struct json_object *part = json_object_array_get_idx(content, i);
         if (llm_history_kind_of(part) == MESSAGE_KIND_NONE) {
            json_object_array_add(merged, json_object_get(part));
         }
      }
   } else {
      const char *text = json_object_get_string(content);
      if (text && *text) {
         json_object_array_add(merged, llm_history_context_part(text, MESSAGE_KIND_NONE));
      }
   }
   json_object_object_add(question, "content", merged);
   return true;
}

/* A block DAWN frames: @p body between its tagged open and close lines. */
static char *framed(const char *name,
                    const char *tag,
                    const char *body,
                    const char *more1,
                    const char *more2) {
   strbuf_t sb;
   strbuf_init(&sb, 512);
   if (tag) {
      strbuf_appendf(&sb, "--- %s (%s) ---\n", name, tag);
   } else {
      strbuf_appendf(&sb, "--- %s ---\n", name);
   }
   const char *pieces[] = { body, more1, more2 };
   for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
      const char *piece = pieces[i];
      if (piece && *piece) {
         strbuf_append(&sb, piece);
         if (piece[strlen(piece) - 1] != '\n') {
            strbuf_append(&sb, "\n");
         }
      }
   }
   if (tag) {
      strbuf_appendf(&sb, "--- END %s (%s) ---\n", name, tag);
   } else {
      strbuf_appendf(&sb, "--- END %s ---\n", name);
   }
   char *out = strbuf_oom(&sb) ? NULL : strbuf_steal(&sb);
   strbuf_free(&sb);
   return out;
}

/* Caller holds history_mutex.  An envelope's context (@p parts, taken) in a
 * message of its own just before it: DAWN's framing never shares a message
 * with untrusted text.  A failed earlier attempt's context there is replaced.
 * Returns the context message (borrowed), or NULL. */
static struct json_object *envelope_context_locked(struct json_object *hist,
                                                   struct json_object *envelope,
                                                   struct json_object *parts) {
   const int len = (int)json_object_array_length(hist);
   int at = -1;
   for (int i = len - 1; i >= 0 && at < 0; i--) {
      if (json_object_array_get_idx(hist, i) == envelope) {
         at = i;
      }
   }
   if (at < 0) {
      json_object_put(parts);
      return NULL;
   }
   /* An earlier attempt's own context (never saved as a message: no id), not
    * another turn's (a question-less turn's context, saved). */
   struct json_object *before = at > 0 ? json_object_array_get_idx(hist, at - 1) : NULL;
   if (before && llm_history_role_is(before, "user") && id_of(before) == 0 &&
       llm_history_kind_of(before) == MESSAGE_KIND_NONE && llm_history_is_context(before)) {
      json_object_object_add(before, "content", parts);
      return before;
   }
   struct json_object *msg = llm_history_context_message(parts);
   return (msg && llm_history_insert(hist, (size_t)at, msg)) ? msg : NULL;
}

/* The turn's context: the builder's body (the time, retrieved items), this
 * turn's note and new device events, framed with the conversation's tag.
 * NULL when there is nothing. */
static char *turn_context_text(const char *tag,
                               const char *body,
                               const char *note,
                               const char *notices) {
   if ((!body || !*body) && (!note || !*note) && (!notices || !*notices)) {
      return NULL;
   }
   return framed("TURN CONTEXT", tag, body, note, notices);
}

/* What DAWN knows about the user, framed with the conversation's tag. */
static char *memory_text(const char *tag, const char *body) {
   return (body && *body) ? framed("USER MEMORY", tag, body, NULL, NULL) : NULL;
}

void session_prefix_apply_turn(session_t *session,
                               const composed_prompt_t *cp,
                               const char *turn_note) {
   if (!session) {
      return;
   }
   const int viewer = session_effective_user_id(session);
   pthread_mutex_lock(&session->history_mutex);
   bool pinned_live = false;
   struct json_object **slot = target_locked(session, &pinned_live);
   if (!*slot) {
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }
   /* This turn's record.  Records of a history the session no longer runs
    * went with it (session_prefix_release_locked); an earlier record of this
    * one whose question isn't saved yet waits for it, and what belongs to the
    * conversation rather than to that question comes to this one.  A voice
    * surface's history is saved whole: its earlier records have nothing left
    * to save. */
   const bool whole = session->type == SESSION_TYPE_LOCAL || session->type == SESSION_TYPE_DAP ||
                      session->type == SESSION_TYPE_DAP2;
   for (struct session_prefix_turn *t = session->prefix_turn; t;) {
      struct session_prefix_turn *next = t->next;
      if (t->hist != session->conversation_history && t->hist != session->turn_history) {
         unlink_locked(session, t);
         turn_release_locked(session, t);
      }
      t = next;
   }
   struct session_prefix_turn *old = newest_of_locked(session, *slot);
   struct session_prefix_turn *turn = calloc(1, sizeof(*turn));
   if (turn) {
      turn->hist = json_object_get(*slot);
      turn->whole = whole;
      turn->appended = json_object_new_array();
      turn->built_at = cp ? cp->built_at : 0;
      turn->built_seq = cp ? cp->built_seq : 0;
      struct session_prefix_turn **tail = &session->prefix_turn;
      while (*tail) {
         tail = &(*tail)->next;
      }
      *tail = turn;
      if (old) {
         take_conversation_locked(turn, old);
      }
      int waiting = 0;
      for (struct session_prefix_turn *t = session->prefix_turn; t != turn; t = t->next) {
         waiting += t->hist == turn->hist;
      }
      for (struct session_prefix_turn *t = session->prefix_turn; t != turn;) {
         struct session_prefix_turn *next = t->next;
         if (t->hist == turn->hist && (whole || waiting > PREFIX_TURNS_WAITING_MAX)) {
            waiting--;
            unlink_locked(session, t);
            if (!whole) {
               carry_messages_locked(turn, t);
               turn->boundary = turn->boundary || t->applied;
            }
            turn_release_locked(session, t);
         }
         t = next;
      }
   }
   char *in_force_before = prefix_in_force_json(*slot);
   bool boundary = turn && turn->boundary;
   /* A withdrawal that came while the last turn was reading: applied now. */
   if (withdraw_pending_locked(session)) {
      boundary = true;
   }
   bool bind = false;
   if (!session_prefix_is_frozen(*slot)) {
      struct json_object *unfrozen = *slot;
      bind = freeze_locked(session, slot, pinned_live, cp, &boundary);
      /* Freezing builds a new array of the same messages: the records of the
       * old one belong to it now. */
      for (struct session_prefix_turn *t = session->prefix_turn; *slot != unfrozen && t;
           t = t->next) {
         if (t->hist == unfrozen) {
            session_release_ref_locked(session, t->hist);
            t->hist = json_object_get(*slot);
         }
      }
   }
   struct json_object *hist = *slot;
   const bool frozen = session_prefix_is_frozen(hist);
   if (frozen && cp && cp->tool_names) {
      bind = set_tool_set_locked(session, hist, cp->tool_names, &boundary) || bind;
   }
   if (turn && bind) {
      struct json_object *first = json_object_array_get_idx(hist, 0);
      struct json_object *tools = NULL;
      free(turn->prefix);
      free(turn->tools);
      turn->prefix = llm_history_text(first) ? strdup(llm_history_text(first)) : NULL;
      turn->tools = NULL;
      if (json_object_object_get_ex(first, LLM_HISTORY_TOOLS_KEY, &tools)) {
         turn->tools = strdup(json_object_to_json_string_ext(tools, JSON_C_TO_STRING_PLAIN));
      }
   }
   if (turn) {
      turn->boundary = boundary;
   }

   /* The conversation's tag (made now for a history frozen before it had
    * one: the instructions below then declare it). */
   const char *tag = frozen ? prefix_in_force_ensure_tag(hist) : NULL;

   /* Changes after the question: the sections of the instructions that
    * changed, then the surface's directions when they did. */
   char *instructions = (frozen && cp) ? prefix_in_force_instructions(hist, cp) : NULL;
   if (instructions) {
      struct json_object *msg = system_message(instructions, MESSAGE_KIND_INSTRUCTION);
      if (msg) {
         json_object_array_add(hist, msg);
         if (turn && turn->appended) {
            json_object_array_add(turn->appended, json_object_get(msg));
         }
         OLOG_INFO("Session %u: instructions changed; the change appended (%zu chars)",
                   session->session_id, strlen(instructions));
      }
      free(instructions);
   }
   if (frozen && cp && cp->directives && prefix_in_force_directives_changed(hist, cp->directives)) {
      struct json_object *msg = system_message(prefix_in_force_directives_text(cp->directives),
                                               MESSAGE_KIND_DIRECTIVE);
      if (msg) {
         json_object_array_add(hist, msg);
         if (turn && turn->appended) {
            json_object_array_add(turn->appended, json_object_get(msg));
         }
      }
   }
   if (frozen && cp && cp->tool_schemas) {
      prefix_in_force_check_tool_schemas(hist, cp->tool_schemas);
   }
   /* What is in force, to store when it changed. */
   char *in_force_after = prefix_in_force_json(hist);
   if (turn && in_force_after &&
       (!in_force_before || strcmp(in_force_before, in_force_after) != 0)) {
      free(turn->in_force);
      turn->in_force = in_force_after;
      in_force_after = NULL;
   }
   free(in_force_before);
   free(in_force_after);

   /* In front of the question: what DAWN knows (when it changed), then the
    * turn's context. */
   struct json_object *question = question_locked(session, hist);
   char *notices = session_take_new_notices_locked(session, viewer);
   char *context = turn_context_text(tag, cp ? cp->volatile_block : NULL, turn_note, notices);
   char *memory = memory_text(tag, cp ? cp->memory_body : NULL);
   struct json_object *parts = json_object_new_array();
   if (parts && memory) {
      const char *had = llm_history_memory_in_force(hist);
      if (!had || strcmp(had, memory) != 0) {
         json_object_array_add(parts, llm_history_context_part(memory, MESSAGE_KIND_MEMORY));
      }
   }
   if (parts && context) {
      json_object_array_add(parts, llm_history_context_part(context, MESSAGE_KIND_TURN_CONTEXT));
   }
   if (turn && question) {
      /* The question the turn's rows are saved with, context or not; saved
       * already when its writer ran first (session_prefix_question_saved). */
      turn->question = json_object_get(question);
      const int64_t row = id_of(question);
      const int64_t hist_conv = hist == session->turn_history &&
                                        hist != session->conversation_history
                                    ? session->turn_history_conv
                                    : atomic_load(&session->history_conversation_id);
      if (row > 0 && session->prefix_saved.row_id == row) {
         turn->saved = true;
         turn->question_id = row;
         turn->conv_id = session->prefix_saved.conv_id;
         turn->user_id = session->prefix_saved.user_id;
      } else if (row > 0 && hist_conv > 0) {
         /* Stamped: it is saved, in its history's conversation. */
         turn->saved = true;
         turn->question_id = row;
         turn->conv_id = hist_conv;
         turn->user_id = viewer;
      }
   } else if (turn) {
      /* No question: the rows go with the conversation as they are. */
      turn->conv_id = hist == session->turn_history && hist != session->conversation_history
                          ? session->turn_history_conv
                          : atomic_load(&session->history_conversation_id);
      turn->user_id = viewer;
   }
   if (parts && json_object_array_length(parts) > 0) {
      if (question && llm_history_kind_of(question) == MESSAGE_KIND_ENVELOPE) {
         struct json_object *own = envelope_context_locked(hist, question, json_object_get(parts));
         if (turn && own) {
            json_object_put(turn->context_msg);
            turn->context_msg = json_object_get(own);
         }
      } else if (question) {
         (void)set_context_parts(question, parts);
      } else {
         /* No question (a turn DAWN started without one): the context on its own. */
         struct json_object *alone = json_object_new_object();
         if (alone) {
            json_object_object_add(alone, "role", json_object_new_string("user"));
            json_object_object_add(alone, "content", json_object_get(parts));
            json_object_array_add(hist, alone);
            if (turn && turn->appended) {
               json_object_array_add(turn->appended, json_object_get(alone));
            }
         }
      }
   }
   json_object_put(parts);
   free(context);
   free(memory);
   free(notices);
   if (turn) {
      turn->applied = true;
   }
   /* Its question already saved (or none to wait for): the record goes with
    * its conversation now. */
   struct session_prefix_turn *ready = NULL;
   if (ready_locked(turn)) {
      give_up_older_locked(session, turn);
      unlink_locked(session, turn);
      ready = turn;
   }
   pthread_mutex_unlock(&session->history_mutex);
   if (ready) {
      save_turn(session, ready);
   }
}

/* Caller holds history_mutex.  The rows @p turn saves, in order, into @p rows
 * (row objects); @p owner gets, per row, where it came from: -1 for a context
 * part, i >= 0 for turn->appended[i], -2 - i for turn->carried[i]. */
static void turn_rows_locked(struct session_prefix_turn *turn,
                             struct json_object *rows,
                             struct json_object *owner) {
   /* The question's context parts (an envelope's: in their own message). */
   struct json_object *content = NULL;
   struct json_object *holder = turn->context_msg ? turn->context_msg : turn->question;
   if (holder) {
      json_object_object_get_ex(holder, "content", &content);
   }
   const size_t n = json_object_is_type(content, json_type_array)
                        ? json_object_array_length(content)
                        : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      const message_kind_t kind = llm_history_kind_of(part);
      if (kind == MESSAGE_KIND_NONE) {
         continue;
      }
      struct json_object *row = json_object_new_object();
      struct json_object *text = NULL;
      json_object_object_get_ex(part, "text", &text);
      json_object_object_add(row, "role", json_object_new_string("user"));
      json_object_object_add(row, "content",
                             json_object_new_string(text ? json_object_get_string(text) : ""));
      llm_history_set_kind(row, kind);
      json_object_array_add(rows, row);
      json_object_array_add(owner, json_object_new_int(-1));
   }
   /* The messages it appended, then those it carries for earlier turns. */
   struct json_object *const lists[] = { turn->appended, turn->carried };
   for (int l = 0; l < 2; l++) {
      const size_t count = lists[l] ? json_object_array_length(lists[l]) : 0;
      for (size_t i = 0; i < count; i++) {
         const size_t before = json_object_array_length(rows);
         llm_history_rows_append(json_object_array_get_idx(lists[l], i), rows);
         for (size_t r = before; r < json_object_array_length(rows); r++) {
            json_object_array_add(owner, json_object_new_int(l == 0 ? (int)i : -2 - (int)i));
         }
      }
   }
}

/* Save @p turn (out of the session's queue) with its conversation, in one
 * transaction: its rows (naming its question; carried ones none), the prefix
 * and tool set it ran under, and a boundary's floor.  Then stamp the messages
 * with their rows' ids and free it, under the lock. */
static void save_turn(session_t *session, struct session_prefix_turn *turn) {
   pthread_mutex_lock(&session->history_mutex);
   const int64_t question_id = turn->question ? turn->question_id : 0;
   struct json_object *rows = json_object_new_array();
   struct json_object *owner = json_object_new_array();
   if (rows && owner) {
      turn_rows_locked(turn, rows, owner);
   }
   pthread_mutex_unlock(&session->history_mutex);

   const size_t n = rows ? json_object_array_length(rows) : 0;
   conv_message_row_t *db_rows = n ? calloc(n, sizeof(*db_rows)) : NULL;
   int64_t *ids = n ? calloc(n, sizeof(*ids)) : NULL;
   int rc = AUTH_DB_FAILURE;
   if (rows && owner && (n == 0 || (db_rows && ids))) {
      for (size_t i = 0; i < n; i++) {
         struct json_object *row = json_object_array_get_idx(rows, i);
         const int from = json_object_get_int(json_object_array_get_idx(owner, i));
         db_rows[i] = (conv_message_row_t){ .role = row_field(row, "role"),
                                            .content = row_field(row, "content"),
                                            .kind = row_field(row, MESSAGE_KIND_KEY),
                                            .context_of = from >= -1 ? question_id : 0 };
      }
      const conv_turn_save_t save = { .rows = db_rows,
                                      .n_rows = n,
                                      .prefix = turn->prefix,
                                      .tools = turn->tools,
                                      .in_force = turn->in_force,
                                      .floor_msg_id = turn->boundary ? question_id : 0,
                                      .floor_at_rows = turn->boundary && question_id == 0,
                                      .question_id = question_id,
                                      .built_at = turn->built_at,
                                      .built_seq = turn->built_seq };
      rc = conv_db_save_turn(turn->conv_id, turn->user_id, &save, ids);
   }
   if (rc != AUTH_DB_SUCCESS) {
      OLOG_WARNING("Session %u: the turn's request context wasn't saved to conv %lld (%d)",
                   session->session_id, (long long)turn->conv_id, rc);
   }

   pthread_mutex_lock(&session->history_mutex);
   /* A message takes its first row's id; not while another turn reads the
    * history (serializing it caches into its objects). */
   const bool stamp = rc == AUTH_DB_SUCCESS && !session_turn_reads_elsewhere_locked(session);
   for (size_t i = 0; stamp && i < n; i++) {
      const int from = json_object_get_int(json_object_array_get_idx(owner, i));
      struct json_object *msg = from >= 0 ? json_object_array_get_idx(turn->appended, (size_t)from)
                                : from <= -2
                                    ? json_object_array_get_idx(turn->carried, (size_t)(-2 - from))
                                    : NULL;
      if (msg && ids[i] > 0 && id_of(msg) == 0) {
         json_object_object_add(msg, "id", json_object_new_int64(ids[i]));
      }
   }
   turn_release_locked(session, turn);
   pthread_mutex_unlock(&session->history_mutex);
   free(db_rows);
   free(ids);
   json_object_put(rows);
   json_object_put(owner);
}

void session_prefix_question_saved(session_t *session,
                                   int64_t conv_id,
                                   int user_id,
                                   int64_t row_id) {
   if (!session || conv_id <= 0 || user_id <= 0 || row_id <= 0) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   /* The record whose question this is (read only: another turn may be
    * reading the history), or, when its prompt isn't applied yet, a note for
    * the apply to come. */
   struct session_prefix_turn *turn = NULL;
   for (struct session_prefix_turn *t = session->prefix_turn; t && !turn; t = t->next) {
      if (t->question && id_of(t->question) == row_id) {
         turn = t;
      }
   }
   /* One whose id waits for a running turn to end (a claimed exchange,
    * session_stamp_claimed): by identity. */
   for (struct session_prefix_turn *t = session->prefix_turn;
        t && !turn && session->claimed_user_row == row_id && session->claimed_user_msg;
        t = t->next) {
      if (t->question == session->claimed_user_msg) {
         turn = t;
      }
   }
   if (!turn) {
      session->prefix_saved.row_id = row_id;
      session->prefix_saved.conv_id = conv_id;
      session->prefix_saved.user_id = user_id;
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }
   turn->saved = true;
   turn->question_id = row_id;
   turn->conv_id = conv_id;
   turn->user_id = user_id;
   struct session_prefix_turn *ready = NULL;
   if (ready_locked(turn)) {
      give_up_older_locked(session, turn);
      unlink_locked(session, turn);
      ready = turn;
   }
   pthread_mutex_unlock(&session->history_mutex);
   if (ready) {
      save_turn(session, ready);
   }
}

struct session_prefix_turn *session_prefix_take_back_locked(session_t *session,
                                                            struct json_object *question) {
   if (!session || !question) {
      return NULL;
   }
   struct session_prefix_turn *turn = NULL;
   for (struct session_prefix_turn *t = session->prefix_turn; t && !turn; t = t->next) {
      if (t->question == question) {
         turn = t;
      }
   }
   /* A saved one went with its question (a retraction handles the rows). */
   if (!turn || turn->saved) {
      return NULL;
   }
   /* The question and its context go with the turn; what it announced to the
    * conversation stays in the history, and is saved now, in place, naming
    * no question. */
   session_release_ref_locked(session, turn->question);
   session_release_ref_locked(session, turn->context_msg);
   turn->question = turn->context_msg = NULL;
   if (turn->conv_id <= 0) {
      turn->conv_id = turn->hist == session->turn_history &&
                              turn->hist != session->conversation_history
                          ? session->turn_history_conv
                          : atomic_load(&session->history_conversation_id);
      turn->user_id = session_effective_user_id(session);
   }
   /* Not ready (never applied, or its conversation not known yet): it stays
    * queued with no question, and goes with its history
    * (session_prefix_release_locked) or a later question (give_up_older). */
   if (!ready_locked(turn)) {
      return NULL;
   }
   unlink_locked(session, turn);
   return turn;
}

void session_prefix_save_taken(session_t *session, struct session_prefix_turn *turn) {
   if (session && turn) {
      save_turn(session, turn);
   }
}

void session_prefix_release_locked(session_t *session) {
   if (!session) {
      return;
   }
   for (struct session_prefix_turn *t = session->prefix_turn; t;) {
      struct session_prefix_turn *next = t->next;
      /* The running turn's (its pinned history) is still its own. */
      if (!(session->turn_history && t->hist == session->turn_history)) {
         if (t->applied && !t->whole && !t->saved) {
            OLOG_INFO("Session %u: a turn's request context left unsaved with its history",
                      session->session_id);
         }
         unlink_locked(session, t);
         turn_release_locked(session, t);
      }
      t = next;
   }
   /* The note of a saved question stays: it names its row, which no other
    * question has (the running turn's apply may still need it). */
}

/* A withdrawal (session_withdraw_forgotten) as a live session applies it. */
typedef struct {
   const conv_withdrawn_t *w;
   bool memory_bodies;
   int remover; /* whose memory blocks go (with memory_bodies) */
} withdraw_ctx_t;

/* Caller holds history_mutex.  Keep @p wc for the session's next turn:
 * {"memory": bool, "items": [[conv_id, handle], ...], "ids": [item_id, ...]}. */
static void withdraw_later_locked(session_t *session, const withdraw_ctx_t *wc) {
   struct json_object *p = session->withdraw_pending;
   if (!p) {
      p = json_object_new_object();
      if (p) {
         json_object_object_add(p, "memory", json_object_new_boolean(0));
         json_object_object_add(p, "items", json_object_new_array());
         json_object_object_add(p, "ids", json_object_new_array());
      }
      session->withdraw_pending = p;
   }
   struct json_object *items = p ? json_object_object_get(p, "items") : NULL;
   struct json_object *ids = p ? json_object_object_get(p, "ids") : NULL;
   bool ok = items && ids;
   if (ok && wc->memory_bodies) {
      json_object_object_add(p, "memory", json_object_new_boolean(1));
   }
   /* Only the pairs of the conversations this session's histories hold. */
   const int64_t live_conv = atomic_load(&session->history_conversation_id);
   for (int i = 0; ok && i < wc->w->n_items; i++) {
      const int64_t conv = wc->w->items[i].conv_id;
      if (conv != live_conv && conv != session->turn_history_conv) {
         continue;
      }
      struct json_object *pair = json_object_new_array();
      ok = pair &&
           json_object_array_add(pair, json_object_new_int64(wc->w->items[i].conv_id)) == 0 &&
           json_object_array_add(pair, json_object_new_int(wc->w->items[i].handle)) == 0 &&
           json_object_array_add(items, pair) == 0;
   }
   for (int i = 0; ok && i < wc->w->n_item_ids; i++) {
      ok = json_object_array_add(ids, json_object_new_string(wc->w->item_ids[i])) == 0;
   }
   if (!ok) {
      OLOG_ERROR("Session %u: out of memory keeping a withdrawal for its next turn; its history "
                 "keeps the forgotten items until the next one",
                 session->session_id);
   }
}

/* Caller holds history_mutex.  The handles to withdraw from a history of
 * conversation @p conv: those the withdrawal names for it, and those the
 * session's own table gives items it names (a history no conversation stored
 * yet has only these).  Into @p *out (caller frees); returns how many, or -1
 * on allocation failure. */
static int handles_for_locked(const session_t *session,
                              int64_t conv,
                              const withdraw_ctx_t *wc,
                              int **out) {
   const int table = session->focus_handles ? session->focus_handles->count : 0;
   const int cap = wc->w->n_items + table;
   *out = NULL;
   if (cap == 0) {
      return 0;
   }
   int *handles = malloc((size_t)cap * sizeof(*handles));
   if (!handles) {
      return -1;
   }
   int n = 0;
   for (int i = 0; conv > 0 && i < wc->w->n_items; i++) {
      if (wc->w->items[i].conv_id == conv) {
         handles[n++] = wc->w->items[i].handle;
      }
   }
   n += focus_handles_withdrawn_locked(session, conv, (const char *const *)wc->w->item_ids,
                                       wc->w->n_item_ids, handles + n, cap - n);
   *out = handles;
   return n;
}

/* Caller holds history_mutex.  Withdraw from @p hist (conversation @p conv)
 * its item lines the withdrawal names and, with it, its memory blocks.
 * Returns whether anything changed. */
static bool withdraw_hist_locked(const session_t *session,
                                 struct json_object *hist,
                                 int64_t conv,
                                 const withdraw_ctx_t *wc) {
   int *handles = NULL;
   const int nh = handles_for_locked(session, conv, wc, &handles);
   if (nh < 0) {
      OLOG_ERROR("Session %u: out of memory withdrawing forgotten items", session->session_id);
      return false;
   }
   if (nh == 0 && !wc->memory_bodies) {
      free(handles);
      return false;
   }
   bool any = false;
   const size_t n = hist ? json_object_array_length(hist) : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *content = NULL;
      json_object_object_get_ex(json_object_array_get_idx(hist, i), "content", &content);
      const size_t np = json_object_is_type(content, json_type_array)
                            ? json_object_array_length(content)
                            : 0;
      for (size_t k = 0; k < np; k++) {
         struct json_object *part = json_object_array_get_idx(content, k);
         const message_kind_t kind = llm_history_kind_of(part);
         struct json_object *text = NULL;
         if (!(kind == MESSAGE_KIND_TURN_CONTEXT && nh > 0) &&
             !(kind == MESSAGE_KIND_MEMORY && wc->memory_bodies)) {
            continue;
         }
         if (!json_object_object_get_ex(part, "text", &text)) {
            continue;
         }
         char *now = NULL;
         if (kind == MESSAGE_KIND_MEMORY) {
            bool changed = false;
            now = llm_context_withdraw_body(json_object_get_string(text), &changed);
            if (now && !changed) {
               free(now);
               now = NULL;
            }
         } else if (llm_context_withdraw_items(json_object_get_string(text), handles, nh, &now) !=
                    0) {
            OLOG_ERROR("Session %u: out of memory withdrawing forgotten items",
                       session->session_id);
         }
         if (now) {
            json_object_object_add(part, "text", json_object_new_string(now));
            any = true;
            free(now);
         }
      }
   }
   free(handles);
   if (any) {
      /* It no longer reads as its turns were sent: their reasoning stays behind. */
      (void)llm_history_drop_turn_blocks(hist);
   }
   return any;
}

/* Caller holds history_mutex.  Apply @p wc to the session's histories.
 * Returns whether anything changed. */
static bool withdraw_now_locked(session_t *session, const withdraw_ctx_t *wc) {
   const int64_t live_conv = atomic_load(&session->history_conversation_id);
   bool any = withdraw_hist_locked(session, session->conversation_history, live_conv, wc);
   if (session->turn_history && session->turn_history != session->conversation_history) {
      any = withdraw_hist_locked(session, session->turn_history, session->turn_history_conv, wc) ||
            any;
   }
   if (any) {
      OLOG_INFO("Session %u: forgotten items withdrawn from its history (boundary)",
                session->session_id);
   }
   return any;
}

static void withdraw_in_session(session_t *session, void *ctx) {
   /* The items leave every history that holds them (any session's own
    * handles say so); the memory blocks are the remover's own. */
   const withdraw_ctx_t *all = ctx;
   const withdraw_ctx_t mine = {
      .w = all->w,
      .memory_bodies = all->memory_bodies && session_effective_user_id(session) == all->remover,
      .remover = all->remover
   };
   const withdraw_ctx_t *wc = &mine;
   pthread_mutex_lock(&session->history_mutex);
   if (session_turn_reads_elsewhere_locked(session)) {
      withdraw_later_locked(session, wc); /* the running turn reads it unlocked */
   } else {
      (void)withdraw_now_locked(session, wc);
   }
   pthread_mutex_unlock(&session->history_mutex);
}

/* Caller holds history_mutex, as the turn's owner (or with no turn running).
 * Apply a withdrawal kept for this turn.  Returns whether anything changed
 * (its rows saved since the withdrawal then replay without their reasoning: a
 * boundary). */
static bool withdraw_pending_locked(session_t *session) {
   struct json_object *p = session->withdraw_pending;
   if (!p) {
      return false;
   }
   struct json_object *items = json_object_object_get(p, "items");
   struct json_object *ids = json_object_object_get(p, "ids");
   const int n = items ? (int)json_object_array_length(items) : 0;
   const int n_ids = ids ? (int)json_object_array_length(ids) : 0;
   conv_withdrawn_item_t *list = n ? calloc((size_t)n, sizeof(*list)) : NULL;
   char **id_list = n_ids ? calloc((size_t)n_ids, sizeof(*id_list)) : NULL;
   if ((n && !list) || (n_ids && !id_list)) {
      /* Kept for the next boundary rather than lost. */
      OLOG_ERROR("Session %u: out of memory applying a kept withdrawal; kept for later",
                 session->session_id);
      free(list);
      free(id_list);
      return false;
   }
   session->withdraw_pending = NULL;
   for (int i = 0; i < n; i++) {
      struct json_object *pair = json_object_array_get_idx(items, i);
      list[i].conv_id = json_object_get_int64(json_object_array_get_idx(pair, 0));
      list[i].handle = json_object_get_int(json_object_array_get_idx(pair, 1));
   }
   for (int i = 0; i < n_ids; i++) {
      id_list[i] = (char *)json_object_get_string(json_object_array_get_idx(ids, i));
   }
   /* Kept from several withdrawals: sorted again for the lookup. */
   if (n_ids > 1) {
      qsort(id_list, (size_t)n_ids, sizeof(*id_list), cmp_str);
   }
   const conv_withdrawn_t w = { .items = list,
                                .n_items = n,
                                .item_ids = id_list,
                                .n_item_ids = n_ids };
   const withdraw_ctx_t wc = { .w = &w,
                               .memory_bodies = json_object_get_boolean(
                                   json_object_object_get(p, "memory")) };
   const bool any = withdraw_now_locked(session, &wc);
   free(list);
   free(id_list);
   json_object_put(p);
   return any;
}

void session_prefix_voice_save_locked(session_t *session) {
   if (session) {
      (void)withdraw_pending_locked(session);
   }
}

/* Withdrawals waiting for the one worker that runs them (at most one thread,
 * however many forgettings come in): one entry per user, merged. */
#define WITHDRAW_QUEUE_MAX 32
static pthread_mutex_t s_withdraw_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
   int user_id;
   bool memory_bodies;
} s_withdraw_queue[WITHDRAW_QUEUE_MAX];
static int s_withdraw_count;
static bool s_withdraw_running;

static void *withdraw_thread(void *arg) {
   (void)arg;
   for (;;) {
      pthread_mutex_lock(&s_withdraw_mutex);
      if (s_withdraw_count == 0) {
         s_withdraw_running = false;
         pthread_mutex_unlock(&s_withdraw_mutex);
         return NULL;
      }
      const int user_id = s_withdraw_queue[0].user_id;
      const bool memory_bodies = s_withdraw_queue[0].memory_bodies;
      memmove(&s_withdraw_queue[0], &s_withdraw_queue[1],
              (size_t)(s_withdraw_count - 1) * sizeof(s_withdraw_queue[0]));
      s_withdraw_count--;
      pthread_mutex_unlock(&s_withdraw_mutex);
      (void)session_withdraw_forgotten(user_id, memory_bodies);
   }
}

void session_withdraw_forgotten_async(int user_id, bool memory_bodies) {
   pthread_mutex_lock(&s_withdraw_mutex);
   int at = -1;
   for (int i = 0; i < s_withdraw_count && at < 0; i++) {
      if (s_withdraw_queue[i].user_id == user_id) {
         at = i;
      }
   }
   if (at < 0 && s_withdraw_count < WITHDRAW_QUEUE_MAX) {
      at = s_withdraw_count++;
      s_withdraw_queue[at].user_id = user_id;
      s_withdraw_queue[at].memory_bodies = false;
   }
   const bool queued = at >= 0;
   if (queued) {
      s_withdraw_queue[at].memory_bodies = s_withdraw_queue[at].memory_bodies || memory_bodies;
   }
   bool start = queued && !s_withdraw_running;
   if (start) {
      s_withdraw_running = true;
   }
   pthread_mutex_unlock(&s_withdraw_mutex);

   if (!queued) {
      /* A full queue (as many users forgetting at once): here, not dropped. */
      OLOG_WARNING("withdraw: queue full; withdrawing user %d's here", user_id);
      (void)session_withdraw_forgotten(user_id, memory_bodies);
      return;
   }
   if (start) {
      pthread_t t;
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      const int rc = pthread_create(&t, &attr, withdraw_thread, NULL);
      pthread_attr_destroy(&attr);
      if (rc != 0) {
         OLOG_WARNING("withdraw: no worker thread; withdrawing here");
         withdraw_thread(NULL);
      }
   }
}

int session_withdraw_forgotten(int user_id, bool memory_bodies) {
   conv_withdrawn_t w;
   if (conv_db_withdraw(user_id, memory_bodies, &w) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   /* Every live session: a shared document's passage can sit in anyone's,
    * saved or not yet. */
   const withdraw_ctx_t wc = { .w = &w, .memory_bodies = memory_bodies, .remover = user_id };
   session_manager_for_each_user_session(0, withdraw_in_session, (void *)&wc);
   conv_withdrawn_free(&w);
   return SUCCESS;
}
