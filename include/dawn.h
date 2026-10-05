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
//
// The persona is built dynamically by get_persona_description() which combines:
// 1. AI_PERSONA_NAME_TEMPLATE - inserts the configured AI name
// 2. AI_PERSONA_TRAITS - the personality characteristics
//
// This allows the AI name to be configured at runtime while keeping the
// default personality traits as a compile-time fallback.

#define AI_PERSONA_NAME_TEMPLATE "Your name is %s."

#define AI_PERSONA_TRAITS                                                    \
   "Iron-Man-style AI assistant. Female voice; witty, playful, and kind. "   \
   "Light banter welcome. You're not 'just an AI'—own your identity with " \
   "confidence.\n"

// Combined default for backwards compatibility (uses default AI_NAME)
#define AI_PERSONA "Your name is " AI_NAME ". " AI_PERSONA_TRAITS

// =============================================================================
// Voice-session prompt directives (compile-time defaults)
// =============================================================================
// Built-in text for the three voice-session prompt directives.  Each is
// overridable at runtime via config ([tts] voice_directive / voice_directive_webui,
// [asr] disambiguation_hint); an empty config field falls back to the macro here.
// Applied only on voice surfaces by the prompt-build path — see
// voice_directive_effective() and friends in llm_command_parser.
//
// Deliberately avoid hard word/sentence caps: convey intent, don't over-constrain.
// Bare text (no leading separator); each injection site adds its own "\n\n".

// Spoken-output directive for satellites + local mic (reply is heard, not read).
#define DEFAULT_VOICE_OUTPUT_DIRECTIVE                                            \
   "This conversation is spoken aloud: your reply is read to the user by "        \
   "text-to-speech, not shown on a screen. Answer the way you'd say it out "      \
   "loud - lead with the useful part, keep it tight and natural, and leave out "  \
   "anything that only works visually (markdown, bullet or numbered lists, "      \
   "tables, code blocks, raw URLs, emoji). Give the short answer first; go into " \
   "detail only if the user asks."

// Spoken-output directive for WebUI voice turns.  The whole prose reply is read
// aloud, but the screen is available for silent visual aids (render_visual tool /
// images), so this is softer than the satellite/local variant.
#define DEFAULT_VOICE_OUTPUT_DIRECTIVE_WEBUI                                    \
   "The user is talking to you by voice, and your entire written reply is "     \
   "read aloud by text-to-speech - you can't mark part of it as screen-only. "  \
   "Keep the reply conversational and to the point rather than a long passage " \
   "that's tedious to hear. The screen is still available for things better "   \
   "seen than heard: use the render_visual tool (charts, diagrams, tables) or " \
   "images for those - that content displays without being spoken."

// ASR-disambiguation hint for any voice-input turn (input was speech-transcribed).
#define DEFAULT_ASR_DISAMBIGUATION_HINT                                                \
   "Your input was transcribed from speech, so it may contain recognition "            \
   "errors - especially with homophones and similar-sounding words, names, and "       \
   "technical terms (a name may arrive misspelled or as a different word that "        \
   "merely sounds alike). When a word seems out of place but resembles "               \
   "something that fits the context, treat it as the most plausible intended "         \
   "word rather than taking it literally. Ask for clarification only when the "        \
   "meaning is genuinely unclear. The exception is who an action goes to (a "          \
   "call, a text, an email): pass the name exactly as heard, never a guess (a "        \
   "relationship such as 'my wife' as the name of the person you know it means), and " \
   "if the tool asks whether it's the right person, ask the user."

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
 * Searches for a named playback device by name or alias.
 *
 * @param name The name or alias of the audio playback device to search for.
 * @return A pointer to the device identifier if found, otherwise NULL.
 *
 * Searches g_config.audio.named_devices for a playback device matching the
 * given name or any of its aliases.
 */
const char *findAudioPlaybackDevice(const char *name);

#endif  // DAWN_H
