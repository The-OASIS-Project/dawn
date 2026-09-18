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
 * STAT network-telemetry interpretation (pure). See stat_net_interpret.h.
 */

#include "core/stat_net_interpret.h"

#include <string.h>

static bool family_matches(const stat_net_route_t *r, const char *family) {
   if (!family || !family[0]) {
      return true;
   }
   return strcmp(r->family, family) == 0;
}

const stat_net_route_t *stat_net_primary_route(const stat_snapshot_t *s, const char *family) {
   if (!s) {
      return NULL;
   }
   const stat_net_route_t *best = NULL;
   for (int i = 0; i < s->net_route_count; i++) {
      const stat_net_route_t *r = &s->net_routes[i];
      if (!family_matches(r, family)) {
         continue;
      }
      if (!best || r->metric < best->metric) {
         best = r; /* first-listed wins an exact tie (strict <) */
      }
   }
   return best;
}

const stat_net_iface_t *stat_net_iface_by_name(const stat_snapshot_t *s, const char *name) {
   if (!s || !name || !name[0]) {
      return NULL;
   }
   for (int i = 0; i < s->net_iface_count; i++) {
      if (strcmp(s->net_ifaces[i].name, name) == 0) {
         return &s->net_ifaces[i];
      }
   }
   return NULL;
}

const stat_net_reach_t *stat_net_reach_for_iface(const stat_snapshot_t *s, const char *iface) {
   if (!s || !iface || !iface[0]) {
      return NULL;
   }
   for (int i = 0; i < s->net_reach_count; i++) {
      if (strcmp(s->net_reach[i].iface, iface) == 0) {
         return &s->net_reach[i];
      }
   }
   return NULL;
}

static bool iface_is_cellular(const stat_net_iface_t *f) {
   return f && strcmp(f->kind, "cellular") == 0;
}

bool stat_net_primary_healthy(const stat_snapshot_t *s, const char *family) {
   const stat_net_route_t *pr = stat_net_primary_route(s, family);
   if (!pr) {
      return false; /* no default route = no uplink */
   }
   const stat_net_iface_t *pi = stat_net_iface_by_name(s, pr->iface);
   if (pi && !(pi->up && pi->carrier)) {
      return false; /* link down (trust up/carrier, not operstate) */
   }
   const stat_net_reach_t *re = stat_net_reach_for_iface(s, pr->iface);
   if (re && re->fail_streak >= STAT_NET_DOWN_FAIL_STREAK) {
      return false; /* gateway down (>= 2 misses; a single miss is transient) */
   }
   return true; /* no reach row = link-state-only = treat as up */
}

bool stat_net_primary_is_cellular(const stat_snapshot_t *s, const char *family) {
   const stat_net_route_t *pr = stat_net_primary_route(s, family);
   if (!pr) {
      return false;
   }
   if (iface_is_cellular(stat_net_iface_by_name(s, pr->iface))) {
      return true;
   }
   /* Penalized-tie robustness: when the primary metric is at/over the +20000
    * penalty level, the first-listed tied route may be the dead wired path. If
    * any same-metric route in this family rides a cellular iface, the effective
    * uplink is the cellular backup. */
   if (pr->metric >= STAT_NET_PENALTY_METRIC) {
      for (int i = 0; i < s->net_route_count; i++) {
         const stat_net_route_t *r = &s->net_routes[i];
         if (!family_matches(r, family) || r->metric != pr->metric) {
            continue;
         }
         if (iface_is_cellular(stat_net_iface_by_name(s, r->iface))) {
            return true;
         }
      }
   }
   return false;
}

int stat_net_since_update(int64_t *since_ms, bool in_state, int64_t now_ms) {
   if (!since_ms) {
      return 0;
   }
   if (in_state) {
      if (*since_ms == 0) {
         *since_ms = now_ms;
      }
   } else {
      *since_ms = 0;
   }
   if (*since_ms == 0) {
      return 0;
   }
   int64_t held = now_ms - *since_ms;
   return held > 0 ? (int)(held / 1000) : 0;
}
