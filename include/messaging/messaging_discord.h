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
 * Discord driver — public registration entry point.
 *
 * v1: DM-only, text-only, LLM-bound.  Connects to the Discord Gateway
 * via libwebsockets client mode and sends outbound via REST.  Same
 * forever-conversation semantics as SMS / Telegram.  See
 * atlas/dawn/archive/MESSAGING_CHANNELS_DESIGN.md §8 (Discord) and §13 Phase 3.
 */
#ifndef MESSAGING_DISCORD_H
#define MESSAGING_DISCORD_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register and initialize the Discord driver with the messaging
 *        engine.  No-op when bot_token is NULL or empty.
 *
 * Must be called AFTER messaging_engine_init().  Spawns a listener
 * thread on a 64 KB stack that runs the Gateway WebSocket connection
 * + heartbeat loop.
 *
 * @param bot_token  Bot token from the Discord Developer Portal
 *                   (stored in secrets.toml as discord_bot_token).
 * @return SUCCESS / FAILURE.
 */
int messaging_discord_register(const char *bot_token);

/**
 * @brief Tear the driver down.  Joins the listener thread.
 */
void messaging_discord_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_DISCORD_H */
