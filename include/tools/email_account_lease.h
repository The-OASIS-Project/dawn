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
 * One IMAP connection per account at a time.  IMAP servers cap connections per
 * account, so every caller (the tool, the digest, a WebUI client) takes the
 * account's lease around a whole operation.  Waiters queue FIFO and a release
 * hands the lease straight to the head waiter, so none is passed over.
 *
 * A waiter is either a blocking thread or an asynchronous ticket: a ticket's
 * owner is told through the release hook, called with no lease lock held, and
 * the ticket then holds the lease.  Lock order: this lock is taken before the
 * OAuth per-account mutex and the auth_db lock, never while holding them.
 */

#ifndef EMAIL_ACCOUNT_LEASE_H
#define EMAIL_ACCOUNT_LEASE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMAIL_LEASE_OK 0
#define EMAIL_LEASE_FAILURE 1   /* bad argument, or release by a non-holder */
#define EMAIL_LEASE_TIMEOUT 2   /* the deadline passed before the lease came */
#define EMAIL_LEASE_CANCELLED 3 /* *cancel was set while waiting */
#define EMAIL_LEASE_FULL 4      /* too many accounts in use at once */
#define EMAIL_LEASE_SELF 5      /* this thread already holds it (a re-take would deadlock) */

/* How often a blocking wait looks at its cancel flag and deadline. */
#define EMAIL_LEASE_WAIT_SLICE_MS 250

/* Ticket states; the lease owns them. */
#define EMAIL_LEASE_TICKET_IDLE 0
#define EMAIL_LEASE_TICKET_WAITING 1
#define EMAIL_LEASE_TICKET_GRANTED 2

/**
 * An asynchronous waiter, embedded in its owner's struct.  Only @c owner is the
 * caller's; the rest belongs to the lease.  It must stay alive while WAITING or
 * holding the lease.  It can be used again once the lease it held is released
 * (back to IDLE) or once email_lease_cancel_ticket removed it.
 */
typedef struct email_lease_ticket {
   struct email_lease_ticket *next; /* lease: FIFO link */
   int64_t account_id;              /* lease */
   int state;                       /* lease: EMAIL_LEASE_TICKET_* */
   bool blocking;                   /* lease: a thread in email_lease_acquire */
   void *owner;                     /* caller's: what to run once granted */
} email_lease_ticket_t;

/** Called when the lease is handed to @p ticket; no lease lock is held. */
typedef void (*email_lease_hook_t)(email_lease_ticket_t *ticket);

/**
 * @brief Take @p account_id's lease, waiting FIFO behind earlier waiters
 * @param cancel    Checked every EMAIL_LEASE_WAIT_SLICE_MS (may be NULL)
 * @param timeout_s Seconds to wait at most (<= 0: no wait beyond an immediate grant)
 * @return EMAIL_LEASE_OK (held: release it), _TIMEOUT, _CANCELLED, _FULL, _SELF
 */
int email_lease_acquire(int64_t account_id, const atomic_bool *cancel, int timeout_s);

/**
 * @brief Take the lease now if it's free, else queue @p ticket
 * @param got Set when the lease was taken now (the caller holds it); otherwise
 *            @p ticket is WAITING and the hook will be told when it's granted
 * @return EMAIL_LEASE_OK, _FULL or _FAILURE (a ticket that isn't IDLE)
 */
int email_lease_acquire_async(int64_t account_id, email_lease_ticket_t *ticket, bool *got);

/**
 * @brief Withdraw a WAITING ticket
 * @return true if it was removed (it never held the lease); false if it wasn't
 *         waiting.  A ticket already GRANTED is the hook's to run and release.
 */
bool email_lease_cancel_ticket(email_lease_ticket_t *ticket);

/** Release @p account_id's lease (held by this caller), handing it to the next waiter. */
int email_lease_release(int64_t account_id);

/**
 * @brief Whether anyone holds @p account_id's lease (for assertions)
 *
 * A thread re-taking its own lease is caught by email_lease_acquire; a ticket
 * holder has no thread, so code that runs under a ticket's lease checks this.
 */
bool email_lease_is_held(int64_t account_id);

/** Register the hook tickets are granted through; a ticket is never granted without one.
 *  The hook hands the ticket on (e.g. queues its task) and must not touch it
 *  afterwards: the task may already have run and released the lease, which
 *  returns the ticket to its owner. */
void email_lease_set_hook(email_lease_hook_t hook);

/** Unregister the hook, returning only once no hook call is still running. */
void email_lease_clear_hook(void);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_ACCOUNT_LEASE_H */
