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

#define MQTT_IP   "192.168.10.1"
#define MQTT_PORT 1883

#define MUSIC_DIR "/Music"    // This is the path to search for music, relative to the user's home directory.

//void drawWaveform(const int16_t *audioBuffer, size_t numSamples);

/* Setters and Getters for audio device. */
const char *getPcmPlaybackDevice(void);
const char *getPcmCaptureDevice(void);
void setPcmCaptureDevice(const char *actioName, char *value);
void setPcmPlaybackDevice(const char *actioName, char *value);

/* Find a playback device by name. */
char *findAudioPlaybackDevice(char *name);

void process_vision_ai(const char *base64_image, size_t image_size);
void textToSpeechCallback(const char *actionName, char *value);

#endif // DAWN_H
