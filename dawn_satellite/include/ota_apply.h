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
 * OTA device-apply (Tier-1) — receives an ota_offer, verifies the signed
 * manifest + downloaded image, atomically swaps the satellite binary, and
 * exits so the launcher re-execs the new build.  Rollback itself lives in the
 * standalone launcher (ota_launch.c); this module only stages the rollback
 * copy + marker and signals liveness/health back into the marker.
 *
 * Threading: the offer handler runs on the WS service thread and spawns a
 * detached worker for the blocking download/verify/swap.  The worker is fully
 * decoupled from the ws_client (which is recreated per reconnect) — it reports
 * progress into a mailbox that the WS thread flushes via
 * ota_apply_flush_status().  See docs/OTA_DESIGN.md §7.
 */

#ifndef OTA_APPLY_H
#define OTA_APPLY_H

#include <stdbool.h>
#include <stdint.h>

#include "ws_client.h"

struct json_object;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle an inbound `ota_offer` (called on the WS thread, with the
 *        ws_client mutex NOT held — mirror the volume_set unlock/relock).
 *
 * Verifies the signed manifest up front, cross-checks it against the offer,
 * checks abi_tag + anti-rollback, and either ws_client_send_ota_reject()s or
 * sends an ack and spawns a detached worker for the download/verify/swap.
 * Connection scalars are passed in (copied) so the worker never derefs the
 * ws_client after spawn.
 *
 * @param client    Live ws_client (for the immediate ack/reject only).
 * @param uuid      This device's satellite uuid.
 * @param host,port,use_ssl,ssl_verify,ca_cert_path  Connection origin (reused for the
 *              HTTPS pull; ssl_verify gates TLS peer/host verification, same as the WS).
 * @param payload   The offer's JSON `payload` object.
 */
void ota_apply_handle_offer(ws_client_t *client,
                            const char *uuid,
                            const char *host,
                            uint16_t port,
                            bool use_ssl,
                            bool ssl_verify,
                            const char *ca_cert_path,
                            struct json_object *payload);

/**
 * @brief Flush any pending OTA status to the server.  Call ONLY from the WS
 *        service thread (live client, no mutex held).  No-op when nothing is
 *        pending.  This is how the decoupled worker's progress / terminal
 *        `failed` reach the server.
 */
void ota_apply_flush_status(ws_client_t *client);

/**
 * @brief Commit: the new binary reached a healthy registration — clear the
 *        pending-update marker.  Idempotent; safe if no update is in flight.
 *        Call from the satellite_register_ack success path.
 */
void ota_apply_mark_healthy(void);

/**
 * @brief Liveness: the new binary has run stably for ≥60 s.  Flags the marker
 *        `ran_ok=1` so the launcher commits even if the device can't reach the
 *        server (server-outage case), instead of false-rolling-back.  One-shot
 *        / idempotent; no-op if no marker.  Call from the reconnect loop.
 */
void ota_apply_mark_running(void);

/**
 * @brief Sweep stale staged downloads from ota/tmp.  Call once at startup.
 */
void ota_apply_cleanup_tmp(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_APPLY_H */
