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
 * Component Status (Keepalive) System
 *
 * Provides bidirectional presence detection between OASIS components.
 * Dawn publishes to dawn/status, subscribes to hud/status.
 * Uses MQTT LWT for immediate disconnect detection and periodic
 * heartbeat for network resilience.
 */

#ifndef COMPONENT_STATUS_H
#define COMPONENT_STATUS_H

#include <mosquitto.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define STATUS_TOPIC_DAWN "dawn/status"
#define STATUS_TOPIC_HUD "hud/status"

#define STATUS_HEARTBEAT_INTERVAL_SEC 30
#define STATUS_TIMEOUT_SEC 90 /* 3x heartbeat interval */

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Configure MQTT LWT before connecting
 *
 * Must be called BEFORE mosquitto_connect(). Sets the Last Will and Testament
 * so the broker publishes offline status if Dawn disconnects unexpectedly.
 *
 * @param mosq MQTT client instance
 * @return 0 on success, non-zero on failure
 */
int component_status_set_lwt(struct mosquitto *mosq);

/**
 * @brief Initialize status system after MQTT connect
 *
 * Call from on_connect callback, on every connect. Publishes online status
 * and subscribes to peer status topics each time; starts the heartbeat timer
 * on the first.
 *
 * @param mosq MQTT client instance
 * @return 0 on success, non-zero on failure
 */
int component_status_init(struct mosquitto *mosq);

/**
 * @brief Publish offline status before graceful disconnect
 *
 * Call before mosquitto_disconnect() for clean shutdown.
 *
 * @param mosq MQTT client instance
 */
void component_status_publish_offline(struct mosquitto *mosq);

/**
 * @brief Shutdown status system
 *
 * Stops heartbeat timer and cleans up resources.
 */
void component_status_shutdown(void);

/* =============================================================================
 * Message Handling
 * ============================================================================= */

/**
 * @brief Handle incoming status message
 *
 * Call from on_message when topic matches STATUS_TOPIC_HUD.
 *
 * @param topic MQTT topic
 * @param payload Message payload (JSON)
 * @param payloadlen Payload length
 */
void component_status_handle_message(const char *topic, const char *payload, int payloadlen);

/* =============================================================================
 * State Queries
 * ============================================================================= */

/**
 * @brief Check if HUD/Mirage is currently online
 *
 * Returns true if we've received an online status and haven't timed out.
 *
 * @return true if HUD is online, false otherwise
 */
bool component_status_is_hud_online(void);

/**
 * @brief Get seconds since last HUD heartbeat
 *
 * @return Seconds since last status message, or -1 if never received
 */
int component_status_get_hud_age(void);

#ifdef __cplusplus
}
#endif

#endif /* COMPONENT_STATUS_H */
