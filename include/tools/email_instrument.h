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
 * Email IMAP login instrumentation + guarded circuit breaker (Phase 0 of the
 * IMAP connection-reuse work — see docs/IMAP_CONNECTION_REUSE_DESIGN.md).
 *
 * This is diagnostic scaffolding that answers the "test-connection succeeds but
 * search gets Login denied" mystery on HostGator shared hosting: it captures the
 * server's actual tagged rejection text on CURLE_LOGIN_DENIED, counts on-wire
 * logins per account (ground truth — a reused connection skips LOGIN), and adds
 * a cPHulk-safe single retry that fires ONLY for capacity/temporary rejections.
 * Phase 2 (the warm connection pool) subsumes these counters + retry policy.
 *
 * Thread-safety: the per-account counter/breaker store is guarded by an internal
 * leaf mutex (no other lock is acquired while it is held, and no I/O or sleep
 * happens under it). The per-op ctx is owned by one op on one handle and needs
 * no lock.
 */

#ifndef EMAIL_INSTRUMENT_H
#define EMAIL_INSTRUMENT_H

#include <curl/curl.h>
#include <stdbool.h>

/**
 * Per-operation debug-capture context. Stack-allocate one per email op and wire
 * it to the handle with email_instrument_attach() before the first perform; it
 * must outlive every curl_easy_perform() on that op's handle (including the
 * batch FETCH), so fold it with email_instrument_op_done() only at op end.
 */
typedef struct {
   char last_reject[256]; /* most recent tagged "NO"/"BAD"/"BYE" server line, sanitized + trimmed */
   int login_seen;        /* on-wire LOGIN/AUTHENTICATE commands sent (ground-truth login count) */
   int retries;           /* guarded retries taken this op (each adds one extra on-wire login) */
} email_instrument_ctx_t;

/**
 * @brief Turn on VERBOSE + a private DEBUGFUNCTION so server rejections and
 *        on-wire logins are captured into @p ctx.
 *
 * Call right after create_imap_handle()/create_smtp_handle() and before the
 * first perform. A private DEBUGFUNCTION replaces curl's stderr debug handler,
 * so this adds no log spew. The callback never logs or stores outbound command
 * text (which contains the password), only counts the login verb.
 */
void email_instrument_attach(CURL *curl, email_instrument_ctx_t *ctx);

/**
 * @brief Perform an auth-bearing, READ-ONLY IMAP op with the Phase-0 breaker.
 *
 * On CURLE_LOGIN_DENIED: logs the captured rejection + account at WARNING,
 * classifies it, and permits at most ONE guarded retry (2-4 s jitter) — only
 * when the rejection is affirmatively capacity/temporary-shaped AND the
 * account's cooldown has elapsed. Credential-shaped and unclassified rejections
 * are never retried (cPHulk-safe: a wrong password or a lockout must not be
 * hammered). Any other CURLcode is returned unchanged with no side effects.
 *
 * Use ONLY for the first (auth-bearing) perform of a read-only IMAP op. SMTP and
 * mutating ops (COPY/STORE/EXPUNGE) must NOT call this — use
 * email_instrument_note_denied() for their diagnosis instead.
 *
 * @return the final CURLcode (of the retry if one was taken, else the original).
 */
CURLcode email_instrument_perform(CURL *curl,
                                  email_instrument_ctx_t *ctx,
                                  const char *account,
                                  const char *op);

/**
 * @brief Fold this op's on-wire login count into the per-account totals and,
 *        for the first op of an account or an op that logged in more than once
 *        (intra-op reuse not happening), emit one INFO summary line.
 *
 * Healthy single-login ops stay silent to keep normal operation quiet.
 */
void email_instrument_op_done(const char *account,
                              const char *op,
                              CURL *curl,
                              const email_instrument_ctx_t *ctx);

/**
 * @brief Log a captured login rejection for a NON-retried path (SMTP send,
 *        test-connection). Bumps the denial counter; no retry, no cooldown.
 */
void email_instrument_note_denied(const char *account,
                                  const email_instrument_ctx_t *ctx,
                                  const char *op);

#endif /* EMAIL_INSTRUMENT_H */
