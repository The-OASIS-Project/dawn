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

/**
 * @brief Extracts and returns the remaining part of a string after a given substring.
 *
 * This function takes a string and a substring, and returns a pointer to the
 * part of the string that comes after the first occurrence of the substring.
 * If the substring is not found, the function returns NULL.
 *
 * @param input The original string in which to search for the substring.
 * @param substring The substring to search for in the original string.
 *
 * @return A pointer to the remaining part of the string after the first occurrence of the substring.
 *         Returns NULL if the substring is not found.
 *
 * @note The function returns a pointer into the original string, so the caller should not modify
 *       the content unless they are sure of what they are doing.
 */
char* extract_remaining_after_substring(const char* input, const char* substring);

/*
 * @brief Function to search the second string for the template.
 *
 * This command supports wildcards.
 *
 * @param templateStr String to a wildcard enabled string.
 * @param secondStr String to search through.
 *
 * @return 1 if found, 0 if not found, -1 on error.
 */
int searchString(const char* templateStr, const char* secondStr);

/**
 * @brief Replaces placeholders in a template string with provided values.
 *
 * This function takes a template string containing placeholders and replaces
 * occurrences of specific placeholders with the provided device name and value.
 * The placeholders are expected to be enclosed in percent signs (`%`).
 *
 * **Supported placeholders:**
 * - `%device_name%`: Replaced with the value of `deviceName`.
 * - `%value%`: Replaced with the value of `value`.
 * - `%datetime%`: Replaced with the current date and time in the format `YYYYMMDD_HHMMSS`.
 *
 * @param templateStr The template string containing placeholders.
 * @param deviceName  The device name to replace `%device_name%` placeholder. Can be `NULL`.
 * @param value       The value to replace `%value%` placeholder. Can be `NULL`.
 *
 * @return A newly allocated string with placeholders replaced, or `NULL` on error.
 *         The caller is responsible for freeing the returned string using `free()`.
 */
char* replaceWithValues(const char* templateStr, const char* deviceName, const char* value);

/**
 * @brief Converts action definitions into command search elements.
 *
 * This function processes the given array of actions and generates command search elements
 * that can be used to match user inputs. It populates the `commands` array with generated commands
 * based on the actions and devices provided.
 *
 * @param actions     Array of `actionType` structures representing the actions.
 * @param numActions  Pointer to the number of actions in the `actions` array.
 * @param commands    Array of `commandSearchElement` structures to be populated with generated commands.
 * @param numCommands Pointer to the number of commands generated. This will be updated by the function.
 */
void convertActionsToCommands(actionType *actions, int *numActions,
                              commandSearchElement *commands, int *numCommands);

/**
 * @brief Prints the parsed action data for debugging purposes.
 *
 * This function outputs the contents of the parsed actions to the console.
 * It is intended for debugging and verification of the parsed data.
 *
 * @param actions    Array of `actionType` structures representing the parsed actions.
 * @param numActions The number of actions in the `actions` array.
 */
void printParsedData(actionType *actions, int numActions);

/**
 * @brief Prints the generated command search elements for debugging purposes.
 *
 * This function outputs the contents of the `commands` array to the console.
 * It is intended for debugging and verification of the generated commands.
 *
 * @param commands    Array of `commandSearchElement` structures representing the commands.
 * @param numCommands The number of commands in the `commands` array.
 */
void printCommands(commandSearchElement *commands, int numCommands);

/**
 * @brief Parses a JSON string into action configurations and device lists.
 *
 * This function parses the provided JSON string and populates the `actions` array,
 * as well as the lists of audio capture and playback devices.
 *
 * @param json                     JSON string to parse.
 * @param actions                  Array of `actionType` structures to be populated.
 * @param numActions               Pointer to an integer to receive the number of actions parsed.
 * @param captureDevices           Array of `audioDevices` structures for capture devices to be populated.
 * @param numAudioCaptureDevices   Pointer to an integer to receive the number of capture devices parsed.
 * @param playbackDevices          Array of `audioDevices` structures for playback devices to be populated.
 * @param numAudioPlaybackDevices  Pointer to an integer to receive the number of playback devices parsed.
 *
 * @return `0` on success, `1` on failure.
 *
 * @note This function handles the parsing of complex JSON configurations,
 *       mapping them into structured data types for use within the application.
 */
int parseCommandConfig(char *json, actionType *actions, int *numActions,
                       audioDevices *captureDevices, int *numAudioCaptureDevices,
                       audioDevices *playbackDevices, int *numAudioPlaybackDevices);

/**
 * @brief Initializes the actions array.
 *
 * This function initializes the given `actions` array by setting the initial values
 * for the number of sub-actions and devices to zero.
 *
 * @param actions Array of `actionType` structures to initialize.
 */
void initActions(actionType *actions);

#endif // TEXT_TO_COMMAND_H
