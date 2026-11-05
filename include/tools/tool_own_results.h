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
 * The tools whose results are DAWN's own text (built from the user's data or
 * DAWN's own services, never someone else's), by name, for code that reads
 * stored tool results without the tool registry: the v113 migration, the
 * benchmark's stand-in registrations.  The same tools declare
 * TOOL_RESULTS_OWN in their metadata (scripts/check_tool_action_kinds.sh
 * keeps the two equal).  An action that differs is listed here too:
 * stocks' transactions carry memos others wrote.
 */

#ifndef TOOL_OWN_RESULTS_H
#define TOOL_OWN_RESULTS_H

/** The tools whose every result is DAWN's own, stored before v113 too. */
#define TOOL_OWN_RESULTS_ALWAYS_NAMES                                                              \
   "audio_device", "calculator", "date", "faceplate", "home_assistant", "hud_control", "hud_mode", \
       "llm_status", "memory", "music", "recording", "render_visual",                              \
       "render_visual_load_guidelines", "reset_conversation", "scheduler", "sfx", "shutdown",      \
       "stocks", "suit_status", "switch_llm", "system_status", "time", "tts", "visual_offset",     \
       "volume", "weather"

/** The tools whose own text goes unframed and someone else's in its frame
 *  (TOOL_FRAME_*): own only in a result stored since they frame it, so the
 *  v113 backfill (rows from before) doesn't count them. */
#define TOOL_OWN_RESULTS_FRAMED_NAMES \
   "calendar", "document_grep", "document_manage", "document_read", "document_search"

/** The names, as an initializer: `{ TOOL_OWN_RESULTS_NAMES }`. */
#define TOOL_OWN_RESULTS_NAMES TOOL_OWN_RESULTS_ALWAYS_NAMES, TOOL_OWN_RESULTS_FRAMED_NAMES

/** The tool and action whose results aren't, though the tool's are. */
#define TOOL_OWN_RESULTS_EXCEPT_TOOL "stocks"
#define TOOL_OWN_RESULTS_EXCEPT_ACTION "transactions"

#endif /* TOOL_OWN_RESULTS_H */
