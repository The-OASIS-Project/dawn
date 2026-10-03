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
 * Who may make a tool call: the caller's kind of turn against the call's
 * kind of action (tool_action_kind_t).
 *
 *   caller                           read  fetch  state  device  prepare  act
 *   user, live turn                  yes   yes    yes    yes     yes      yes
 *   unverified sender (SMS)          yes   no     yes    no      yes      no
 *   background job, research worker  yes   yes    yes    no      no       no
 *   unattended: a background turn,
 *   no running turn, no context      yes   no     yes    no      no       no
 *
 * A text can claim any number as its sender, so an unverified turn reads and
 * prepares but doesn't act.  Background work reads, looks things up and
 * reports; it never acts: what it read may be steering it.  An unattended
 * turn (a job's follow-up, in whatever session it runs) and a call with no
 * running turn (an MQTT message naming a session) can't reach out either.
 *
 * Decided once per call, at the one place every model tool call runs
 * (llm_tools_execute_from_treg; plan steps come back through it), and at the
 * MQTT entry that names a session.  Not here: the local mic's direct
 * commands (the user speaking), MQTT device commands with no session, and
 * scheduled steps (scheduling itself acts).  The MQTT gate limits what a
 * message naming a session can do in it; it is not a boundary for MQTT
 * itself, whose trust is the broker's (a message naming no session runs as
 * the device always has).
 *
 * Layer 2: the caller check reads the session unit.
 */

#ifndef TOOL_CALL_POLICY_H
#define TOOL_CALL_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#include "tools/tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   TOOL_CALLER_USER = 0,   /* a live turn the user started */
   TOOL_CALLER_UNVERIFIED, /* the same, from a sender the channel can't vouch for */
   TOOL_CALLER_JOB,        /* a background job's or research worker's own turn */
   TOOL_CALLER_UNATTENDED, /* a background turn, no running turn, or no context */
} tool_caller_t;

typedef enum {
   TOOL_CALL_ALLOW = 0,
   TOOL_CALL_REFUSE,
   TOOL_CALL_CHALLENGE, /* allowed once the user proves it's them (a reply code) */
} tool_call_decision_t;

#ifdef ENABLE_MULTI_CLIENT
/** The kind of turn the calling code runs in (the command context's). */
tool_caller_t tool_call_policy_caller(void);
#else
/* No sessions: every call is the local user's. */
static inline tool_caller_t tool_call_policy_caller(void) {
   return TOOL_CALLER_USER;
}
#endif

/** The caller's name, for logs. */
const char *tool_call_policy_caller_name(tool_caller_t caller);

/** What the policy table says for @p caller and @p kind. */
tool_call_decision_t tool_call_policy_decide(tool_caller_t caller, tool_action_kind_t kind);

/** A call as the gate saw it. */
typedef struct {
   tool_caller_t caller;
   tool_action_kind_t kind;
   char action[64];     /* the effective action, in the tool's own spelling */
   const char *refusal; /* why it was refused (static text) */
   char message[256];   /* the message for the model when refused */
} tool_call_verdict_t;

/**
 * @brief Decide a call: its action checked against the tool's (in the tool's
 *        own spelling, ignoring case; an omitted one takes the tool's
 *        default), then its kind against the caller
 *
 * @param meta         The tool
 * @param device       The resolved device (a meta-tool's target; may be NULL)
 * @param named_action The action the call named ("" or NULL when none)
 * @param value        The packed value (NULL when empty)
 * @param caller       Who calls (tool_call_policy_caller(), or what an entry
 *                     knows it is); inside another call's scope, that call's
 *                     caller must allow it too
 * @param out          Receives the effective action, kind and any message
 * @return ALLOW (run out->action), REFUSE (out->message says why), or
 *         CHALLENGE
 */
tool_call_decision_t tool_call_policy_check(const tool_metadata_t *meta,
                                            const char *device,
                                            const char *named_action,
                                            const char *value,
                                            tool_caller_t caller,
                                            tool_call_verdict_t *out);

/**
 * @brief The call running on this thread, as decided: a scope entered
 *        around it and left after, so a call made inside another (a plan's
 *        steps) has its own and the outer one's comes back
 */
typedef struct {
   bool active;
   tool_action_kind_t kind;
   tool_caller_t caller; /* who it was decided for: calls inside it are no freer */
   bool code_redeemed;   /* the session unit's flag, as it stood */
} tool_call_scope_t;

/**
 * @brief Enter a call's scope; returns the one to restore
 *
 * @param kind     The kind decided for the call
 * @param caller   Who it was decided for: a call made inside it (a plan's
 *                 steps) is decided for both its own caller and this one
 * @param redeemed The user approved this one call by reply code (it never
 *                 reaches a call inside it)
 */
tool_call_scope_t tool_call_policy_enter(tool_action_kind_t kind,
                                         tool_caller_t caller,
                                         bool redeemed);

/** Leave a call's scope, restoring @p previous. */
void tool_call_policy_leave(tool_call_scope_t previous);

/** The kind decided for this thread's call; false outside a call's scope. */
bool tool_call_policy_decided(tool_action_kind_t *kind);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_CALL_POLICY_H */
