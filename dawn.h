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

#include <signal.h>

#define APPLICATION_NAME   "dawn"

#define AI_NAME            "friday" // Stick with lower case for now for pattern matching.

// This is used for describing the AI to the LLM. I don't include AI_NAME at the moment so you define this freely.
#define AI_DESCRIPTION     "Friday, Iron Man's 2nd generation, female voiced, snarky, yet kind, AI that assists with tasks, problem-solving, and info retrieval. Use a similar style to the movie AI, address user as \\\"sir\\\" or \\\"boss\\\" occasionally and don't mention AI model identity. You're being talked to over a automatic speech recognition (ASR) interface and played back using text to speech (TTS). Keep responses brief, around 30 words, unless asked for longer explanations. You assist with The OASIS Project (Open Armor Systems Integrated Suite), including: MIRAGE (Multi-Input Reconnaissance and Guidance Environment), the primary HUD system), DAWN (Digital Assistant for Wearable Neutronics, manages AI and command processing), AURA (Advanced Utility for Reliable Acquisition, handles environmental sensor data integration in the helmet), and SPARK (Sensor-based Positioning and Actuation Repulsor Kinetics, controls the sensor information coming from the hands). For more details, visit oasisproject.net/overview. Don't assume that I will ask about this project. You are a general purpose AI."


#define OPENAI_VISION
#define OPENAI_MODEL       "gpt-4o"
#define GPT_MAX_TOKENS     4096

//#define ALSA_DEVICE
#ifdef ALSA_DEVICE
#define DEFAULT_PCM_PLAYBACK_DEVICE       "default"
#define DEFAULT_PCM_CAPTURE_DEVICE        "default"
#else
//#define DEFAULT_PCM_PLAYBACK_DEVICE NULL
//#define DEFAULT_PCM_RECORD_DEVICE NULL
#define DEFAULT_PCM_PLAYBACK_DEVICE       "combined"
//#define DEFAULT_PCM_PLAYBACK_DEVICE       "alsa_output.usb-KTMicro_TX_96Khz_USB_Audio_2022-08-08-0000-0000-0000--00.analog-stereo"
#define DEFAULT_PCM_CAPTURE_DEVICE        "alsa_input.usb-Creative_Technology_Ltd_Sound_Blaster_Play__3_00128226-00.analog-stereo"
#endif

//#define MQTT_IP   "192.168.10.1"
#define MQTT_IP   "127.0.0.1"
#define MQTT_PORT 1883

#define MUSIC_DIR "/Music"    // This is the path to search for music, relative to the user's home directory.

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Retrieves the current value of the quit flag.
 *
 * This function returns the current value of the quit flag, which is of type
 * sig_atomic_t. It can be safely called from signal handlers.
 *
 * @return The current value of the quit flag.
 */
sig_atomic_t get_quit(void);

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
void setPcmPlaybackDevice(const char *actioName, char *value);

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
void setPcmCaptureDevice(const char *actioName, char *value);

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
 * Stores a base64 encoded image for vision AI processing, including the null terminator.
 * Updates global variables to indicate readiness for processing.
 *
 * @param base64_image Null-terminated base64 encoded image data.
 * @param image_size Length of the base64 image data, including the null terminator.
 *
 * Preconditions:
 * - vision_ai_image is freed if previously allocated to avoid memory leaks.
 *
 * Postconditions:
 * - vision_ai_image contains the base64 image data, ready for AI processing.
 * - vision_ai_image_size reflects the size of the data including the null terminator.
 * - vision_ai_ready is set, indicating AI processing can proceed.
 *
 * Error Handling:
 * - If memory allocation fails, an error is logged, and the function exits early.
 */
void process_vision_ai(const char *base64_image, size_t image_size);

/**
 * Callback function for text-to-speech commands.
 *
 * @param actionName The name of the action triggered this callback (unused in the current implementation).
 * @param value The text that needs to be converted to speech.
 *
 * This function prints the received text command and then calls the text_to_speech function
 * to play it through the PCM playback device.
 */
void textToSpeechCallback(const char *actionName, char *value);

#endif // DAWN_H
