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
 * A turn's result beyond its text (llm_interface.h): what a provider reports
 * about the turn's requests, taken by the caller once the turn's call
 * returns, on the same thread (like llm_last_error()).  Provider-neutral, so
 * a session never reads a provider's own state.
 */

#include <stdbool.h>

#include "llm/llm_claude_betas.h"
#include "llm/llm_interface.h"

static __thread bool t_inline_tools_rejected = false;

void llm_turn_result_reset(void) {
   t_inline_tools_rejected = false;
   /* The provider's own per-turn state goes with it: this turn's requests
    * render as its conversation says, whatever an earlier one hit. */
   (void)claude_betas_take_inline_rejected();
}

void llm_note_inline_tools_rejected(void) {
   t_inline_tools_rejected = true;
}

bool llm_take_inline_tools_rejected(void) {
   const bool rejected = t_inline_tools_rejected;
   t_inline_tools_rejected = false;
   return rejected;
}
