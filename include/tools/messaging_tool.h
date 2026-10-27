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
 * Messaging LLM tool — exposes messaging_engine to the LLM.  See
 * docs/MESSAGING_CHANNELS_DESIGN.md §3.
 */
#ifndef MESSAGING_TOOL_H
#define MESSAGING_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the messaging LLM tool with the tool registry.
 *
 * Called from `tools_register_all()`.  The tool's init starts the messaging
 * engine (`messaging_engine_init()`) and the SMS driver (always — ECHO owns
 * the modem connection lifecycle); its start, once the daemon is up, registers
 * the Telegram/Discord/Slack drivers whose tokens are set in secrets.toml.
 * See `docs/MESSAGING_CHANNELS_DESIGN.md` §3.
 *
 * @return SUCCESS on successful registration, FAILURE on engine init
 *         or registry failure.  Driver registration failures are
 *         logged but non-fatal (the LLM tool still works; absent
 *         drivers surface as `MESSAGING_DRIVER_NOT_REGISTERED` errors
 *         on send).
 */
int messaging_tool_register(void);

/**
 * @brief Start any token-gated messaging driver whose token is now set but which
 *        isn't already registered.
 *
 * Called after the WebUI saves secrets, so adding a Telegram/Discord/Slack bot
 * token starts its driver live without a daemon restart.  Additive only: it
 * never tears down a running driver, so rotating or removing a token remains
 * daemon-restart-to-apply (live teardown would join a long-poll listener and the
 * engine has no clean per-driver unregister path).  Safe to call on any secrets
 * save — already-running drivers are skipped.  Before the tool's start phase it
 * does nothing: the start registers every driver whose token is set.
 *
 * @note Thread safety: typically called from the lws service thread (set_secrets).
 *       The shared registration state is mutex-guarded, so it is safe even if it
 *       races the start phase.
 */
void messaging_tool_refresh_drivers(void);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_TOOL_H */
