/**
 * @file state_machine.h
 * @brief DAWN state machine definitions
 *
 * This file is part of the DAWN Voice Assistant project.
 *
 * DAWN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DAWN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DAWN. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

/**
 * @enum dawn_state_t
 * Enum representing the possible states of Dawn's listening process.
 *
 * @var DAWN_STATE_SILENCE
 * The AI is not actively listening or processing commands.
 * It's waiting for a noise threshold to be exceeded.
 *
 * @var DAWN_STATE_WAKEWORD_LISTEN
 * The AI is listening for a wake word to initiate interaction.
 *
 * @var DAWN_STATE_COMMAND_RECORDING
 * The AI is recording a command after recognizing a wake word.
 *
 * @var DAWN_STATE_PROCESS_COMMAND
 * The AI is processing a recorded command.
 *
 * @var DAWN_STATE_INVALID
 * Invalid state marker (used as array size sentinel).
 */
typedef enum {
   DAWN_STATE_SILENCE,
   DAWN_STATE_WAKEWORD_LISTEN,
   DAWN_STATE_COMMAND_RECORDING,
   DAWN_STATE_PROCESS_COMMAND,
   DAWN_STATE_INVALID
} dawn_state_t;

/**
 * @brief Get the string name of a state.
 *
 * @param state The state to get the name of.
 * @return const char* The human-readable name of the state.
 */
static inline const char *dawn_state_name(dawn_state_t state) {
   switch (state) {
      case DAWN_STATE_SILENCE:
         return "SILENCE";
      case DAWN_STATE_WAKEWORD_LISTEN:
         return "WAKEWORD_LISTEN";
      case DAWN_STATE_COMMAND_RECORDING:
         return "COMMAND_RECORDING";
      case DAWN_STATE_PROCESS_COMMAND:
         return "PROCESS_COMMAND";
      case DAWN_STATE_INVALID:
      default:
         return "UNKNOWN";
   }
}

#endif /* STATE_MACHINE_H */
