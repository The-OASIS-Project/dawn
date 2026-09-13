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
 * Email IMAP login instrumentation + guarded circuit breaker (Phase 0).
 * See docs/IMAP_CONNECTION_REUSE_DESIGN.md §7 and include/tools/email_instrument.h.
 */

#include "tools/email_instrument.h"

#include <ctype.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "logging.h"

/* Guarded-retry tuning (Phase 0). */
#define EMAIL_BREAKER_COOLDOWN_SEC 60 /* min seconds between guarded retries per account */
#define EMAIL_BREAKER_JITTER_MIN_MS 2000
#define EMAIL_BREAKER_JITTER_SPAN_MS 2001 /* [2000, 4000] ms */

/* =============================================================================
 * Case-insensitive bounded substring search
 *
 * The DEBUGFUNCTION receives non-NUL-terminated buffers, so all matching is
 * length-bounded.  Classification runs on the NUL-terminated last_reject via
 * ci_contains() (a thin strlen() wrapper).
 * ============================================================================= */

static bool mem_ci_contains(const char *hay, size_t haylen, const char *needle) {
   size_t nlen = strlen(needle);
   if (nlen == 0 || haylen < nlen)
      return false;
   for (size_t i = 0; i + nlen <= haylen; i++) {
      size_t j = 0;
      while (j < nlen && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
         j++;
      if (j == nlen)
         return true;
   }
   return false;
}

static bool ci_contains(const char *hay, const char *needle) {
   return mem_ci_contains(hay, strlen(hay), needle);
}

/* =============================================================================
 * Rejection classification
 *
 * HostGator/Dovecot maps several distinct conditions onto CURLE_LOGIN_DENIED.
 * We only ever retry an AFFIRMATIVELY capacity/temporary rejection; a
 * credential-shaped or unrecognized rejection is left alone (a wrong password
 * or a cPHulk lockout must not be retried into a longer lockout).
 * ============================================================================= */

static bool reject_is_credential(const char *s) {
   return ci_contains(s, "authenticationfailed") || ci_contains(s, "authentication failed") ||
          ci_contains(s, "invalid credentials") || ci_contains(s, "invalid login") ||
          ci_contains(s, "authorizationfailed") || ci_contains(s, "login failure") ||
          ci_contains(s, "password") || ci_contains(s, "[AUTHFAILED]");
}

static bool reject_is_capacity(const char *s) {
   return ci_contains(s, "[unavailable]") || ci_contains(s, "too many") ||
          ci_contains(s, "maximum number of connections") || ci_contains(s, "connection limit") ||
          ci_contains(s, "concurrent") || ci_contains(s, "[inuse]") ||
          ci_contains(s, "try again") || ci_contains(s, "temporarily") ||
          ci_contains(s, "rate limit") || ci_contains(s, "[limit]");
}

/* =============================================================================
 * Per-account counter + breaker store (fixed array, leaf mutex)
 * ============================================================================= */

#define EMAIL_INSTR_MAX_ACCOUNTS 16

typedef struct {
   char account[128];
   bool in_use;
   unsigned long logins;  /* cumulative on-wire logins */
   unsigned long ops;     /* cumulative instrumented ops */
   unsigned long denials; /* cumulative CURLE_LOGIN_DENIED */
   unsigned long retries; /* cumulative guarded retries taken */
   time_t cooldown_until; /* no guarded retry before this wall-clock time */
   bool baseline_logged;  /* one-time first-op baseline INFO emitted */
} email_acct_state_t;

static email_acct_state_t s_accts[EMAIL_INSTR_MAX_ACCOUNTS];
static pthread_mutex_t s_accts_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Find or create the slot for `account`.  Caller MUST hold s_accts_mutex.
 * Returns NULL only if the table is full (degrades to unattributed logging). */
static email_acct_state_t *acct_slot_locked(const char *account) {
   if (!account || !account[0])
      account = "(unknown)";
   int free_idx = -1;
   for (int i = 0; i < EMAIL_INSTR_MAX_ACCOUNTS; i++) {
      if (s_accts[i].in_use) {
         /* Keyed on the first 128 bytes of the username. Two usernames sharing a
          * 127-char prefix would merge counters — cosmetic (mis-attributed
          * diagnostic totals) and never an auth boundary; IMAP usernames don't
          * approach that length in practice. */
         if (strncmp(s_accts[i].account, account, sizeof(s_accts[i].account)) == 0)
            return &s_accts[i];
      } else if (free_idx < 0) {
         free_idx = i;
      }
   }
   if (free_idx < 0)
      return NULL;
   email_acct_state_t *slot = &s_accts[free_idx];
   memset(slot, 0, sizeof(*slot));
   slot->in_use = true;
   snprintf(slot->account, sizeof(slot->account), "%s", account);
   return slot;
}

/* =============================================================================
 * DEBUGFUNCTION
 *
 * Captures the server's tagged rejection lines (HEADER_IN) and counts on-wire
 * LOGIN/AUTHENTICATE commands (HEADER_OUT).  Outbound command text carries the
 * password, so it is NEVER stored or logged — only the login verb is matched.
 * ============================================================================= */

/* True iff the outbound IMAP/SMTP command line's first token after the tag is
 * an authentication verb (LOGIN / AUTHENTICATE / AUTH). */
static bool header_out_is_login(const char *data, size_t size) {
   size_t i = 0;
   /* Skip the command tag (leading non-space run, e.g. "A001"). */
   while (i < size && !isspace((unsigned char)data[i]))
      i++;
   while (i < size && (data[i] == ' ' || data[i] == '\t'))
      i++;
   const char *verb = data + i;
   size_t vlen = size - i;
   /* Match verb followed by a space or end-of-token. */
   static const char *const kVerbs[] = { "LOGIN", "AUTHENTICATE", "AUTH", NULL };
   for (int v = 0; kVerbs[v]; v++) {
      size_t k = strlen(kVerbs[v]);
      if (vlen >= k && mem_ci_contains(verb, k, kVerbs[v]) &&
          (vlen == k || verb[k] == ' ' || verb[k] == '\r' || verb[k] == '\n'))
         return true;
   }
   return false;
}

static int email_debug_cb(CURL *handle,
                          curl_infotype type,
                          char *data,
                          size_t size,
                          void *userptr) {
   (void)handle;
   email_instrument_ctx_t *ctx = (email_instrument_ctx_t *)userptr;
   if (!ctx || !data || size == 0)
      return 0;

   if (type == CURLINFO_HEADER_IN) {
      /* Server -> client control line.  Capture tagged failures + BYE. */
      if (mem_ci_contains(data, size, " NO ") || mem_ci_contains(data, size, " BAD ") ||
          mem_ci_contains(data, size, "* BYE")) {
         size_t n = size < sizeof(ctx->last_reject) - 1 ? size : sizeof(ctx->last_reject) - 1;
         /* Drop trailing CR/LF/space from the raw line first... */
         while (n > 0 && (data[n - 1] == '\r' || data[n - 1] == '\n' || data[n - 1] == ' '))
            n--;
         /* ...then copy, mapping any remaining non-printable byte to '.'.  This
          * text is server-controlled and gets logged via %s; a hostile/comprom-
          * ised IMAP server could otherwise embed CR/LF or ANSI escapes to forge
          * multi-line log entries or injection into a terminal log viewer. */
         for (size_t k = 0; k < n; k++) {
            unsigned char c = (unsigned char)data[k];
            ctx->last_reject[k] = (c < 0x20 || c == 0x7f) ? '.' : (char)c;
         }
         ctx->last_reject[n] = '\0';
      }
   } else if (type == CURLINFO_HEADER_OUT) {
      /* Client -> server command.  Count logins; never store the line. */
      if (header_out_is_login(data, size))
         ctx->login_seen++;
   }
   return 0;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

void email_instrument_attach(CURL *curl, email_instrument_ctx_t *ctx) {
   if (!curl || !ctx)
      return;
   memset(ctx, 0, sizeof(*ctx));
   curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, email_debug_cb);
   curl_easy_setopt(curl, CURLOPT_DEBUGDATA, ctx);
   curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
}

CURLcode email_instrument_perform(CURL *curl,
                                  email_instrument_ctx_t *ctx,
                                  const char *account,
                                  const char *op) {
   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_LOGIN_DENIED || !ctx)
      return res;

   const char *reject = ctx->last_reject[0] ? ctx->last_reject : "(no tagged rejection captured)";
   bool cred = reject_is_credential(ctx->last_reject);
   bool cap = reject_is_capacity(ctx->last_reject);
   const char *cls = cred ? "credential/auth" : cap ? "capacity/temporary" : "unclassified";

   time_t now = time(NULL);
   bool do_retry = false;
   unsigned long denials = 0, retries = 0;

   pthread_mutex_lock(&s_accts_mutex);
   email_acct_state_t *slot = acct_slot_locked(account);
   if (slot) {
      slot->denials++;
      denials = slot->denials;
      /* Retry only an affirmatively capacity-shaped rejection, never a
       * credential-shaped one, and at most once per cooldown window. */
      if (cap && !cred && now >= slot->cooldown_until) {
         do_retry = true;
         slot->retries++;
         slot->cooldown_until = now + EMAIL_BREAKER_COOLDOWN_SEC;
      }
      retries = slot->retries;
   }
   pthread_mutex_unlock(&s_accts_mutex);

   /* Record the retry on the ctx so op_done can discount its extra on-wire login
    * from the intra-op-reuse anomaly signal (a denial+retry logs in twice, which
    * is not a reuse failure). */
   if (do_retry)
      ctx->retries++;

   if (!do_retry) {
      const char *why = cred  ? "credential-shaped — fix the account, not the client"
                        : cap ? "capacity-shaped but cooldown active"
                              : "unclassified — observing before any retry";
      OLOG_WARNING(
          "email: [%s] IMAP %s login denied (%s): \"%s\" [denial #%lu] — not retrying (%s)",
          account ? account : "(unknown)", op ? op : "?", cls, reject, denials, why);
      return res;
   }

   /* Jitter 2-4 s off a monotonic-clock nanosecond spread (no global rand seed). */
   struct timespec tp;
   clock_gettime(CLOCK_MONOTONIC, &tp);
   int jitter_ms = EMAIL_BREAKER_JITTER_MIN_MS + (int)(tp.tv_nsec % EMAIL_BREAKER_JITTER_SPAN_MS);
   OLOG_WARNING(
       "email: [%s] IMAP %s login denied (%s): \"%s\" [denial #%lu] — retrying once in %d ms",
       account ? account : "(unknown)", op ? op : "?", cls, reject, denials, jitter_ms);

   struct timespec sleep_ts = { jitter_ms / 1000, (long)(jitter_ms % 1000) * 1000000L };
   nanosleep(&sleep_ts, NULL);

   res = curl_easy_perform(curl);
   if (res == CURLE_OK) {
      OLOG_WARNING("email: [%s] IMAP %s retry succeeded after login denial [retry #%lu]",
                   account ? account : "(unknown)", op ? op : "?", retries);
   } else {
      /* A denied retry is a second on-wire denial — count it so the `denied`
       * total stays comparable to the on-wire login count. */
      if (res == CURLE_LOGIN_DENIED) {
         pthread_mutex_lock(&s_accts_mutex);
         email_acct_state_t *slot2 = acct_slot_locked(account);
         if (slot2) {
            slot2->denials++;
            denials = slot2->denials;
         }
         pthread_mutex_unlock(&s_accts_mutex);
      }
      OLOG_WARNING(
          "email: [%s] IMAP %s retry failed after login denial: %s [retry #%lu, denial #%lu]",
          account ? account : "(unknown)", op ? op : "?", curl_easy_strerror(res), retries,
          denials);
   }
   return res;
}

void email_instrument_op_done(const char *account,
                              const char *op,
                              CURL *curl,
                              const email_instrument_ctx_t *ctx) {
   if (!ctx)
      return;

   long new_conns = 0;
   if (curl)
      curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &new_conns);

   /* Discount retries: a denial+retry logs in twice on purpose, which is not an
    * intra-op reuse failure.  Only "logged in more than once for reasons other
    * than a guarded retry" is the reuse-not-happening tell. */
   bool first = false, anomaly = (ctx->login_seen - ctx->retries) > 1;
   unsigned long L = 0, O = 0, D = 0, R = 0;

   pthread_mutex_lock(&s_accts_mutex);
   email_acct_state_t *slot = acct_slot_locked(account);
   if (slot) {
      slot->ops++;
      slot->logins += (unsigned long)ctx->login_seen;
      if (!slot->baseline_logged) {
         slot->baseline_logged = true;
         first = true;
      }
      L = slot->logins;
      O = slot->ops;
      D = slot->denials;
      R = slot->retries;
   }
   pthread_mutex_unlock(&s_accts_mutex);

   /* Quiet by default: one baseline line per account, plus a line whenever an op
    * logged in more than once (the intra-op-reuse-not-happening signal). */
   if (first || anomaly)
      OLOG_INFO("email: [%s] op=%s on-wire-logins=%d new-conns=%ld | totals logins=%lu ops=%lu "
                "denied=%lu retried=%lu%s",
                account ? account : "(unknown)", op ? op : "?", ctx->login_seen, new_conns, L, O, D,
                R, anomaly ? "  <-- intra-op reuse NOT happening" : "");
}

void email_instrument_note_denied(const char *account,
                                  const email_instrument_ctx_t *ctx,
                                  const char *op) {
   const char *reject = (ctx && ctx->last_reject[0]) ? ctx->last_reject
                                                     : "(no tagged rejection captured)";
   unsigned long denials = 0;
   pthread_mutex_lock(&s_accts_mutex);
   email_acct_state_t *slot = acct_slot_locked(account);
   if (slot) {
      slot->denials++;
      denials = slot->denials;
   }
   pthread_mutex_unlock(&s_accts_mutex);
   OLOG_WARNING("email: [%s] %s login denied: \"%s\" [denial #%lu]",
                account ? account : "(unknown)", op ? op : "?", reject, denials);
}
