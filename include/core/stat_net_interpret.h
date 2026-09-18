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
 * STAT network-telemetry interpretation (pure, Layer-2, always compiled).
 *
 * The single source of truth for the network "honesty rules" applied to a
 * stat_snapshot_t: primary path = the LOWEST-metric default route (NetworkManager
 * penalizes a failed-over path with +20000, so the metric — never the interface
 * name, which re-enumerates — identifies the active path); link state = up &&
 * carrier (not the operstate string); "down" only at fail_streak >= 2 (a single
 * missed probe is transient); and the cellular-bearer honesty caveat (a gateway
 * ping over a cellular iface proves the USB link to the modem, not that the
 * bearer is carrying traffic).
 *
 * Shared by the STAT tool renderer (src/tools/stat_render.c, Layer 3) AND the
 * SAGE proactive-attention catalog readers (src/core/attention/, Layer 2). It
 * therefore lives at Layer 2 and depends only on the snapshot structs in
 * core/stat_service.h — no tool, service, or config plumbing — so both consumers
 * interpret the wire identically and cannot drift.
 */

#ifndef STAT_NET_INTERPRET_H
#define STAT_NET_INTERPRET_H

#include <stdbool.h>
#include <stdint.h>

#include "core/stat_service.h" /* stat_snapshot_t + net structs */

/* NetworkManager adds this to a route's metric when its connectivity check
 * marks the path degraded; a metric at or above it means "penalized / backup". */
#define STAT_NET_PENALTY_METRIC 20000

/* Consecutive gateway-probe failures that read as "down"; a single miss (1) is a
 * dropped packet, not an outage. Matches the renderer's fail_streak >= 2 rule. */
#define STAT_NET_DOWN_FAIL_STREAK 2

/**
 * @brief The primary default route: the lowest-metric route (optionally filtered
 *        to one address family), first-listed winning an exact metric tie.
 *
 * @param s      snapshot (must be non-NULL).
 * @param family "ipv4" / "ipv6" to restrict to that family, or NULL/"" for any.
 * @return pointer into @p s (valid for its lifetime), or NULL if there is no
 *         matching default route.
 */
const stat_net_route_t *stat_net_primary_route(const stat_snapshot_t *s, const char *family);

/** @brief Interface named @p name, or NULL. */
const stat_net_iface_t *stat_net_iface_by_name(const stat_snapshot_t *s, const char *name);

/** @brief Reachability probe row bound to @p iface, or NULL. */
const stat_net_reach_t *stat_net_reach_for_iface(const stat_snapshot_t *s, const char *iface);

/**
 * @brief Is the primary path (in @p family) healthy right now?
 *
 * Healthy = a default route exists AND its interface is up && carrier AND its
 * gateway probe is not down (no reach row, or fail_streak < 2 — link-state-only
 * counts as healthy, matching the renderer). No default route, a down link, or a
 * gateway at/over the fail-streak threshold all read as NOT healthy.
 *
 * NOTE (cellular blind spot): when the primary path is itself cellular, the
 * gateway probe answers from the modem's own IP stack regardless of whether the
 * bearer carries traffic, so this can only detect USB-link loss there, not a dead
 * bearer. Bearer liveness needs an upstream signal (ECHO/STAT), not the probe.
 */
bool stat_net_primary_healthy(const stat_snapshot_t *s, const char *family);

/**
 * @brief Is the primary path (in @p family) the cellular backup?
 *
 * True when the primary route's interface kind is "cellular". Robust to a
 * penalized metric tie (both wired and cellular at the +20000 level): if the
 * primary metric is penalized and any same-metric route in @p family is cellular,
 * the effective uplink is the cellular backup, so this returns true even when the
 * first-listed tied route is the (dead) wired path.
 */
bool stat_net_primary_is_cellular(const stat_snapshot_t *s, const char *family);

/**
 * @brief Idempotent "time-in-state" bookkeeping for a dwell metric.
 *
 * Tracks how long a boolean condition has held continuously, using one caller-
 * owned timestamp slot. On the first call where @p in_state is true, stamps
 * *@p since_ms = @p now_ms; while it stays true, leaves the stamp untouched;
 * when it goes false, clears the stamp to 0. Returns the seconds the condition
 * has held (0 when not in state).
 *
 * MUST be idempotent because SAGE ingest is sampled off-tick as well as on-tick
 * (metric_current / readings_snapshot): calling it twice with the same @p now_ms
 * yields the same stamp and duration — never an accumulator. To FREEZE the timer
 * across an unknown gap (e.g. STAT stale), simply do not call it; the stamp is
 * preserved and the duration resumes from it when sampling resumes.
 *
 * 0 is the reserved "not in state" sentinel for *@p since_ms, valid because
 * callers pass an epoch-millisecond @p now_ms (never 0); a monotonic-from-boot
 * clock starting near 0 would need a different sentinel.
 */
int stat_net_since_update(int64_t *since_ms, bool in_state, int64_t now_ms);

#endif /* STAT_NET_INTERPRET_H */
