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
 * Actions waiting for the user's reply code: one per messaging channel.
 *
 * A request by text from a sender the channel can't vouch for (SMS: any
 * number can be put on a text) may read and prepare, but an action waits
 * here.  DAWN texts the user what was asked and a code (the model never sees
 * the code); the user's reply with it runs the stored call, once.  A forger
 * can put the user's number on a text, but can't read the text DAWN sends to
 * it.
 *
 * Keyed by channel (its messaging_channels row), which outlives a session
 * (/new, an evicted session): a new session can't reset the limits, and
 * /new drops what waits.  A newer request from a later turn replaces an
 * older one (the older code stops working), so the user is never asked to
 * use a code for something else.  A leaf mutex guards it; no other lock is
 * taken while it is held.
 */

#ifndef TOOL_CALL_CHALLENGE_H
#define TOOL_CALL_CHALLENGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/reply_code.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOOL_CALL_CHALLENGE_TTL_SEC 300 /* a code is good for five minutes at most */
#define TOOL_CALL_CHALLENGE_MIN_SEC 60  /* less than this left: not worth a code */
#define TOOL_CALL_CHALLENGE_TRIES 3     /* wrong replies before it's void */
/* Codes a channel may have outstanding or left unused: one the user replies
 * with is refunded (a forger never sees a code, so never redeems one). */
#define TOOL_CALL_CHALLENGE_PER_HOUR 10   /* unused codes per channel, any hour */
#define TOOL_CALL_CHALLENGE_PER_DAY 30    /* unused codes per channel, any day */
#define TOOL_CALL_CHALLENGE_ENDED_SEC 300 /* a late reply with an ended code is still answered */
#define TOOL_CALL_CHALLENGE_DESC_MAX 480  /* what the text says was asked */
#define TOOL_CALL_CHALLENGE_TOOL_MAX 64

typedef enum {
   TOOL_CHALLENGE_OK = 0,
   TOOL_CHALLENGE_BUSY,  /* this turn already holds another action for a code */
   TOOL_CHALLENGE_SAME,  /* this very call already waits for its code (no new one) */
   TOOL_CHALLENGE_LIMIT, /* this channel was sent too many codes lately */
   TOOL_CHALLENGE_FULL,  /* no room (every slot holds another channel's) */
} tool_challenge_rc_t;

/** A held call: what runs, and what the user is shown. */
typedef struct {
   int64_t channel_id;
   int user_id;
   uint64_t turn_token; /* the turn that asked */
   int valid_for_sec;   /* how long what it confirms stays valid (capped at the TTL) */
   const char *tool;
   const char *args;        /* as the model sent them; resolved again when run */
   const char *binding;     /* the call as resolved when held (llm_tools_approved_binding) */
   const char *description; /* DAWN's words for the text */
} tool_challenge_t;

/** How a code the user was texted stopped working. */
typedef enum {
   TOOL_CHALLENGE_END_NONE = 0,
   TOOL_CHALLENGE_END_EXPIRED,   /* ran out */
   TOOL_CHALLENGE_END_REPLACED,  /* a newer request took its place */
   TOOL_CHALLENGE_END_CANCELLED, /* STOP, /new, a reset or an unlink */
   TOOL_CHALLENGE_END_VOIDED,    /* too many wrong codes */
   TOOL_CHALLENGE_END_USED,      /* the user replied with it: it ran */
} tool_challenge_end_t;

/** Hold a call until the user replies with its code (replacing an older one
 *  of the channel's from an earlier turn); @p c->channel_id must be > 0 (else
 *  FULL).  The same call (its binding) asked
 *  again while it waits keeps the code it has: nothing new is sent or counted
 *  (and it then counts as this turn's hold). */
tool_challenge_rc_t tool_call_challenge_create(const tool_challenge_t *c);

/** Whether an action is waiting for its code on this channel. */
bool tool_call_challenge_live(int64_t channel_id);

typedef enum {
   TOOL_TAKE_OK = 0,
   TOOL_TAKE_NONE,    /* nothing on this channel waits to be sent */
   TOOL_TAKE_EXPIRED, /* one ran out before its code was texted (said once) */
} tool_take_rc_t;

/**
 * @brief The code and description of a challenge not yet texted: the code
 *        is handed out once, then kept only as a digest
 *
 * @param seconds Receives how many seconds it stays good (may be NULL)
 */
tool_take_rc_t tool_call_challenge_take_unsent(int64_t channel_id,
                                               char code[REPLY_CODE_LEN],
                                               char *description,
                                               size_t description_len,
                                               int *seconds);

typedef enum {
   TOOL_REDEEM_OK = 0,
   TOOL_REDEEM_NONE,    /* nothing waits for a code on this channel */
   TOOL_REDEEM_EXPIRED, /* it did, but its time is up (it's gone) */
   TOOL_REDEEM_WRONG,   /* not its code (tries left) */
   TOOL_REDEEM_VOIDED,  /* not its code, and that was the last try (it's gone) */
} tool_redeem_rc_t;

/** A redeemed call, handed over to be run once. */
typedef struct {
   char tool[TOOL_CALL_CHALLENGE_TOOL_MAX];
   char *args; /* malloc'd: the caller frees it */
   char binding[REPLY_CODE_DIGEST_HEX];
   char description[TOOL_CALL_CHALLENGE_DESC_MAX];
} tool_redeemed_t;

/**
 * @brief The user's reply: on the right code, the stored call is handed over
 *        (and removed)
 *
 * @param out   Receives the call on TOOL_REDEEM_OK
 * @param tries Receives the wrong tries so far on TOOL_REDEEM_WRONG (may be NULL)
 */
tool_redeem_rc_t tool_call_challenge_redeem(int64_t channel_id,
                                            int user_id,
                                            const char *code,
                                            tool_redeemed_t *out,
                                            int *tries);

/** Whether nothing waits on this channel but a code texted to it stopped
 *  working within TOOL_CALL_CHALLENGE_ENDED_SEC (a late reply with it gets
 *  DAWN's answer, not the model's). */
bool tool_call_challenge_ended_recently(int64_t channel_id);

/** How that code ended (NONE past the window); said once, then forgotten. */
tool_challenge_end_t tool_call_challenge_take_ended(int64_t channel_id);

/** Drop this channel's waiting action, if any.  @return whether one was. */
bool tool_call_challenge_cancel(int64_t channel_id);

/** Whether this channel's waiting action was held by the turn @p turn_token. */
bool tool_call_challenge_live_in_turn(int64_t channel_id, uint64_t turn_token);

/** Drop this channel's waiting action if an earlier turn held it (the user
 *  has moved on to a new request).  @return whether one was dropped. */
bool tool_call_challenge_cancel_earlier(int64_t channel_id, uint64_t turn_token);

/** Drop this channel's waiting action whose code couldn't be texted; it
 *  doesn't count against the channel's limits. */
void tool_call_challenge_cancel_unsent(int64_t channel_id);

/** Forget every waiting action and count (shutdown, tests). */
void tool_call_challenge_clear_all(void);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_CALL_CHALLENGE_H */
