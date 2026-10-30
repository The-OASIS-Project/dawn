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
 * What the local voice interface is doing, published for other OASIS
 * components (an expressive body, a HUD avatar) on dawn/events as OCP events.
 */

#ifndef DAWN_EVENTS_H
#define DAWN_EVENTS_H

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

#define DAWN_EVENTS_TOPIC "dawn/events"

/**
 * @brief Publish one OCP event on dawn/events
 *
 * Adds the envelope (device "dawn", msg_type "event", the event name, a
 * timestamp in Unix ms, surface "local") to @p fields and publishes it, not
 * retained.  A no-op when MQTT is off.  Safe from any thread (libmosquitto
 * runs threaded); it takes no DAWN lock.
 *
 * @param event  The event's name (a noun: "state_change", "wake_word", ...)
 * @param fields The event's own fields, or NULL; always consumed
 */
void dawn_events_publish(const char *event, struct json_object *fields);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_EVENTS_H */
