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
 * All contributions to this project are agreed to be licensed under the
 * GPLv3 or any later version. Contributions are understood to be
 * any modifications, enhancements, or additions to the project
 * and become the property of the original author Kris Kersey.
 */

#ifndef TEXT_TO_COMMAND_H
#define TEXT_TO_COMMAND_H

#define MAX_WORD_LENGTH          256
#define MAX_COMMAND_LENGTH       512
#define MAX_SUBACTIONS           10
#define MAX_ACTIONS              10
#define MAX_DEVICES_PER_ACTION   10
#define MAX_WORDS                10

#define MAX_AUDIO_DEVICES        5

#define MAX_COMMANDS             1000

#define CONFIG_FILE              "commands_config_nuevo.json"

/**
 * @brief This is the information on a device that can be controlled.
 *
 * The information in this struct applies to one device type. This is a sub-
 * struct to the actionType struct to make the command easily processed based
 * on command order.
 */
typedef struct {
   char name[MAX_WORD_LENGTH];               /**< Name of the device. */
   char aliases[MAX_WORDS][MAX_WORD_LENGTH]; /**< Other names that someone may call this device. */
   int numAliases;                           /**< How many aliases have we added. */
   char unit[MAX_WORD_LENGTH];               /**< Units that this device's settings may be given in. */
   char topic[MAX_WORD_LENGTH];
} commandDevice;

/**
 * @brief These are the sub-actions of and action type.
 *
 * Each action type may have different actions that can be taken upon it.
 * This is how we refer to them.
 */
typedef struct {
   char name[MAX_WORD_LENGTH];               /**< Name of the sub-action. */
   char actionWords[MAX_WORDS][MAX_WORD_LENGTH];   /**< How may we refer to this action. */
   int numActionWords;                       /** How many action words have we added. */
   char actionCommand[MAX_WORD_LENGTH];      /** What command to we send once we process this action. */
} commandAction;

/**
 * @brief This is the top-level struct for processing actions.
 *
 * Each type of action will be here. Under each action will be be the
 * different sub-actions that can be performed and devices that are of the type.
 */
typedef struct {
   char name[MAX_WORD_LENGTH];               /**< Name of the action type. */
   commandAction subActions[MAX_SUBACTIONS]; /**< Each sub-action this action supports. */
   int numSubActions;                        /**< How many sub-actions have we added. */
   commandDevice devices[MAX_DEVICES_PER_ACTION];  /**< Each device that this action can be apply to. */
   int numDevices;                           /**< How many devices are of this type. */
} actionType;

/**
 * @brief Data structure that reformats the input in a ready-to-process format.
 *
 * The way we read this information in from the config file is not optimal for
 * processing commands. So we filter the "action_words" into regular
 * expressions, organize them into their own array, and provide the command
 * we need to execute.
 */
typedef struct {
   char actionWordsWildcard[MAX_COMMAND_LENGTH];
   char actionWordsRegex[MAX_COMMAND_LENGTH];
   char actionCommand[MAX_COMMAND_LENGTH];
   char topic[MAX_WORD_LENGTH];
} commandSearchElement;

typedef enum {AUDIO_DEVICE_UNKNOWN, AUDIO_DEVICE_CAPTURE, AUDIO_DEVICE_PLAYBACK} adType;  /**< For labeling the type of device during searches. */
#define AUDIO_DEVICE_CAPTURE_STRING    "audio capture device"
#define AUDIO_DEVICE_PLAYBACK_STRING   "audio playback device"

/**
 * @brief This is the information on audio devices that can be selected.
 *
 */
typedef struct {
   adType type;                              /**< Type of audio device. This will be the same for each in the array. */
   char name[MAX_WORD_LENGTH];               /**< Name of the audio device. */
   char aliases[MAX_WORDS][MAX_WORD_LENGTH]; /**< Other names that someone may call this audio device. */
   int numAliases;                           /**< How many aliases have we added. */
   char device[MAX_WORD_LENGTH];             /**< Audio device name. May be an ALSA device or Pulseaudio device.  */
} audioDevices;

char* extract_remaining_after_substring(const char* input, const char* substring);
int searchString(const char* templateStr, const char* secondStr);
char* replaceWithValues(const char* templateStr, const char* deviceName, const char* value);
void convertActionsToCommands(actionType *actions, int *numActions,
                              commandSearchElement *commands, int *numCommands);
void printParsedData(actionType *actions, int numActions);
void printCommands(commandSearchElement *commands, int numCommands);
int parseCommandConfig(char *json, actionType *actions, int *numActions,
                       audioDevices *captureDevices, int *numAudioCaptureDevices,
                       audioDevices *playbackDevices, int *numAudioPlaybackDevices);
void initActions(actionType *actions);

#endif // TEXT_TO_COMMAND_H
