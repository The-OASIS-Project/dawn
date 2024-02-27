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
char *getPcmPlaybackDevice(void);
char *findAudioPlaybackDevice(char *name);
void setPcmCaptureDevice(const char *actioName, char *value);
char *getPcmCaptureDevice(void);

#endif // DAWN_H
