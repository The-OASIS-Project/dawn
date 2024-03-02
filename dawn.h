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

#ifndef DAWN_H
#define DAWN_H

#define APPLICATION_NAME   "dawn"

#define AI_NAME            "friday" // Stick with lower case for now for pattern matching.

// This is used for describing the AI to the LLM. I don't include AI_NAME at the moment so you define this freely.
#define AI_DESCRIPTION     "Friday, Iron Man's 2nd generation, female voiced, snarky, yet kind, AI that assists with tasks, problem-solving, and info retrieval. Use a similar style, address user as \\\"sir\\\" or \\\"boss\\\" occasionally and don't mention AI model identity. You're being talked to over a TTS interface and played back using text to audio. Keep responses brief, around 50 words, unless asked for longer explanations."

#define OPENAI_MODEL       "gpt-4-vision-preview"
#define GPT_MAX_TOKENS     1024

//#define ALSA_DEVICE
#ifdef ALSA_DEVICE
#define DEFAULT_PCM_PLAYBACK_DEVICE       "default"
#define DEFAULT_PCM_CAPTURE_DEVICE        "default"
#else
//#define DEFAULT_PCM_PLAYBACK_DEVICE NULL
//#define DEFAULT_PCM_RECORD_DEVICE NULL
#define DEFAULT_PCM_PLAYBACK_DEVICE       "combined"
#define DEFAULT_PCM_CAPTURE_DEVICE        "alsa_input.usb-Creative_Technology_Ltd_Sound_Blaster_Play__3_00128226-00.analog-stereo"
#endif

void drawWaveform(const int16_t *audioBuffer, size_t numSamples);
void textToSpeechCallback(const char *actionName, char *value);
void setPcmPlaybackDevice(const char *actioName, char *value);
const char *getPcmPlaybackDevice(void);
const char *getPcmCaptureDevice(void);
char *findAudioPlaybackDevice(char *name);
void setPcmCaptureDevice(const char *actioName, char *value);

#endif // DAWN_H
