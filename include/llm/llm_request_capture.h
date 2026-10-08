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
 * Request capture: writes the next LLM requests one user's turns send (URL,
 * headers with credentials redacted, body) to files an admin names, so the
 * LLM quality suite replays exactly what DAWN sends (dawn-admin llm capture).
 */

#ifndef LLM_REQUEST_CAPTURE_H
#define LLM_REQUEST_CAPTURE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct curl_slist;

/** Longest capture directory accepted (bytes, excluding the NUL). */
#define LLM_CAPTURE_DIR_MAX 200
/** Most requests one arming may capture. */
#define LLM_CAPTURE_COUNT_MAX 50
/** An arming stops on its own after this long (seconds). */
#define LLM_CAPTURE_TTL_SEC (30 * 60)

/**
 * @brief Capture the next @p count requests made for @p user_id
 *
 * Replaces any earlier arming, and stops on its own after LLM_CAPTURE_TTL_SEC.
 * Captures the LLM calls made on a thread working for one of @p user_id's
 * sessions: a turn's requests, and any side call a tool makes during it (a
 * search summary).  Background calls (memory extraction, compaction) run with
 * no session and are not captured.
 *
 * @param user_id The user whose turns to capture (> 0)
 * @param dir An existing, empty directory (absolute path, not a symlink) owned by
 *            the daemon's user and writable by no one else
 * @param count Requests to capture, 1..LLM_CAPTURE_COUNT_MAX
 * @param err Receives a reason on failure (may be NULL)
 * @param err_len Size of @p err
 * @return SUCCESS, or FAILURE with @p err set
 */
int llm_request_capture_arm(int user_id, const char *dir, int count, char *err, size_t err_len);

/** @brief Stop capturing. */
void llm_request_capture_disarm(void);

/** @brief Requests still to capture (0 when disarmed). */
int llm_request_capture_remaining(void);

/**
 * @brief Write this request to the capture directory, if armed for its user
 *
 * Call just before the request is sent.  The file is
 * <dir>/<NNN>-<provider>.json, created exclusively (no symlinks) with mode 0600:
 * {"provider", "url", "captured_at", "headers": [...], "body": <JSON>}.
 * Credential headers (Authorization, x-api-key, ...), URL userinfo and
 * credential URL parameters (key, api_key, token, ...) are written as
 * "[REDACTED]".  Cheap when disarmed.
 *
 * @param provider A short provider tag ("claude", "openai-chat", ...)
 * @param url The request URL
 * @param headers The request headers (may be NULL)
 * @param body The request body (JSON text)
 */
void llm_request_capture(const char *provider,
                         const char *url,
                         const struct curl_slist *headers,
                         const char *body);

#ifdef __cplusplus
}
#endif

#endif /* LLM_REQUEST_CAPTURE_H */
