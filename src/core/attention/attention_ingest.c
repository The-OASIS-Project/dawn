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
 * SAGE ingest — samples every available source ONCE per tick into a shared
 * ctx.  Poll-based (avoids the single-consumer on_message dispatch): telemetry
 * lives in the STAT/suit services' live caches, and this reuses their existing
 * copy-out snapshot APIs.  The get_snapshot() CALLS are the only #ifdef-guarded
 * bits — a compiled-out service leaves its snapshot struct zeroed (valid=false),
 * so catalog readers see "not present" and watches on it never fire.
 */

#include <stdatomic.h>
#include <string.h>

#include "core/attention/attention_internal.h"
#include "core/component_status.h"
#include "core/stat_net_interpret.h"

/* Dwell timers for the network duration metrics. File-static because the "how
 * long has this held" state must persist across ticks. Atomic because
 * attention_ingest_sample runs on several threads (the heartbeat tick plus the
 * off-tick metric_current / readings_snapshot query paths); only the tick caller
 * COMMITS a new value (advance_dwell), so there is a single writer and the
 * atomics keep the concurrent load/store race-free (TSan-clean). Probed family is
 * IPv4 (STAT's gateway probe is v4-only), so v6-only degradation is not covered. */
static _Atomic int64_t s_uplink_down_since_ms;
static _Atomic int64_t s_cellular_since_ms;

void attention_ingest_sample(attention_sample_ctx_t *ctx, int64_t now_ms, bool advance_dwell) {
   if (!ctx) {
      return;
   }
   memset(ctx, 0, sizeof(*ctx));
   ctx->now_ms = now_ms;

#ifdef DAWN_ENABLE_STAT_TOOL
   if (stat_service_is_active()) {
      stat_service_get_snapshot(&ctx->stat);
      ctx->stat_valid = true;
   }
#endif

#ifdef DAWN_ENABLE_SUIT_TOOL
   if (suit_service_is_active()) {
      suit_service_get_snapshot(&ctx->suit);
      ctx->suit_valid = true;
   }
#endif

   /* Component status (HUD/MIRAGE) is always compiled. */
   int hud_age = component_status_get_hud_age();
   ctx->hud_ever = (hud_age >= 0);
   ctx->hud_online = component_status_is_hud_online();
   ctx->hud_age_sec = (hud_age >= 0) ? hud_age : 0;

   /* Network transition state, derived from the STAT snapshot's routing/reach
    * view using the shared honesty rules. Only meaningful with fresh network
    * telemetry: when STAT is absent/stale or carries no network data, report
    * not-present and FREEZE the dwell timers (don't call the updater) so a brief
    * STAT blackout doesn't reset an in-progress outage/failover window. */
   ctx->net_present = ctx->stat_valid && !ctx->stat.stale && ctx->stat.have_network;
   if (ctx->net_present) {
      bool down = !stat_net_primary_healthy(&ctx->stat, "ipv4");
      bool cellular = stat_net_primary_is_cellular(&ctx->stat, "ipv4");
      /* Compute the dwell from the committed stamp; only the tick stores back, so
       * concurrent readers observe the current duration without perturbing it. */
      int64_t up_since = atomic_load_explicit(&s_uplink_down_since_ms, memory_order_relaxed);
      int64_t cell_since = atomic_load_explicit(&s_cellular_since_ms, memory_order_relaxed);
      ctx->net_uplink_down_sec = stat_net_since_update(&up_since, down, now_ms);
      ctx->net_cellular_sec = stat_net_since_update(&cell_since, cellular, now_ms);
      if (advance_dwell) {
         atomic_store_explicit(&s_uplink_down_since_ms, up_since, memory_order_relaxed);
         atomic_store_explicit(&s_cellular_since_ms, cell_since, memory_order_relaxed);
      }
   }
}
