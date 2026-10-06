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
 * HUD Discovery - MQTT-based dynamic discovery for HUD capabilities
 *
 * This module handles discovery of HUD elements and modes from Mirage
 * (the HUD renderer) via MQTT. When Mirage advertises its capabilities,
 * this module updates the tool registry with the available options.
 */

#ifndef HUD_DISCOVERY_H
#define HUD_DISCOVERY_H

#include <mosquitto.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define HUD_DISCOVERY_MAX_ITEMS 16        /* Max elements/modes from discovery */
#define HUD_DISCOVERY_STALE_THRESHOLD 300 /* 5 minutes in seconds */
#define HUD_DISCOVERY_TIMEOUT_MS 5000     /* Initial request timeout */

/* A discovered name reaches every conversation's standing directions, so it
 * is held to a plain shape: 1-32 letters, digits, spaces, '_' or '-'. */
#define HUD_DISCOVERY_NAME_MAX 32
/* Changes to the discovered sets applied per window (each changes every
 * conversation's standing directions, appended to its request); the same set
 * announced again is no change. */
#define HUD_DISCOVERY_CHANGES_PER_WINDOW 6
#define HUD_DISCOVERY_CHANGE_WINDOW_SEC 3600

/* MQTT Topics */
#define HUD_DISCOVERY_TOPIC_ELEMENTS "hud/discovery/elements"
#define HUD_DISCOVERY_TOPIC_MODES "hud/discovery/modes"
#define HUD_DISCOVERY_TOPIC_REQUEST "hud/discovery/request"
#define HUD_DISCOVERY_TOPIC_WILDCARD "hud/discovery/#"

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize the HUD discovery subsystem
 *
 * Subscribes to discovery topics and requests current state from Mirage.
 * Must be called after MQTT connection is established (in on_connect).
 *
 * @param mosq Mosquitto client instance
 * @return 0 on success, non-zero on error
 */
int hud_discovery_init(struct mosquitto *mosq);

/**
 * @brief Shutdown the HUD discovery subsystem
 *
 * Unsubscribes from discovery topics and cleans up state.
 */
void hud_discovery_shutdown(void);

/* =============================================================================
 * Message Handling
 * ============================================================================= */

/**
 * @brief Handle an incoming MQTT discovery message
 *
 * Called from on_message when a hud/discovery/# message is received.
 * Parses the JSON payload and updates tool parameters.
 *
 * @param topic Full MQTT topic (e.g., "hud/discovery/elements")
 * @param payload Message payload (JSON string)
 * @param payloadlen Length of payload
 */
void hud_discovery_handle_message(const char *topic, const char *payload, int payloadlen);

/* =============================================================================
 * State Queries
 * ============================================================================= */

/**
 * @brief Check if discovery data is valid (received and not stale)
 *
 * @return true if valid discovery data exists, false otherwise
 */
bool hud_discovery_is_valid(void);

/**
 * @brief Check if discovery data is stale
 *
 * Data is considered stale if it's older than HUD_DISCOVERY_STALE_THRESHOLD.
 *
 * @return true if data is stale or missing, false if fresh
 */
bool hud_discovery_is_stale(void);

/**
 * @brief Get the number of discovered HUD elements
 *
 * @return Number of elements, or 0 if no discovery data
 */
int hud_discovery_get_element_count(void);

/**
 * @brief Get the number of discovered HUD modes
 *
 * @return Number of modes, or 0 if no discovery data
 */
int hud_discovery_get_mode_count(void);

/** Whether @p name has the shape a discovered name must
 *  (HUD_DISCOVERY_NAME_MAX): only such a name is kept, or echoed back. */
bool hud_discovery_name_ok(const char *name);

/** Whether the helmet's last discovery listed HUD element @p name. */
bool hud_discovery_has_element(const char *name);

/** Whether the helmet's last discovery listed HUD mode @p name. */
bool hud_discovery_has_mode(const char *name);

/**
 * @brief The HUD elements and modes available now, as one line for the turn's
 *        standing directions ('HUD elements available now (hud_control): "a",
 *        "b". HUD modes available now (hud_mode): "x", "y".'), into @p out;
 *        each name quoted, as data
 *
 * The tools' schemas name no values (a conversation freezes its schemas, and
 * these change as the helmet connects); the model learns them here.
 *
 * @return The line's length; 0 (and "") when nothing was discovered
 */
size_t hud_discovery_describe(char *out, size_t size);

/* =============================================================================
 * Manual Control
 * ============================================================================= */

/**
 * @brief Request a discovery update from Mirage
 *
 * Publishes a discovery request message. Mirage will respond with
 * current elements and modes (if running).
 *
 * @param mosq Mosquitto client instance
 */
void hud_discovery_request_update(struct mosquitto *mosq);

/**
 * @brief Force use of default values
 *
 * Called when discovery times out or fails. Applies default
 * HUD elements and modes to the tool registry.
 */
void hud_discovery_apply_defaults(void);

#ifdef __cplusplus
}
#endif

#endif /* HUD_DISCOVERY_H */
