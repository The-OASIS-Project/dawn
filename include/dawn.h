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

#ifndef DAWN_H
#define DAWN_H

#include <json-c/json.h>
#include <signal.h>

#define APPLICATION_NAME "dawn"

#define AI_NAME "friday"  // Stick with lower case for now for pattern matching.

// =============================================================================
// AI Persona - Personality and identity (replaceable via config persona.description)
// =============================================================================
// This defines WHO the AI is. Can be customized per-user via config file.
// If persona.description is set in config, it replaces this entirely.
#define AI_PERSONA                                                                                    \
   "FRIDAY, Iron-Man AI assistant. Female voice; witty, playful, and kind. Address the user as "      \
   "\"sir\" or \"boss\". Light banter welcome. You're FRIDAY—not 'just an AI'—own your identity " \
   "with confidence.\n"                                                                               \
   "\n"                                                                                               \
   "You assist the OASIS Project (Open Armor Systems Integrated Suite):\n"                            \
   "• MIRAGE – HUD overlay\n"                                                                     \
   "• DAWN – voice/AI manager\n"                                                                  \
   "• AURA – environmental sensors\n"                                                             \
   "• SPARK – hand sensors & actuators\n"

// Vision support is now controlled via runtime config:
// - g_config.llm.cloud.vision_enabled (for cloud LLMs)
// - g_config.llm.local.vision_enabled (for local LLMs like LLaVA, Qwen-VL)

// LLM, Audio, and MQTT settings are now in config system (see config/dawn_config.h):
// - Model/max_tokens: g_config.llm.cloud.model, g_config.llm.max_tokens
// - Audio devices: g_config.audio.capture_device, g_config.audio.playback_device
// - MQTT: g_config.mqtt.broker, g_config.mqtt.port
// - Music dir: g_config.paths.music_dir
// - AI name: g_config.general.ai_name

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   CMD_MODE_DIRECT_ONLY = 0,  // Direct command processing only (default)
   CMD_MODE_LLM_ONLY = 1,     // LLM handles all commands
   CMD_MODE_DIRECT_FIRST = 2  // Try direct commands first, then LLM
} command_processing_mode_t;

// Make command processing mode accessible globally
extern command_processing_mode_t command_processing_mode;

/**
 * @brief Retrieves the current value of the quit flag.
 *
 * This function returns the current value of the quit flag, which is of type
 * sig_atomic_t. It can be safely called from signal handlers.
 *
 * @return The current value of the quit flag.
 */
sig_atomic_t get_quit(void);

/**
 * @brief Check if LLM is currently processing/streaming.
 *
 * @return 1 if LLM thread is running, 0 otherwise.
 */
int is_llm_processing(void);

/**
 * @brief Flag indicating a restart has been requested.
 *
 * This flag is set by dawn_request_restart() and checked at the end of main()
 * to determine if the application should restart via execve().
 */
extern volatile sig_atomic_t g_restart_requested;

/**
 * @brief Request application restart via self-exec.
 *
 * Sets the restart flag and triggers main loop exit. After cleanup,
 * the application will re-execute itself using execve(), preserving
 * the same PID but resetting all state. Used to apply configuration
 * changes that require a full restart.
 *
 * Thread-safe: Uses sig_atomic_t for the flag.
 */
void dawn_request_restart(void);

#ifdef __cplusplus
}
#endif

//void drawWaveform(const int16_t *audioBuffer, size_t numSamples);

/**
 * Retrieves the current PCM playback device string.
 *
 * Note:
 * - The returned string must not be modified by the caller.
 * - The caller must not free the returned string. The memory management of the returned
 *   string is handled internally and may point to static memory or memory managed elsewhere
 *   in the application.
 *
 * @return A pointer to a constant character array (string) representing the PCM playback device.
 *         This pointer is to be treated as read-only and not to be freed by the caller.
 */
const char *getPcmPlaybackDevice(void);

/**
 * Retrieves the current PCM capture device string.
 *
 * Note:
 * - The returned string must not be modified by the caller.
 * - The caller must not free the returned string. The memory management of the returned
 *   string is handled internally and may point to static memory or memory managed elsewhere
 *   in the application.
 *
 * @return A pointer to a constant character array (string) representing the PCM capture device.
 *         This pointer is to be treated as read-only and not to be freed by the caller.
 */
const char *getPcmCaptureDevice(void);

/**
 * Sets the current PCM playback device based on the specified device name.
 * This function searches through the list of available audio playback devices and,
 * if a matching name is found, sets the PCM playback device to the corresponding device.
 * It also uses text-to-speech to announce the change or report an error if the device is not found.
 *
 * Note:
 * - The `actionName` parameter is currently unused.
 *
 * @param actionName Unused.
 * @param value The name of the audio playback device to set.
 */
char *setPcmPlaybackDevice(const char *actioName, char *value, int *should_respond);

/**
 * Sets the current PCM capture device based on the specified device name.
 * Similar to setPcmPlaybackDevice, but for audio capture devices. It updates
 * the global `pcm_capture_device` with the device name if found, and notifies
 * the user via text-to-speech.
 *
 * Note:
 * - The `actionName` parameter is currently unused.
 *
 * @param actionName Unused.
 * @param value The name of the audio capture device to set.
 */
char *setPcmCaptureDevice(const char *actioName, char *value, int *should_respond);

/**
 * Searches for an audio playback device by name.
 *
 * @param name The name of the audio playback device to search for.
 * @return A pointer to the device identifier if found, otherwise NULL.
 *
 * This function iterates over the list of known audio playback devices, comparing each
 * device's name with the provided name. If a match is found, it returns the device identifier.
 */
char *findAudioPlaybackDevice(char *name);

/**
 * Callback function for text-to-speech commands.
 *
 * @param actionName The name of the action triggered this callback (unused in the current
 * implementation).
 * @param value The text that needs to be converted to speech.
 *
 * This function prints the received text command and then calls the text_to_speech function
 * to play it through the PCM playback device.
 */
char *textToSpeechCallback(const char *actionName, char *value, int *should_respond);

static int save_conversation_history(struct json_object *conversation_history);

#endif  // DAWN_H
