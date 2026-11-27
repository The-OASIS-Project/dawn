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
 * Slack driver — public registration entry point.
 *
 * v1: DM-only, text-only, LLM-bound.  Connects to Slack Socket Mode via
 * libwebsockets client mode and sends outbound via REST chat.postMessage.
 * Same forever-conversation semantics as SMS / Telegram / Discord.
 * Single-workspace token install for v1; OAuth-per-workspace install is
 * deferred to a follow-up.  See atlas/dawn/archive/MESSAGING_CHANNELS_DESIGN.md §8
 * (Slack) and §13 Phase 4.
 *
 * Required Slack App configuration (operator one-time):
 *   - Create app at api.slack.com/apps
 *   - Enable Socket Mode
 *   - Bot scopes: chat:write, im:history, im:read, app_mentions:read
 *   - Event subscriptions (bot events): message.im, app_mention
 *   - Install to workspace; copy "Bot User OAuth Token" (xoxb-...) and
 *     "App-Level Token" (xapp-...) into secrets.toml
 */
#ifndef MESSAGING_SLACK_H
#define MESSAGING_SLACK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register and initialize the Slack driver with the messaging
 *        engine.  No-op when either token is NULL or empty.
 *
 * Must be called AFTER messaging_engine_init().  Spawns a listener
 * thread on a 64 KB stack that opens the Socket Mode WebSocket via
 * `apps.connections.open` and runs the receive + ack loop.
 *
 * @param app_token  App-level token (xapp-...) used to open the Socket
 *                   Mode connection.  Stored in secrets.toml as
 *                   slack_app_token.
 * @param bot_token  Bot user OAuth token (xoxb-...) used for outbound
 *                   REST sends.  Stored in secrets.toml as
 *                   slack_bot_token.
 * @return SUCCESS / FAILURE.
 */
int messaging_slack_register(const char *app_token, const char *bot_token);

/**
 * @brief Tear the driver down.  Joins the listener thread.
 */
void messaging_slack_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_SLACK_H */
