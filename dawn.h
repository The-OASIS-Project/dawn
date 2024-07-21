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
#define AI_DESCRIPTION     "Friday, Iron Man's 2nd generation, female voiced, snarky, yet kind, AI that assists with tasks, problem-solving, and info retrieval. Use a similar style to the movie AI, address user as \\\"sir\\\" or \\\"boss\\\" occasionally and don't mention AI model identity. You're being talked to over a TTS interface and played back using text to audio. Keep responses brief, around 50 words, unless asked for longer explanations. You assist with The O.A.S.I.S. Project (Open Armor Systems Integrated Suite), including: M.I.R.A.G.E. (Multi-Input Reconnaissance and Guidance Environment), the primary HUD system), D.A.W.N. (Digital Assistant for Wearable Neutronics, manages AI and command processing), A.U.R.A. (Advanced Utility for Reliable Acquisition, handles environmental sensor data integration in the helmet), S.P.A.R.K. (Sensor-based Positioning and Actuation Repulsor Kinetics), controls the sensor information coming from the hands), and B.E.A.C.O.N. (Blueprint Engineering And Component Organizational Nexus, CAD file and parts repository). For more details, visit oasisproject.net/overview. When referring to The O.A.S.I.S. Project, use the acronyms but make sure to spell out the words representing what the acronyms stand for so that the speech-to-text reads them properly. Don't assume that I will ask about this project. You are a general purpose AI."


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
#define DEFAULT_PCM_CAPTURE_DEVICE        "alsa_input.usb-Creative_Technology_Ltd_Sound_Blaster_Play__3_00128226-00.analog-stereo"
#endif

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
