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
 * Cross-device utterance dedup — a content-free, session-keyed suppression
 * gate.  When the local mic and a nearby satellite hear one spoken command,
 * each session independently reaches the LLM and the user gets two answers.
 * This module gates that: the first source to call check() within a short
 * window wins and proceeds; any *different* session that calls check() inside
 * the window is told to suppress.  No transcript is ever compared — which also
 * catches cross-engine ASR divergence (Pi Whisper vs Jetson Whisper) and a
 * device whose ASR returns empty/garbage.  See
 * atlas/dawn/archive/CROSS_DEVICE_DEDUP_DESIGN.md.
 */

#ifndef UTTERANCE_DEDUP_H
#define UTTERANCE_DEDUP_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize the dedup gate with the suppression window.
 *
 * Safe to call once at startup.  A window of <= 0 disables the gate (every
 * check() returns false).  The module is a static singleton — there is no
 * handle to pass around.
 *
 * @note Pre-init safe: because the backing window is static-zero before this
 *       runs, any check() that races ahead of init() is treated as disabled
 *       (returns false).  Do not "fix" this by adding an init guard that
 *       suppresses — fail-open is the correct startup behavior.
 *
 * @param window_sec Suppression window in seconds (<= 0 disables).
 */
void utterance_dedup_init(int window_sec);

/**
 * @brief Live-retune the suppression window (e.g. from a WebUI set_config).
 *
 * @param window_sec New window in seconds (<= 0 disables).
 */
void utterance_dedup_set_window(int window_sec);

/**
 * @brief Check whether this session's utterance should be suppressed.
 *
 * Returns true when a *different* session produced a command event within the
 * window — the caller should drop its command and return to listening.  The
 * first source in a window is always allowed and never waits (zero added
 * latency on the responding path).  A same-session repeat ("next, next") is
 * always allowed.
 *
 * Suppressed events are deliberately NOT recorded, so the suppression window
 * anchors to the first/winning event and a suppressed device cannot later
 * suppress the winner's own deliberate repeat.
 *
 * @note Leaf lock: the internal mutex is acquired with NO session or global
 *       lock held.  Call this only after releasing any such lock.
 *
 * @param session_id Discriminator (LOCAL_SESSION_ID for the local mic,
 *                   nonzero per WebUI/satellite session).
 * @return true to suppress (a different source already fired), false to allow.
 */
bool utterance_dedup_check(uint32_t session_id);

/**
 * @brief Test seam: override the clock used for window math.
 *
 * @note The injected clock MUST be monotonic non-decreasing. The window-expiry
 *       math uses unsigned subtraction (`now - ts_ms`) which relies on
 *       `now >= ts_ms`; a clock that steps backward wraps the subtraction and
 *       fails open (a slot is treated as expired, suppression silently stops).
 *       The default clock is CLOCK_MONOTONIC, which satisfies this.
 *
 * @param fn Function returning a monotonic millisecond timestamp, or NULL to
 *           restore the default (CLOCK_MONOTONIC milliseconds).
 */
void utterance_dedup_set_clock(uint64_t (*fn)(void));

/**
 * @brief Test seam: clear all recorded events (isolation between cases).
 */
void utterance_dedup_reset_all(void);

#endif /* UTTERANCE_DEDUP_H */
