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
 * The coding_agent tool writing on a GitHub pull request, as the user and on
 * their yes to a preview DAWN renders: replies on its review threads (which
 * then resolve them), and asking Copilot to review it again.  The threads
 * the replies answer are GitHub's text, so the preview frames them as web
 * content; the replies themselves are shown whole, signed as the assistant.
 */

#include <ctype.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/pending_slots.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "core/tool_call_challenge.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "llm/llm_context_text.h"
#include "llm/llm_third_party.h"
#include "tools/coding_agent_gateway.h"
#include "tools/coding_agent_internal.h"
#include "tools/tool_pending.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

#define CG_PENDING_REPLY 1
#define CG_PENDING_REVIEW 2
#define CG_PENDING_TTL_SEC 600
#define CG_CONFIRM_TURNS 3 /* turns after the preview that may confirm it */
#define CG_REPLY_SLOTS 4   /* staged replies, every session (each holds up to 20) */
#define CG_REVIEW_SLOTS 8  /* staged Copilot requests */
/* "\n\n\xe2\x80\x94 " (six bytes: two newlines, an em dash, a space), then the name
 * (CONFIG_NAME_MAX with its NUL); two to spare. */
#define CG_SIGNATURE_MAX (CONFIG_NAME_MAX + 8)
#define CG_PR_MAX 256          /* "owner/repo#N" */
#define CG_LOGIN_MAX 48        /* a GitHub login (the gateway cuts them at 40) */
#define CG_PATH_SHOWN 160      /* a thread's path in a preview */
#define CG_OPENING_SHOWN 120   /* a thread's first comment, its opening, in a preview */
#define CG_DESCRIBE_EXCERPT 90 /* a reply's text in a reply-code line */
#define CG_PREVIEW_ROOM 1024   /* a preview's own words around what it shows */
#define CG_EXCERPT_MARK 48     /* str_excerpt_line's "... (N more characters)" */
#define CG_ID_DIGITS_MAX 18    /* a comment_id written as digits (fits an int64) */
#define CG_LINE_SUFFIX_MAX 16  /* ":<line>" after a path */
/* A reply round's pr_reply call: GitHub's reads, then each reply about a second apart. */
#define CG_REPLY_CALL_BASE_MS 10000
#define CG_REPLY_CALL_EACH_MS 3000

typedef struct {
   int64_t comment_id;
   int line; /* 0: none */
   bool outdated;
   bool resolve;          /* resolve the thread after the reply */
   bool copilot_resolved; /* resolved by Copilot already: left as it is */
   char path[CG_PATH_SHOWN + CG_EXCERPT_MARK];
   char author[CG_LOGIN_MAX + CG_EXCERPT_MARK];
   char opening[CG_OPENING_SHOWN + CG_EXCERPT_MARK];
   char body[CODING_GITHUB_REPLY_BODY_MAX + CG_SIGNATURE_MAX]; /* as it will be posted, signed */
} cg_reply_t;

typedef struct {
   pending_slot_t hdr;
   char pr[CG_PR_MAX];
   char as_login[CG_LOGIN_MAX + CG_EXCERPT_MARK];
   int count;
   cg_reply_t items[CODING_GITHUB_REPLIES_MAX];
} cg_replies_pending_t;

typedef struct {
   pending_slot_t hdr;
   char pr[CG_PR_MAX];
} cg_review_pending_t;

PENDING_ITEM_CHECK(cg_replies_pending_t);
PENDING_ITEM_CHECK(cg_review_pending_t);
_Static_assert(CG_CONFIRM_TURNS <= TURN_ORIGIN_UNASKED_QUIET_TURNS,
               "the quiet window must cover the confirm window");
static cg_replies_pending_t s_replies[CG_REPLY_SLOTS];
static cg_review_pending_t s_reviews[CG_REVIEW_SLOTS];
static pthread_mutex_t s_pending_mutex = PTHREAD_MUTEX_INITIALIZER; /* both tables */
PENDING_ARRAY_CHECK(s_replies);
PENDING_ARRAY_CHECK(s_reviews);
static const pending_slots_t s_reply_slots = {
   .base = s_replies,
   .stride = sizeof(s_replies[0]),
   .count = CG_REPLY_SLOTS,
   .ttl_sec = CG_PENDING_TTL_SEC,
   .later_turns = CG_CONFIRM_TURNS,
};
static const pending_slots_t s_review_slots = {
   .base = s_reviews,
   .stride = sizeof(s_reviews[0]),
   .count = CG_REVIEW_SLOTS,
   .ttl_sec = CG_PENDING_TTL_SEC,
   .later_turns = CG_CONFIRM_TURNS,
};

/* "owner/repo#N" of the pull request a gateway answer's "pr" names, or "". */
/* @p sb's text, NULL when it didn't fit in memory (what was built is freed). */
static char *taken(strbuf_t *sb) {
   char *out = strbuf_steal(sb);
   strbuf_free(sb);
   return out;
}

static void pr_name(struct json_object *ans, char *out, size_t len) {
   struct json_object *pr = json_object_object_get(ans, "pr");
   if (snprintf(out, len, "%s/%s#%s", coding_gateway_jstr(pr, "owner"),
                coding_gateway_jstr(pr, "repo"), coding_gateway_jstr(pr, "number")) >= (int)len ||
       coding_gateway_jstr(pr, "number")[0] == '\0') {
      out[0] = '\0';
   }
}

/* The line a reply is signed with ("" when replies aren't signed). */
static void signature(char *out, size_t len) {
   out[0] = '\0';
   if (!coding_agent_sign_replies() || g_config.general.ai_name[0] == '\0') {
      return;
   }
   char name[CONFIG_NAME_MAX];
   safe_strscpy(name, g_config.general.ai_name);
   name[0] = (char)toupper((unsigned char)name[0]);
   snprintf(out, len, "\n\n\xe2\x80\x94 %s", name); /* an em dash */
}

static bool jtrue(struct json_object *o, const char *key) {
   return json_object_get_boolean(json_object_object_get(o, key));
}

/* The thread of @p threads whose first comment is @p comment_id, or NULL; a
 * thread someone other than Copilot resolved is NULL too: replies don't reopen
 * a discussion a person closed. */
static struct json_object *find_thread(struct json_object *threads, int64_t comment_id) {
   const size_t n = json_object_is_type(threads, json_type_array)
                        ? json_object_array_length(threads)
                        : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *t = json_object_array_get_idx(threads, i);
      if (json_object_get_int64(json_object_object_get(t, "comment_id")) == comment_id) {
         return jtrue(t, "resolved") && !jtrue(t, "resolved_by_copilot") ? NULL : t;
      }
   }
   return NULL;
}

/* Whether the commits the replies name may not be on the pull request yet:
 * the job that published it has commits since, or the pull request's head
 * isn't the one it published. */
static bool fixes_unpublished(int user_id, struct json_object *ans) {
   const char *job = coding_gateway_jstr(ans, "job");
   if (job[0] == '\0') {
      return false;
   }
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "client_ref", json_object_new_string(job));
   struct json_object *list = NULL;
   char *err = NULL;
   bool unpublished = false;
   if (coding_gateway_call(user_id, "task_list", args, &list, &err) == CODING_GW_OK) {
      struct json_object *left = json_object_object_get(list, "unpublished_commits");
      const char *head = coding_gateway_jstr(
          json_object_object_get(json_object_object_get(list, "task"), "published"), "head");
      const char *pr_head = coding_gateway_jstr(ans, "head_sha");
      unpublished = (json_object_is_type(left, json_type_int) && json_object_get_int(left) > 0) ||
                    (head[0] != '\0' && pr_head[0] != '\0' && strcmp(head, pr_head) != 0);
   }
   json_object_put(list);
   free(err);
   return unpublished;
}

/* A reply's comment_id: a number, or one written as digits; 0 for neither. */
static int64_t reply_comment_id(struct json_object *reply) {
   struct json_object *cid = json_object_object_get(reply, "comment_id");
   if (json_object_is_type(cid, json_type_int)) {
      return json_object_get_int64(cid) > 0 ? json_object_get_int64(cid) : 0;
   }
   const char *s = json_object_is_type(cid, json_type_string) ? json_object_get_string(cid) : "";
   if (s[0] == '\0' || s[0] == '0' || strspn(s, "0123456789") != strlen(s) ||
       strlen(s) > CG_ID_DIGITS_MAX) {
      return 0;
   }
   return strtoll(s, NULL, 10);
}

/* The replies a call asks for, checked: 1 to CODING_GITHUB_REPLIES_MAX, each naming a
 * different thread with a reply that says something and isn't too long.  NULL,
 * else the error result. */
static char *check_replies(struct json_object *replies) {
   const size_t n = json_object_is_type(replies, json_type_array)
                        ? json_object_array_length(replies)
                        : 0;
   if (n == 0 || n > CODING_GITHUB_REPLIES_MAX) {
      return coding_agent_error(
          "reply needs the pull request ({\"ref\": ...} or {\"job\": N}) and \"replies\": 1 "
          "to " CA_STR(CODING_GITHUB_REPLIES_MAX) " of {\"comment_id\": <the "
                                                  "thread's comment_id, as pr_threads "
                                                  "lists it>, \"body\": <the reply>}.");
   }
   for (size_t i = 0; i < n; i++) {
      struct json_object *r = json_object_array_get_idx(replies, i);
      const int64_t cid = reply_comment_id(r);
      const char *body = coding_gateway_jstr(r, "body");
      if (cid == 0) {
         return coding_agent_error("Each reply names its thread by comment_id, a number, as "
                                   "pr_threads lists it.");
      }
      for (size_t j = 0; j < i; j++) {
         if (reply_comment_id(json_object_array_get_idx(replies, j)) == cid) {
            return coding_agent_error("Two replies name the same thread; give each thread one.");
         }
      }
      if (!coding_agent_has_text(body)) {
         return coding_agent_error("A reply has no text.");
      }
      bool ignored;
      if (!coding_agent_yes_no(r, "resolve", true, &ignored)) {
         return coding_agent_error("A reply's resolve is true or false.");
      }
      if (strlen(body) > CODING_GITHUB_REPLY_BODY_MAX) {
         strbuf_t sb;
         strbuf_init(&sb, 64);
         strbuf_appendf(&sb,
                        "Reply %zu is over " CA_STR(
                            CODING_GITHUB_REPLY_BODY_MAX) " bytes; say it more briefly.",
                        i + 1);
         return coding_agent_error_free(taken(&sb));
      }
   }
   return NULL;
}

/* "src/x.c:42" (or the path alone), and " (outdated)". */
static void where_line(strbuf_t *sb, const cg_reply_t *r) {
   strbuf_append(sb, r->path);
   if (r->line > 0) {
      strbuf_appendf(sb, ":%d", r->line);
   }
   if (r->outdated) {
      strbuf_append(sb, " (outdated)");
   }
}

/* The threads the replies answer, GitHub's text: neutralized and framed. */
static char *threads_framed(const cg_replies_pending_t *p) {
   strbuf_t sb;
   strbuf_init(&sb, 1024);
   for (int i = 0; i < p->count; i++) {
      const cg_reply_t *r = &p->items[i];
      strbuf_appendf(&sb, "%d) ", i + 1);
      where_line(&sb, r);
      strbuf_appendf(&sb, " (%s): %s\n", r->author, r->opening);
   }
   char *body = llm_context_neutralize_owned(taken(&sb));
   char *framed = body ? llm_third_party_frame(TOOL_FRAME_WEB, NULL, body) : NULL;
   free(body);
   return framed;
}

/* Fill reply @p r from @p reply (the model's; its resolve, else @p resolve) and @p t
 * (GitHub's thread). */
static void fill_reply(cg_reply_t *r,
                       struct json_object *reply,
                       struct json_object *t,
                       const char *sig,
                       bool resolve) {
   r->comment_id = reply_comment_id(reply);
   (void)coding_agent_yes_no(reply, "resolve", resolve, &r->resolve); /* checked by check_replies */
   r->copilot_resolved = jtrue(t, "resolved_by_copilot");
   r->resolve = r->resolve && !r->copilot_resolved;
   r->line = json_object_get_int(json_object_object_get(t, "line"));
   r->outdated = jtrue(t, "outdated");
   str_excerpt_line(coding_gateway_jstr(t, "path"), CG_PATH_SHOWN, r->path, sizeof(r->path));
   struct json_object *first = json_object_array_get_idx(json_object_object_get(t, "comments"), 0);
   str_excerpt_line(coding_gateway_jstr(first, "author"), CG_LOGIN_MAX, r->author,
                    sizeof(r->author));
   str_excerpt_line(coding_gateway_jstr(first, "body"), CG_OPENING_SHOWN, r->opening,
                    sizeof(r->opening));
   snprintf(r->body, sizeof(r->body), "%s%s", coding_gateway_jstr(reply, "body"), sig);
}

char *coding_github_reply(struct json_object *details, int user_id, const turn_origin_t *origin) {
   struct json_object *replies = json_object_object_get(details, "replies");
   char *bad = check_replies(replies);
   if (bad != NULL) {
      return bad;
   }
   bool resolve; /* for replies that don't say */
   if (!coding_agent_yes_no(details, "resolve", true, &resolve)) {
      return coding_agent_error("resolve is true or false.");
   }
   char *refusal = NULL;
   struct json_object *args = coding_github_pr_args(details, user_id, &refusal);
   if (args != NULL) { /* resolved too: one Copilot resolved takes a reply */
      json_object_object_add(args, "all", json_object_new_boolean(true));
   }
   struct json_object *ans = args ? coding_github_call(user_id, "pr_threads", args, &refusal)
                                  : NULL;
   if (ans == NULL) {
      return refusal;
   }
   coding_agent_mark_read(); /* the threads are GitHub's text */
   char pr[CG_PR_MAX];
   pr_name(ans, pr, sizeof(pr));
   struct json_object *threads = json_object_object_get(ans, "threads");
   const size_t n = json_object_array_length(replies);
   strbuf_t unknown;
   strbuf_init(&unknown, 64);
   for (size_t i = 0; i < n; i++) {
      const int64_t cid = reply_comment_id(json_object_array_get_idx(replies, i));
      if (find_thread(threads, cid) == NULL) {
         strbuf_appendf(&unknown, "%s%lld", strbuf_len(&unknown) ? ", " : "", (long long)cid);
      }
   }
   if (pr[0] == '\0' || strbuf_len(&unknown) > 0) {
      strbuf_t sb;
      strbuf_init(&sb, 256);
      strbuf_appendf(&sb,
                     "comment_id %s doesn't start an unresolved review thread on %s, or one "
                     "Copilot resolved; reply only on threads pr_threads lists, by their "
                     "comment_id.",
                     strbuf_len(&unknown) ? strbuf_str(&unknown) : "?", pr[0] ? pr : "it");
      strbuf_free(&unknown);
      json_object_put(ans);
      return coding_agent_error_free(taken(&sb));
   }
   strbuf_free(&unknown);
   const bool unpublished = fixes_unpublished(user_id, ans);
   char sig[CG_SIGNATURE_MAX];
   signature(sig, sizeof(sig));

   cg_replies_pending_t *staged = calloc(1, sizeof(*staged)); /* the preview's copy */
   if (staged == NULL) {
      json_object_put(ans);
      return coding_agent_error("Out of memory.");
   }
   safe_strscpy(staged->pr, pr);
   str_excerpt_line(coding_gateway_jstr(ans, "as_login"), CG_LOGIN_MAX, staged->as_login,
                    sizeof(staged->as_login));
   staged->count = (int)n;
   for (size_t i = 0; i < n; i++) {
      struct json_object *reply = json_object_array_get_idx(replies, i);
      fill_reply(&staged->items[i], reply, find_thread(threads, reply_comment_id(reply)), sig,
                 resolve);
   }
   json_object_put(ans);
   char *framed = threads_framed(staged);
   if (framed == NULL) {
      free(staged);
      return coding_agent_error("The reply preview couldn't be built; nothing was staged.");
   }

   pending_stage_rc_t rc = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   cg_replies_pending_t *p = (cg_replies_pending_t *)pending_slots_stage(&s_reply_slots, origin,
                                                                         user_id, CG_PENDING_REPLY,
                                                                         pending_slots_now(), &rc);
   const uint32_t id = p ? p->hdr.item_id : 0;
   const uint32_t replaced = p ? p->hdr.replaced_turn : 0;
   if (p != NULL) {
      const pending_slot_t hdr = p->hdr;
      *p = *staged;
      p->hdr = hdr;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   coding_agent_preview_replaced(replaced);
   if (id == 0) {
      free(framed);
      free(staged);
      return tool_pending_stage_refusal(rc, "reply batch");
   }

   strbuf_t sb;
   strbuf_init(&sb, CG_PREVIEW_ROOM + strlen(framed));
   strbuf_appendf(&sb, "About to post %d repl%s on %s as %s.\nThe threads they answer:\n%s\n",
                  staged->count, staged->count == 1 ? "y" : "ies", staged->pr,
                  staged->as_login[0] ? staged->as_login : "the gateway's GitHub user", framed);
   free(framed);
   strbuf_append(&sb, "The replies, as they will be posted:\n");
   for (int i = 0; i < staged->count; i++) {
      strbuf_appendf(&sb, "Reply %d (%s):\n%s\n", i + 1,
                     staged->items[i].copilot_resolved ? "its thread is already resolved by Copilot"
                     : staged->items[i].resolve        ? "then resolve its thread"
                                                       : "its thread stays open",
                     staged->items[i].body);
   }
   if (unpublished) {
      strbuf_append(&sb, "Warning: the fixes aren't all on the pull request yet: propose the "
                         "publish too, so the commits these replies name are there; one confirm "
                         "can name both.\n");
   }
   strbuf_appendf(&sb,
                  "Read the replies to the user and ask them to confirm; if they say yes in "
                  "their reply, call coding_agent with action 'confirm' and arguments "
                  "{\"ids\": [%u]}.",
                  (unsigned)id);
   free(staged);
   return taken(&sb);
}

/* pr_reply's arguments for the staged replies. */
static struct json_object *reply_args(const cg_replies_pending_t *p) {
   struct json_object *args = json_object_new_object();
   struct json_object *list = json_object_new_array();
   for (int i = 0; i < p->count; i++) {
      struct json_object *r = json_object_new_object();
      json_object_object_add(r, "comment_id", json_object_new_int64(p->items[i].comment_id));
      json_object_object_add(r, "body", json_object_new_string(p->items[i].body));
      json_object_object_add(r, "resolve", json_object_new_boolean(p->items[i].resolve));
      json_object_array_add(list, r);
   }
   json_object_object_add(args, "ref", json_object_new_string(p->pr));
   json_object_object_add(args, "replies", list);
   return args;
}

/* What pr_reply did, reply by reply. */
static char *replies_result(const cg_replies_pending_t *p,
                            struct json_object *ans,
                            bool *complete) {
   struct json_object *results = json_object_object_get(ans, "results");
   const size_t n = json_object_is_type(results, json_type_array)
                        ? json_object_array_length(results)
                        : 0;
   int posted = 0, resolved = 0, asked = 0;
   strbuf_t lines;
   strbuf_init(&lines, 512);
   for (size_t i = 0; i < n; i++) {
      struct json_object *r = json_object_array_get_idx(results, i);
      const char *error = coding_gateway_jstr(r, "error");
      strbuf_appendf(&lines, "%zu) ", i + 1);
      if (error[0] != '\0') {
         strbuf_appendf(&lines, "not posted: %s\n", error);
         continue;
      }
      posted++;
      const char *url = coding_gateway_jstr(r, "url");
      strbuf_append(&lines, url[0] ? url : "posted");
      if (json_object_get_boolean(json_object_object_get(r, "already_posted"))) {
         strbuf_append(&lines, " (it was there already)");
      }
      const bool resolve = (int)i < p->count && p->items[i].resolve;
      asked += resolve;
      if (json_object_get_boolean(json_object_object_get(r, "resolved"))) {
         resolved++;
      } else if (resolve) {
         const char *why = coding_gateway_jstr(r, "resolve_error");
         strbuf_appendf(&lines, " (not resolved%s%s)", why[0] ? ": " : "", why);
      }
      strbuf_append(&lines, "\n");
   }
   strbuf_t sb;
   strbuf_init(&sb, 256 + strbuf_len(&lines));
   strbuf_appendf(&sb, "Posted %d of %d repl%s on %s", posted, p->count,
                  p->count == 1 ? "y" : "ies", p->pr);
   if (asked > 0) {
      strbuf_appendf(&sb, " and resolved %d thread%s", resolved, resolved == 1 ? "" : "s");
   }
   strbuf_appendf(&sb, ":\n%s", strbuf_str(&lines));
   strbuf_free(&lines);
   /* A reply not posted leaves the batch unfinished (a confirm stops there); an
    * error result only when none was, since one that posted did something. */
   *complete = posted == p->count;
   return posted == 0 ? coding_agent_error_free(taken(&sb)) : taken(&sb);
}

char *coding_github_confirm_reply(uint32_t id,
                                  int user_id,
                                  const turn_origin_t *origin,
                                  bool *complete) {
   bool done_all = false;
   if (complete == NULL) {
      complete = &done_all;
   }
   *complete = false;
   cg_replies_pending_t *p = malloc(sizeof(*p));
   if (p == NULL) {
      return coding_agent_error("Out of memory.");
   }
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   pthread_mutex_lock(&s_pending_mutex);
   const pending_find_rc_t rc = pending_slots_take(&s_reply_slots, origin, user_id,
                                                   CG_PENDING_REPLY, id, pending_slots_now(), p,
                                                   sizeof(*p), &orc);
   pthread_mutex_unlock(&s_pending_mutex);
   if (rc != PENDING_FOUND) {
      free(p);
      return tool_pending_take_refusal(rc, orc, "reply batch");
   }
   turn_origin_note_answered(session_get_command_context(), p->hdr.origin.turn_number);
   /* Once more when no answer came: the gateway posts each reply only once.  The
    * call may take a while: GitHub's reads, then each reply about a second apart. */
   const long timeout_ms = CG_REPLY_CALL_BASE_MS + CG_REPLY_CALL_EACH_MS * (long)p->count;
   struct json_object *ans = NULL;
   char *err = NULL;
   int sent = CODING_GW_FAILED;
   for (int attempt = 0; attempt < 2 && sent == CODING_GW_FAILED; attempt++) {
      free(err);
      err = NULL;
      sent = coding_gateway_call_timed(user_id, "pr_reply", reply_args(p), timeout_ms, &ans, &err);
   }
   char *out = NULL;
   if (sent == CODING_GW_OK) {
      out = replies_result(p, ans, complete);
      free(err);
   } else if (sent == CODING_GW_FAILED) { /* the first try may have posted them */
      strbuf_t sb;
      strbuf_init(&sb, 256);
      strbuf_appendf(&sb,
                     "%s Whether the replies on %s were posted isn't known; pr_threads "
                     "shows them.",
                     err ? err : "The coding-agent gateway didn't answer.", p->pr);
      free(err);
      out = coding_agent_error_free(taken(&sb));
   } else {
      out = coding_agent_error_free(err);
   }
   json_object_put(ans);
   free(p);
   return out;
}

char *coding_github_request_review(struct json_object *details,
                                   int user_id,
                                   const turn_origin_t *origin) {
   char *refusal = NULL;
   struct json_object *args = coding_github_pr_args(details, user_id, &refusal);
   if (args != NULL) {
      json_object_object_add(args, "check", json_object_new_boolean(true));
   }
   struct json_object *ans = args ? coding_github_call(user_id, "pr_request_review", args, &refusal)
                                  : NULL;
   if (ans == NULL) {
      return refusal;
   }
   char pr[CG_PR_MAX];
   pr_name(ans, pr, sizeof(pr));
   struct json_object *jnew = json_object_object_get(ans, "new_commits");
   const int fresh = json_object_is_type(jnew, json_type_int) ? json_object_get_int(jnew) : -1;
   struct json_object *round = json_object_object_get(ans, "copilot_round");
   const bool reviewed = json_object_is_type(round, json_type_object);
   /* No new commits: the fixes may be waiting on a publish, or there are none. */
   const bool unpublished = fresh == 0 && fixes_unpublished(user_id, ans);
   /* Its re-review found nothing new: it had threads, and each repeats an answered one. */
   const bool only_repeats = json_object_get_boolean(json_object_object_get(round, "rereview")) &&
                             json_object_get_int(json_object_object_get(round, "new")) == 0 &&
                             json_object_get_int(json_object_object_get(round, "repeats")) > 0;
   json_object_put(ans);
   if (pr[0] == '\0') {
      return coding_agent_error("The gateway didn't say which pull request it is.");
   }
   pending_stage_rc_t rc = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   cg_review_pending_t *p = (cg_review_pending_t *)pending_slots_stage(&s_review_slots, origin,
                                                                       user_id, CG_PENDING_REVIEW,
                                                                       pending_slots_now(), &rc);
   const uint32_t id = p ? p->hdr.item_id : 0;
   const uint32_t replaced = p ? p->hdr.replaced_turn : 0;
   if (p != NULL) {
      safe_strscpy(p->pr, pr);
   }
   pthread_mutex_unlock(&s_pending_mutex);
   coding_agent_preview_replaced(replaced);
   if (id == 0) {
      return tool_pending_stage_refusal(rc, "Copilot review request");
   }
   strbuf_t sb;
   strbuf_init(&sb, 768);
   strbuf_appendf(&sb, "About to ask Copilot to review %s (", pr);
   if (unpublished) {
      strbuf_append(&sb, "after the publish: its fixes aren't on the pull request yet; propose "
                         "the publish too if it isn't waiting, so one confirm names both");
   } else if (fresh == 0) {
      strbuf_append(&sb, "no new commits since Copilot's last review, merges of the base aside; "
                         "likely nothing new");
      if (only_repeats) {
         strbuf_append(&sb, "; its last re-review only repeated answered threads");
      }
   } else if (reviewed && fresh >= 0) {
      strbuf_appendf(&sb, "%d new commit%s since Copilot's last review", fresh,
                     fresh == 1 ? "" : "s");
   } else {
      strbuf_append(&sb, reviewed ? "new commits since Copilot's last review"
                                  : "Copilot hasn't reviewed it yet");
   }
   strbuf_append(&sb, "). Its review is posted on the pull request and uses the account's "
                      "Copilot review quota. Read this to the user and ask them to confirm; ");
   strbuf_appendf(&sb,
                  "if they say yes in their reply, call coding_agent with action 'confirm' and "
                  "arguments {\"ids\": [%u%s]}%s.",
                  (unsigned)id, unpublished ? ", <the publish's id>" : "",
                  unpublished ? " (the publish runs first, in the same confirm)" : "");
   return taken(&sb);
}

char *coding_github_confirm_request_review(uint32_t id, int user_id, const turn_origin_t *origin) {
   cg_review_pending_t p;
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   pthread_mutex_lock(&s_pending_mutex);
   const pending_find_rc_t rc = pending_slots_take(&s_review_slots, origin, user_id,
                                                   CG_PENDING_REVIEW, id, pending_slots_now(), &p,
                                                   sizeof(p), &orc);
   pthread_mutex_unlock(&s_pending_mutex);
   if (rc != PENDING_FOUND) {
      return tool_pending_take_refusal(rc, orc, "Copilot review request");
   }
   turn_origin_note_answered(session_get_command_context(), p.hdr.origin.turn_number);
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "ref", json_object_new_string(p.pr));
   struct json_object *ans = NULL;
   char *err = NULL;
   /* Not sent twice on no answer: a second would find the first pending. */
   const int sent = coding_gateway_call(user_id, "pr_request_review", args, &ans, &err);
   if (sent != CODING_GW_OK) {
      json_object_put(ans);
      if (sent == CODING_GW_FAILED) {
         free(err);
         strbuf_t sb;
         strbuf_init(&sb, 256);
         strbuf_appendf(&sb,
                        "The gateway didn't answer, so whether Copilot was asked to review %s "
                        "isn't known; pr_status shows whether its review is requested.",
                        p.pr);
         return coding_agent_error_free(taken(&sb));
      }
      return coding_agent_error_free(err);
   }
   free(err);
   json_object_put(ans);
   strbuf_t sb;
   strbuf_init(&sb, 256);
   strbuf_appendf(&sb,
                  "GitHub accepted the request: Copilot will review %s, usually within a few "
                  "minutes. Read its threads with pr_threads once it has reviewed.",
                  p.pr);
   return taken(&sb);
}

/* A reply-code line for the staged replies: each one's opening. */
static int describe_replies(const cg_replies_pending_t *p, char *out, size_t out_len) {
   strbuf_t sb;
   strbuf_init(&sb, 512);
   strbuf_appendf(&sb, "post %d repl%s on %s as %s:", p->count, p->count == 1 ? "y" : "ies", p->pr,
                  p->as_login[0] ? p->as_login : "the gateway's GitHub user");
   for (int i = 0; i < p->count; i++) {
      char line[CG_DESCRIBE_EXCERPT + CG_EXCERPT_MARK];
      char where[CG_PATH_SHOWN + CG_EXCERPT_MARK + CG_LINE_SUFFIX_MAX];
      str_excerpt_line(p->items[i].body, CG_DESCRIBE_EXCERPT, line, sizeof(line));
      safe_strscpy(where, p->items[i].path);
      if (p->items[i].line > 0) {
         snprintf(where + strlen(where), sizeof(where) - strlen(where), ":%d", p->items[i].line);
      }
      strbuf_appendf(&sb, " %d) on %s \"%s\"%s", i + 1, where, line,
                     p->items[i].copilot_resolved ? " (resolved by Copilot already)"
                     : p->items[i].resolve        ? " (resolve)"
                                                  : " (leave open)");
   }
   const int n = snprintf(out, out_len, "%s", strbuf_str(&sb));
   strbuf_free(&sb);
   if (n > 0 && (size_t)n > CA_DESCRIBE_ROOM) {
      snprintf(out, out_len,
               "the replies are too long to approve by reply code; the user can ask for them "
               "again in the WebUI");
      return FAILURE;
   }
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}

int coding_github_describe(uint32_t id,
                           bool review,
                           char *out,
                           size_t out_len,
                           int *valid_for_sec) {
   turn_origin_t origin;
   snprintf(out, out_len, "that %s isn't waiting any more; propose it again",
            review ? "Copilot review request" : "reply");
   if (id == 0 || !turn_origin_capture(&origin)) {
      return FAILURE;
   }
   const pending_slots_t *slots = review ? &s_review_slots : &s_reply_slots;
   int rc = FAILURE;
   pthread_mutex_lock(&s_pending_mutex);
   pending_slot_t *slot = NULL;
   if (pending_slots_find(slots, &origin, tool_get_current_user_id(),
                          review ? CG_PENDING_REVIEW : CG_PENDING_REPLY, id, pending_slots_now(),
                          &slot) == PENDING_FOUND) {
      if (review) {
         const int n = snprintf(out, out_len, "ask Copilot to review %s",
                                ((const cg_review_pending_t *)slot)->pr);
         rc = (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
      } else {
         rc = describe_replies((const cg_replies_pending_t *)slot, out, out_len);
      }
      *valid_for_sec = pending_slots_valid_for(slots, slot, pending_slots_now());
   }
   pthread_mutex_unlock(&s_pending_mutex);
   return rc;
}

ca_confirm_kind_t coding_github_staged(uint32_t id, const turn_origin_t *origin, int user_id) {
   int kind = 0;
   pthread_mutex_lock(&s_pending_mutex);
   const time_t now = pending_slots_now();
   ca_confirm_kind_t found = CA_CONFIRM_NONE;
   if (pending_slots_kind_of(&s_reply_slots, origin, user_id, id, now, &kind)) {
      found = CA_CONFIRM_REPLY;
   } else if (pending_slots_kind_of(&s_review_slots, origin, user_id, id, now, &kind)) {
      found = CA_CONFIRM_REVIEW;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   return found;
}
