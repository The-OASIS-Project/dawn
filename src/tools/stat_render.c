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
 * STAT network-telemetry renderer (pure). See stat_render.h.
 */

#include "tools/stat_render.h"

#include <stdio.h>
#include <string.h>

/* Primary uplink = the default route with the LOWEST metric. NetworkManager
 * penalizes a failed-over path with +20000, so the metric (not the interface
 * name — usb0 isn't stable across USB re-enumeration) identifies the active
 * path. Returns the route index, or -1 if there is no default route. */
static int net_primary_route(const stat_snapshot_t *s) {
   int best = -1;
   for (int i = 0; i < s->net_route_count; i++) {
      if (best < 0 || s->net_routes[i].metric < s->net_routes[best].metric) {
         best = i;
      }
   }
   return best;
}

static const stat_net_iface_t *net_iface_by_name(const stat_snapshot_t *s, const char *name) {
   for (int i = 0; i < s->net_iface_count; i++) {
      if (strcmp(s->net_ifaces[i].name, name) == 0) {
         return &s->net_ifaces[i];
      }
   }
   return NULL;
}

static const stat_net_reach_t *net_reach_for_iface(const stat_snapshot_t *s, const char *iface) {
   for (int i = 0; i < s->net_reach_count; i++) {
      if (strcmp(s->net_reach[i].iface, iface) == 0) {
         return &s->net_reach[i];
      }
   }
   return NULL;
}

/* Append the honest reachability phrase for one interface's gateway probe. A
 * single reachable:false is a dropped packet, not an outage — only fail_streak
 * >= 2 reads as "down". */
static void append_reach_phrase(const stat_net_reach_t *re, char *buf, size_t sz) {
   if (!re) {
      return;
   }
   size_t off = strlen(buf);
   if (re->fail_streak >= 2) {
      snprintf(buf + off, sz - off, ", gateway unreachable (%d consecutive failures)",
               re->fail_streak);
   } else if (re->reachable) {
      if (re->has_rtt) {
         snprintf(buf + off, sz - off, ", gateway reachable (%.1f ms)", re->rtt_ms);
      } else {
         snprintf(buf + off, sz - off, ", gateway reachable");
      }
   } else {
      snprintf(buf + off, sz - off, ", gateway probe missed once (transient)");
   }
}

void stat_render_network(const stat_snapshot_t *s, char *buf, size_t sz) {
   if (!s->have_network) {
      size_t off = strlen(buf);
      snprintf(buf + off, sz - off, "No network telemetry from STAT. ");
      return;
   }

   int pr = net_primary_route(s);
   size_t off = strlen(buf);
   if (pr >= 0) {
      const stat_net_route_t *r = &s->net_routes[pr];
      const stat_net_iface_t *pf = net_iface_by_name(s, r->iface);
      const char *kind = (pf && pf->kind[0]) ? pf->kind : "";
      snprintf(buf + off, sz - off, "Network: primary path %s%s%s%s via %s. ", r->iface,
               kind[0] ? " (" : "", kind, kind[0] ? ")" : "", r->gateway);
   } else {
      snprintf(buf + off, sz - off, "Network: no default route. ");
   }

   for (int i = 0; i < s->net_iface_count; i++) {
      const stat_net_iface_t *f = &s->net_ifaces[i];
      bool linkup = f->up && f->carrier; /* trust up/carrier, not the operstate string */
      const stat_net_reach_t *re = net_reach_for_iface(s, f->name);
      off = strlen(buf);
      snprintf(buf + off, sz - off, "%s (%s): %s%s%s", f->name, f->kind[0] ? f->kind : "?",
               linkup ? "up" : "down", f->ipv4[0] ? ", " : "", f->ipv4[0] ? f->ipv4 : "");
      append_reach_phrase(re, buf, sz);
      if (f->addr_truncated) {
         off = strlen(buf);
         snprintf(buf + off, sz - off, " (+more addresses)");
      }
      /* Cellular honesty: a gateway ping over the cellular iface proves the USB
       * link to the modem, NOT that the cellular bearer is carrying traffic. A
       * global IPv6 on the iface is the best available bearer-presence proxy.
       * Lead with the caveat so it survives any buffer truncation, not the
       * "likely active" hedge. */
      if (strcmp(f->kind, "cellular") == 0 && linkup) {
         off = strlen(buf);
         snprintf(buf + off, sz - off,
                  " (gateway reachability here is the USB link to the modem, not the cellular "
                  "bearer; %s)",
                  f->ipv6_global[0] ? "a global IPv6 is present, so the bearer is likely active"
                                    : "no global IPv6, so the bearer may be down");
      }
      off = strlen(buf);
      snprintf(buf + off, sz - off, ". ");
   }

   if (!s->net_probe_available) {
      off = strlen(buf);
      snprintf(buf + off, sz - off, "(Reachability probe unavailable — link state only.) ");
   }
   if (s->net_ifaces_truncated || s->net_routes_truncated || s->net_reach_truncated) {
      off = strlen(buf);
      snprintf(buf + off, sz - off, "(Some interfaces/routes/probes omitted.) ");
   }
}
