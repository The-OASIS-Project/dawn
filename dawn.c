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

/* Std C */
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* JSON */
#include <json-c/json.h>

/* CURL */
#include <curl/curl.h>

/* Mosquitto */
#include <mosquitto.h>

/* Speech to Text */
#include "vosk_api.h"

/* Local */
#include "audio_utils.h"
#include "dawn.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "openai.h"
#include "text_to_command_nuevo.h"
#include "text_to_speech.h"
#include "version.h"

// Define the default sample rate for audio capture.
#define DEFAULT_RATE             44100

// Define the default number of audio channels (1 for mono).
#define DEFAULT_CHANNELS         1

// Define the default duration of audio capture in seconds.
#define DEFAULT_CAPTURE_SECONDS  0.5f

// Define the default command timeout in terms of iterations of DEFAULT_CAPTURE_SECONDS.
#define DEFAULT_COMMAND_TIMEOUT  2

// Define the duration for background audio capture in seconds.
#define BACKGROUND_CAPTURE_SECONDS  6

// Check if ALSA_DEVICE is defined to include ALSA-specific headers and define ALSA-specific macros.
#ifdef ALSA_DEVICE
#include <alsa/asoundlib.h>

// Define the default ALSA PCM access type (read/write interleaved).
#define DEFAULT_ACCESS     SND_PCM_ACCESS_RW_INTERLEAVED

// Define the default ALSA PCM format (16-bit signed little-endian).
#define DEFAULT_FORMAT     SND_PCM_FORMAT_S16_LE

// Define the default number of frames per ALSA PCM period.
#define DEFAULT_FRAMES     64
#else
// Include PulseAudio simple API and error handling headers for non-ALSA configurations.
#include <pulse/simple.h>
#include <pulse/error.h>

// Define the default PulseAudio sample format (16-bit signed little-endian).
#define DEFAULT_PULSE_FORMAT  PA_SAMPLE_S16LE
#endif

// Define the threshold offset for detecting talking in the audio stream.
#define TALKING_THRESHOLD_OFFSET 0.015

static char pcm_capture_device[MAX_WORD_LENGTH + 1] = "";
static char pcm_playback_device[MAX_WORD_LENGTH + 1] = "";

/* Parsed audio devices. */
static audioDevices captureDevices[MAX_AUDIO_DEVICES];   /**< Audio capture devices. */
static int numAudioCaptureDevices = 0;                   /**< How many capture devices. */

static audioDevices playbackDevices[MAX_AUDIO_DEVICES];  /**< Audio playback devices. */
static int numAudioPlaybackDevices = 0;                  /**< How many playback devices. */

/**
 * @struct audioControl
 * @brief Manages audio capture settings and state for either ALSA or PulseAudio systems.
 *
 * This structure abstracts the specific audio system being used, allowing the rest of the
 * code to interact with audio hardware in a more uniform way. It must be initialized with
 * the appropriate settings for the target audio system before use.
 *
 * @var audioControl::handle
 * Pointer to the ALSA PCM device handle.
 *
 * @var audioControl::frames
 * Number of frames for ALSA to capture in each read operation.
 *
 * @var audioControl::pa_handle
 * Pointer to the PulseAudio simple API handle.
 *
 * @var audioControl::pa_framesize
 * Size of the buffer (in bytes) for PulseAudio to use for each read operation.
 *
 * @var audioControl::full_buff_size
 * Size of the buffer to be filled in each read operation, common to both ALSA and PulseAudio.
 */
typedef struct {
#ifdef ALSA_DEVICE
   snd_pcm_t *handle;
   snd_pcm_uframes_t frames;
#else
   pa_simple *pa_handle;
   size_t pa_framesize;
#endif

   uint32_t full_buff_size;
} audioControl;

#ifndef ALSA_DEVICE
static const pa_sample_spec sample_spec = {
   .format = DEFAULT_PULSE_FORMAT,
   .rate = DEFAULT_RATE,
   .channels = DEFAULT_CHANNELS
};
#endif

// Holds the current RMS (Root Mean Square) level of the background audio.
// Used to monitor ambient noise levels and potentially adjust listening sensitivity.
static double backgroundRMS = 0.0;

// Holds the current RMS (Root Mean Square) level of the background audio.
// Used to monitor ambient noise levels and potentially adjust listening sensitivity.
static char *wakeWords[] = {
   "hello " AI_NAME,
   "okay " AI_NAME,
   "alright " AI_NAME,
   "hey " AI_NAME,
   "hi " AI_NAME,
   "good evening " AI_NAME,
   "good day " AI_NAME,
   "good morning " AI_NAME
};

// Array of words/phrases used to signal the end of an interaction with the AI.
static char *goodbyeWords[] = {
   "good bye",
   "goodbye",
   "good night",
   "bye",
   "quit",
   "exit"
};

// Array of predefined responses the AI can use upon recognizing a wake word/phrase.
const char* wakeResponses[] = {
   "Hello Sir.",
   "At your service Sir.",
   "Yes Sir?",
   "How may I assist you Sir?",
   "Listening Sir."
};

// Array of words/phrases that the AI should explicitly ignore during interaction.
// This includes common filler words or phrases signaling to disregard the prior input.
static char *ignoreWords[] = {
   "",
   "the",
   "cancel",
   "never mind",
   "nevermind",
   "ignore"
};

// Array of words/phrases that we accept as a way to tell the AI to cancel its current
// text to speech instead of requiring another command.
static char *cancelWords[] = {
   "stop",
   "stop it",
   "cancel",
   "hold on",
   "wait",
   "never mind",
   "abort",
   "pause",
   "enough",
   "disregard",
   "no thanks",
   "forget it",
   "leave it",
   "drop it",
   "stand by",
   "cease",
   "interrupt",
   "say no more",
   "shut up",
   "silence",
   "zip it",
   "enough already",
   "that's enough",
   "stop right there"
};

// Standard greeting messages based on the time of day.
const char* morning_greeting = "Good morning boss.";
const char* day_greeting = "Good day Sir.";
const char* evening_greeting = "Good evening Sir.";

/**
 * @enum listeningState
 * Enum representing the possible states of Dawn's listening process.
 *
 * @var SILENCE
 * The AI is not actively listening or processing commands.
 * It's waiting for a noise threshold to be exceeded.
 *
 * @var WAKEWORD_LISTEN
 * The AI is listening for a wake word to initiate interaction.
 *
 * @var COMMAND_RECORDING
 * The AI is recording a command after recognizing a wake word.
 *
 * @var PROCESS_COMMAND
 * The AI is processing a recorded command.
 *
 * @var VISION_AI_READY
 * Indicates that the vision AI component is ready for processing.
 */
typedef enum {
   SILENCE,
   WAKEWORD_LISTEN,
   COMMAND_RECORDING,
   PROCESS_COMMAND,
   VISION_AI_READY,
   INVALID_STATE
} listeningState;

// Define the shared variables for tts state
pthread_cond_t tts_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t tts_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile sig_atomic_t tts_playback_state = TTS_PLAYBACK_IDLE;

/**
 * @var static char *vision_ai_image
 * Pointer to a buffer containing the latest image captured for vision AI processing.
 * Initially set to NULL and should be allocated when an image is captured.
 */
static char *vision_ai_image = NULL;

/**
 * @var static int vision_ai_image_size
 * Size of the buffer pointed to by vision_ai_image, representing the image size in bytes.
 * Initially set to 0 and updated upon capturing an image.
 */
static int vision_ai_image_size = 0;

/**
 * @var static int vision_ai_ready
 * Flag indicating whether the vision AI component is ready for image processing.
 * Set to 1 when ready, 0 otherwise.
 */
static int vision_ai_ready = 0;

/**
 * @var volatile sig_atomic_t quit
 * @brief Global flag indicating the application should quit.
 *
 * This flag is set to 1 when a SIGINT signal is received, signaling the
 * main loop to terminate and allow for a graceful exit. The use of
 * `volatile sig_atomic_t` ensures that the variable is updated atomically
 * and prevents the compiler from applying unwanted optimizations, considering
 * it may be altered asynchronously by signal handling.
 */
volatile sig_atomic_t quit = 0;

/* MQTT */
static struct mosquitto *mosq;

sig_atomic_t get_quit(void) {
    return quit;
}

#if 0
// Define the function to draw the waveform using SDL
void drawWaveform(const int16_t *audioBuffer, size_t numSamples) {
    // Clear the screen
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    // Define waveform colors (e.g., green lines)
    SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);

    // Determine the dimensions and position of the waveform within the window
    int waveformPosX = 0; // Adjust as needed
    int waveformPosY = 50; // Adjust as needed
    int waveformWidth = 0; // Adjust as needed
    int waveformHeight = 0; // Adjust as needed
    SDL_GetWindowSize(win, &waveformWidth, &waveformHeight);
    waveformPosY = waveformPosY + (waveformHeight - waveformPosY*2) / 2;

    // Calculate the step size to sample audio data and draw the waveform
    int stepSize = numSamples / waveformWidth;
    if (stepSize <= 0) {
        stepSize = 1;
    }

    // Scale factor for the waveform's height
    float scaleFactor = (float)waveformHeight / INT16_MAX;

    // Draw the waveform based on the audio buffer data
    int prevX = waveformPosX;
    int prevY = waveformPosY + (int)(audioBuffer[0] * scaleFactor);
    for (size_t i = 1; i < numSamples; i += stepSize) {
        int x = waveformPosX + waveformWidth * i / numSamples;
        int y = waveformPosY + (int)(audioBuffer[i] * scaleFactor);

        SDL_RenderDrawLine(ren, prevX, prevY, x, y);

        prevX = x;
        prevY = y;
    }

    // Update the screen
    SDL_RenderPresent(ren);
}
#endif

/**
 * @fn void signal_handler(int signal)
 * @brief Signal handler for SIGINT.
 *
 * This function is designed to handle the SIGINT signal (typically generated
 * by pressing Ctrl+C). When the signal is received, it sets the global
 * `quit` flag to 1, indicating to the application that it should exit.
 *
 * @param signal The signal number received, expected to be SIGINT.
 */
void signal_handler(int signal) {
    if (signal == SIGINT) {
        quit = 1;
    }
}

void textToSpeechCallback(const char *actionName, char *value) {
   LOG_INFO("Received text to speech command: \"%s\"\n", value);
   text_to_speech(value);
}

const char *getPcmPlaybackDevice(void) {
   return (const char*) pcm_playback_device;
}

const char *getPcmCaptureDevice(void) {
   return (const char*) pcm_capture_device;
}

char *findAudioPlaybackDevice(char *name) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];

   for (i = 0; i < numAudioPlaybackDevices; i++) {
      if (strcmp(playbackDevices[i].name, name) == 0) {
         return playbackDevices[i].device;
      }
   }

   return NULL;
}

void setPcmPlaybackDevice(const char *actionName, char *value) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];

   for (i = 0; i < numAudioPlaybackDevices; i++) {
      if (strcmp(playbackDevices[i].name, value) == 0) {
         LOG_INFO("Setting audio playback device to \"%s\".\n", playbackDevices[i].device);
         strncpy(pcm_playback_device, playbackDevices[i].device, MAX_WORD_LENGTH);
         pcm_playback_device[MAX_WORD_LENGTH] = '\0'; // Ensure null termination
         snprintf(speech, MAX_COMMAND_LENGTH, "Switching playback device to %s.", value);
         text_to_speech(speech);
         break;
      }
   }

   if (i >= numAudioPlaybackDevices) {
      LOG_ERROR("Requested audio playback device not found.\n");
      snprintf(speech, MAX_COMMAND_LENGTH, "Sorry sir. A playback devices called %s was not found.", value);
      text_to_speech(speech);
   }
}

void setPcmCaptureDevice(const char *actionName, char *value) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];

   for (i = 0; i < numAudioCaptureDevices; i++) {
      if (strcmp(captureDevices[i].name, value) == 0) {
         LOG_INFO("Setting audio capture device to \"%s\".\n", captureDevices[i].device);
         strncpy(pcm_capture_device, captureDevices[i].device, MAX_WORD_LENGTH);
         pcm_capture_device[MAX_WORD_LENGTH] = '\0'; // Ensure null termination
         snprintf(speech, MAX_COMMAND_LENGTH, "Switching capture device to %s.", value);
         text_to_speech(speech);
         break;
      }
   }

   if (i >= numAudioCaptureDevices) {
      LOG_ERROR("Requested audio capture device not found.\n");
      snprintf(speech, MAX_COMMAND_LENGTH, "Sorry sir. A capture devices called %s was not found.", value);
      text_to_speech(speech);
   }
}

/**
 * Measures the RMS value of background audio for a predefined duration.
 * This function supports both ALSA and PulseAudio backends, determined at compile time.
 * It captures audio into a buffer, computes the RMS value, and stores it in a global variable.
 *
 * Note: This function is designed to be run in a separate thread, not required, taking a pointer
 * to an audioControl structure as its argument. This structure must be properly initialized
 * before calling this function.
 *
 * @param audHandle A void pointer to an audioControl structure containing audio capture settings.
 * @return NULL always, indicating the thread's work is complete.
 */
void *measureBackgroundAudio(void *audHandle)
{
   audioControl *myControl = (audioControl *) audHandle;

   // Allocate Audio Buffers based on the backend and specified parameters.
   char *buff = (char *)malloc(myControl->full_buff_size);
   if (buff == NULL) {
      LOG_ERROR("malloc() failed on buff.\n");
      return NULL; // Early return on allocation failure for buff.
   }

   uint32_t max_buff_size = // Calculate maximum buffer size based on backend.
      DEFAULT_RATE * DEFAULT_CHANNELS * sizeof(int16_t) * BACKGROUND_CAPTURE_SECONDS;

   char *max_buff = (char *)malloc(max_buff_size);
   if (max_buff == NULL) {
      LOG_ERROR("malloc() failed on max_buff.\n");
      free(buff); // Ensure buff is freed to avoid a memory leak.
      return NULL; // Early return on allocation failure for max_buff.
   }

   uint32_t buff_size = 0;
   int rc = 0, error = 0;

#ifdef ALSA_DEVICE
   // ALSA audio capture and RMS calculation loop.
   while (1) {
      rc = snd_pcm_readi(myControl->handle, buff, myControl->frames);
      if ((rc > 0) && (buff_size + myControl->full_buff_size <= max_buff_size)) {
         memcpy(max_buff + buff_size, buff, myControl->full_buff_size);
         buff_size += myControl->full_buff_size;
      } else {
         if (rc <= 0) {
            LOG_ERROR("Error reading PCM.\n");
         }
         break; // Exit loop on read error or buffer full.
      }
   }
#else
   pa_simple_flush(myControl->pa_handle, NULL);
   // PulseAudio audio capture loop.
   for (size_t i = 0; i < max_buff_size / myControl->full_buff_size; ++i) {
      if (pa_simple_read(myControl->pa_handle, buff, myControl->pa_framesize, &error) < 0) {
         LOG_ERROR("Could not read audio: %s\n", pa_strerror(error));
         break; // Exit loop on read error.
      }
      memcpy(max_buff + buff_size, buff, myControl->full_buff_size);
      buff_size += myControl->full_buff_size;
   }
#endif

   // Compute RMS for captured audio.
   double rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));
   LOG_INFO("RMS of background recording is %g.\n", rms);
   backgroundRMS = rms; // Store RMS value in a global variable.

   // Clean up allocated buffers.
   free(buff);
   free(max_buff);

   return NULL;
}

/**
 * Parses a JSON string to extract the value of the "text" field.
 *
 * @param input A JSON string expected to contain a "text" field.
 * @return A dynamically allocated string containing the value of the "text" field.
 *         The caller is responsible for freeing this string.
 *         Returns NULL on error, including JSON parsing errors, missing "text" field,
 *         or memory allocation failures.
 */
char *getTextResponse(const char *input) {
   struct json_object *parsed_json;
   struct json_object *text_object;
   char *return_text = NULL;

   // Parse the JSON data
   parsed_json = json_tokener_parse(input);
   if (parsed_json == NULL) {
      LOG_ERROR("Error: Unable to process text response.\n");
      return NULL;
   }

   // Get the "text" object from the JSON
   if (json_object_object_get_ex(parsed_json, "text", &text_object)) {
      const char *input_text = json_object_get_string(text_object);
      if (input_text == NULL) {
         LOG_ERROR("Error: Unable to get string from input text.\n");
         json_object_put(parsed_json);
         return NULL;
      }

      return_text = malloc((strlen(input_text) + 1) * sizeof(char));
      if (return_text == NULL) {
         LOG_ERROR("malloc() failed in getTextResponse().\n");
         json_object_put(parsed_json);
         return NULL;
      }

      // Directly copy the input text into the return buffer
      strcpy(return_text, input_text);

      // Debugging: Print the extracted text
      LOG_INFO("Input Text: %s\n", return_text);
   } else {
      LOG_ERROR("Error: 'text' field not found in JSON.\n");
   }

   // Cleanup and return
   json_object_put(parsed_json);
   return return_text;
}

#ifdef ALSA_DEVICE
/**
 * Opens an ALSA PCM capture device and configures it with default hardware parameters.
 *
 * This function initializes an ALSA PCM capture device using specified settings for audio capture.
 * It sets parameters such as the audio format, rate, channels, and period size to defaults defined elsewhere.
 *
 * @param handle Pointer to a snd_pcm_t pointer where the opened PCM device handle will be stored.
 * @param pcm_device String name of the PCM device to open (e.g., "default" or a specific hardware device).
 * @param frames Pointer to a snd_pcm_uframes_t variable where the period size in frames will be stored.
 *
 * @return 0 on success, 1 on error, with an error message printed to stderr.
 */
int openAlsaPcmCaptureDevice(snd_pcm_t **handle, char *pcm_device, snd_pcm_uframes_t *frames)
{
   snd_pcm_hw_params_t *params = NULL;
   unsigned int rate = DEFAULT_RATE;
   int dir = 0;
   *frames = DEFAULT_FRAMES;
   int rc = 0;

   LOG_INFO("ALSA CAPTURE DRIVER\n");

   /* Open PCM device for playback. */
   rc = snd_pcm_open(handle, pcm_device, SND_PCM_STREAM_CAPTURE, 0);
   if (rc < 0) {
      LOG_ERROR("Unable to open pcm device for capture (%s): %s\n", pcm_device, snd_strerror(rc));
      return 1;
   }

   snd_pcm_hw_params_alloca(&params);
   snd_pcm_hw_params_any(*handle, params);
   snd_pcm_hw_params_set_access(*handle, params, DEFAULT_ACCESS);
   snd_pcm_hw_params_set_format(*handle, params, DEFAULT_FORMAT);
   snd_pcm_hw_params_set_channels(*handle, params, DEFAULT_CHANNELS);
   snd_pcm_hw_params_set_rate_near(*handle, params, &rate, &dir);
   LOG_INFO("Capture rate set to %u\n", rate);
   snd_pcm_hw_params_set_period_size_near(*handle, params, frames, &dir);
   LOG_INFO("Frames set to %lu\n", *frames);
   rc = snd_pcm_hw_params(*handle, params);
   if (rc < 0) {
      LOG_ERROR("Unable to set hw parameters: %s\n", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   return 0;
}
#else
/**
 * Opens a PulseAudio capture stream for a given PCM device.
 *
 * This function initializes a PulseAudio capture stream, using the PulseAudio Simple API,
 * for audio recording. It requires specifying the PCM device and uses predefined sample specifications
 * and application name defined elsewhere in the code.
 *
 * @param pcm_capture_device String name of the PCM capture device or NULL for the default device.
 *
 * @return A pointer to the initialized pa_simple structure representing the capture stream, or NULL on error,
 * with an error message printed to stderr.
 */
pa_simple *openPulseaudioCaptureDevice(char *pcm_capture_device)
{
   pa_simple *pa_handle = NULL;
   int rc = 0;

   LOG_INFO("PULSEAUDIO CAPTURE DRIVER: %s\n", pcm_capture_device);

   /* Create a new capture stream */
   if (!(pa_handle = pa_simple_new(NULL, APPLICATION_NAME, PA_STREAM_RECORD, pcm_capture_device, "record", &sample_spec, NULL, NULL, &rc))) {
      LOG_ERROR("Error opening PulseAudio record: %s\n", pa_strerror(rc));
      return NULL;
   }

   LOG_INFO("Capture opened successfully.\n");

   return pa_handle;
}
#endif

/**
 * Generates a greeting message based on the current time of day.
 *
 * This function checks the system's local time and selects an appropriate greeting
 * for morning, day, or evening. It uses predefined global strings for each time-specific
 * greeting, ensuring that the greeting is contextually relevant.
 *
 * @return A pointer to a constant character string containing the selected greeting.
 *         The return value points to a global variable and should not be modified or freed.
 */
const char* timeOfDayGreeting(void) {
   time_t t = time(NULL);
   struct tm *local_time = localtime(&t); // Obtain local time from the system clock.

   int hour = local_time->tm_hour; // Extract the hour component of the current time.

   // Select the appropriate greeting based on the hour of the day.
   if (hour >= 3 && hour < 12) {
      return morning_greeting; // Morning greeting from 3 AM to before 12 PM.
   } else if (hour >= 12 && hour < 18) {
      return day_greeting; // Day greeting from 12 PM to before 6 PM.
   } else {
      return evening_greeting; // Evening greeting for 6 PM onwards.
   }
}

/**
 * Selects a random acknowledgment response to a wake word detection.
 *
 * This function is designed to provide variability in the AI's response to
 * being activated by a wake word. It randomly selects one of the predefined
 * responses from the global `wakeResponses` array each time it's called.
 *
 * @return A pointer to a constant character string containing the selected wake word acknowledgment.
 *         The return value points to an element within the global `wakeResponses` array and
 *         should not be modified or freed.
 */
const char* wakeWordAcknowledgment() {
   int numWakeResponses = sizeof(wakeResponses) / sizeof(wakeResponses[0]); // Calculate the number of available responses.
   int choice;

   srand(time(NULL)); // Seed the random number generator.
   choice = rand() % numWakeResponses; // Generate a random index to select a response.

   return wakeResponses[choice]; // Return the randomly selected wake word acknowledgment.
}

/**
 * Captures audio into a buffer until it is full or an error occurs, managing its own local buffer.
 * 
 * This function abstracts the audio capture process to work with either ALSA or PulseAudio,
 * depending on compilation flags. It allocates a local buffer for audio data capture,
 * copies captured data into a larger buffer provided by the caller, and ensures
 * that the larger buffer does not overflow. The local buffer is dynamically allocated
 * and managed within the function.
 *
 * @param myAudioControls A pointer to an audioControl structure containing audio capture settings and state.
 * @param max_buff A pointer to the buffer where captured audio data will be accumulated.
 * @param max_buff_size The maximum size of max_buff, indicating how much data can be stored.
 * @param ret_buff_size A pointer to an integer where the function will store the total size of captured
 *                      audio data stored in max_buff upon successful completion.
 * @return Returns 0 on successful capture of audio data without filling the buffer.
 *         Returns 1 if a memory allocation failure occurs or an error occurred during audio capture,
 *         such as a failure to read from the audio device.
 *
 * @note The function updates *ret_buff_size with the total size of captured audio data.
 *       It's crucial to ensure that max_buff is large enough to hold the expected amount of data.
 */
int capture_buffer(audioControl *myAudioControls,
                   char *max_buff, uint32_t max_buff_size,
                   int *ret_buff_size) {
   int rc = 0; ///< Return code from audio read functions.
   int buffer_full = 0; ///< Flag to indicate if the buffer is full.
   int error = 0; ///< Error code for PulseAudio operations.

   *ret_buff_size = 0; // Initialize the returned buffer size to 0.

   // Allocate a local buffer for audio data capture.
   char *buff = (char *)malloc(myAudioControls->full_buff_size);
   if (buff == NULL) {
      LOG_ERROR("malloc() failed on buff.\n");
      return 1; // Return error code on memory allocation failure.
   }

   while (!buffer_full) { // Continue until the buffer is full or an error occurs.
#ifdef ALSA_DEVICE
      // Attempt to read audio frames using ALSA.
      rc = snd_pcm_readi(myAudioControls->handle, buff, myAudioControls->frames);
      if ((rc > 0) && (*ret_buff_size + myAudioControls->full_buff_size <= max_buff_size)) {
         // Copy the read data into max_buff if there's enough space.
         memcpy(max_buff + *ret_buff_size, buff, myAudioControls->full_buff_size);
         *ret_buff_size += myAudioControls->full_buff_size; // Update the size of captured data.
      } else {
         if (rc <= 0) {
            LOG_ERROR("Error reading PCM.\n");
            free(buff); // Free the local buffer on error.
            return 1; // Return error code on read failure.
         }
         buffer_full = 1; // Set buffer_full flag if max_buff is filled.
      }
#else
      // Attempt to read audio frames using PulseAudio.
      rc = pa_simple_read(myAudioControls->pa_handle, buff, myAudioControls->pa_framesize, &error);
      if ((rc == 0) && (*ret_buff_size + myAudioControls->full_buff_size <= max_buff_size)) {
         // Copy the read data into max_buff if there's enough space.
         memcpy(max_buff + *ret_buff_size, buff, myAudioControls->full_buff_size);
         *ret_buff_size += myAudioControls->full_buff_size; // Update the size of captured data.
      } else {
         if (rc < 0) {
            LOG_ERROR("pa_simple_read() failed: %s\n", pa_strerror(error));
            free(buff); // Free the local buffer on error.
            pa_simple_free(myAudioControls->pa_handle);
            myAudioControls->pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
            if (myAudioControls->pa_handle == NULL) {
               LOG_ERROR("Error creating Pulse capture device.\n");
            }
            return 1; // Return error code on read failure.
         }
         buffer_full = 1; // Set buffer_full flag if max_buff is filled.
      }
#endif
   }

   free(buff); // Free the local buffer before exiting.
   return 0; // Return success.
}

void process_vision_ai(const char *base64_image, size_t image_size) {
   if (vision_ai_image != NULL) {
      free(vision_ai_image);
      vision_ai_image = NULL;
   }

   vision_ai_image = malloc(image_size);
   if (!vision_ai_image) {
      LOG_ERROR("Error: Memory allocation failed.\n");
      return;
   }
   memcpy(vision_ai_image, base64_image, image_size);

   vision_ai_image_size = image_size;
   vision_ai_ready = 1;
}

listeningState currentState = INVALID_STATE;

/* Publish AI State. Only send state if it's changed.
 *
 * FIXME: Build this JSON correctly.
 *        Also pick a better topic for general purpose use.
 */
int publish_ai_state(listeningState newState) {
   const char stateTemplate[] = "{\"device\": \"ai\", \"name\":\"%s\", \"state\":\"%s\"}";
   char state[18] = "";
   char *aiState = NULL;
   int aiStateLength = 0;
   int rc = 0;

   if (newState == currentState || newState == INVALID_STATE) {
      return 0;
   }

   switch (newState) {
      case SILENCE:
         strcpy(state, "SILENCE");
         break;
      case WAKEWORD_LISTEN:
         strcpy(state, "WAKEWORD_LISTEN");
         break;
      case COMMAND_RECORDING:
         strcpy(state, "COMMAND_RECORDING");
         break;
      case PROCESS_COMMAND:
         strcpy(state, "PROCESS_COMMAND");
         break;
      case VISION_AI_READY:
         strcpy(state, "VISION_AI_READY");
         break;
      default:
         LOG_ERROR("Unknown state: %d", newState);
         return 1;
   }

   /* "- 4" is from substracting the two %s but adding the term char */
   aiStateLength = strlen(stateTemplate) + strlen(AI_NAME) + strlen(state) - 4 + 1;
   aiState = malloc(aiStateLength);
   if (aiState == NULL ) {
      LOG_ERROR("Error allocating memory for AI state.");
      return 1;
   }

   rc = snprintf(aiState, aiStateLength, stateTemplate, AI_NAME, state);
   if (rc < 0 || rc >= aiStateLength) {
      LOG_ERROR("Error creating AI state message.");
      free(aiState);
      return 1;
   }

   rc = mosquitto_publish(mosq, NULL, "hud", strlen(aiState),
                          aiState, 0, false);
   if(rc != MOSQ_ERR_SUCCESS){
      LOG_ERROR("Error publishing: %s\n", mosquitto_strerror(rc));
      free(aiState);
      return 1;
   }

   free(aiState);

   currentState = newState;  // Update the state after successful publish

   return 0;
}



/**
 * Displays help information for the program, outlining the usage and available command-line options.
 * The function dynamically adjusts the usage message based on whether the program name is available
 * from the command-line arguments.
 *
 * @param argc The number of command-line arguments passed to the program.
 * @param argv The array of command-line arguments. argv[0] is expected to contain the program name.
 */
void display_help(int argc, char *argv[]) {
   if (argc > 0) {
      LOG_INFO("Usage: %s [options]\n", argv[0]);
   } else {
      LOG_INFO("Usage: [options]\n");
   }

   // Print the list of available command-line options.
   LOG_INFO("Options:\n");
   LOG_INFO("  -c, --capture DEVICE   Specify the PCM capture device.");
   LOG_INFO("  -l, --logfile LOGFILE  Specify the log filename instead of stdout/stderr.");
   LOG_INFO("  -d, --playback DEVICE  Specify the PCM playback device.");
   LOG_INFO("  -h, --help             Display this help message and exit.");
}

int main(int argc, char *argv[])
{
   char *input_text = NULL;
   char *command_text = NULL;
   char *response_text = NULL;
   const char *vosk_output = NULL;
   size_t vosk_output_length = 0;
   int vosk_nochange = 0;
   struct json_object *conversation_history = NULL;
   struct json_object *system_message = NULL;
   int rc = 0;
   int opt = 0;
   const char *log_filename = NULL;

#ifndef ALSA_DEVICE
   // Define the Pulse parameters
   int error = 0;
#endif

   // Audio Buffer
   uint32_t buff_size = 0;
   float temp_buff_size = DEFAULT_RATE * DEFAULT_CHANNELS * sizeof(int16_t) * DEFAULT_CAPTURE_SECONDS;
   uint32_t max_buff_size = (uint32_t)ceil(temp_buff_size);
   char *max_buff = NULL;
   double rms = 0.0;

   // Command Configuration
   FILE *configFile = NULL;
   char buffer[10*1024];
   int bytes_read = 0;

   // Command Parsing
   actionType actions[MAX_ACTIONS]; /**< All of the available actions read from the JSON. */
   int numActions = 0;              /**< Total actions in the actions array. */

   commandSearchElement commands[MAX_COMMANDS];
   int numCommands = 0;

   char *next_char_ptr = NULL;

   audioControl myAudioControls;
   pthread_t backgroundAudioDetect;

   int commandTimeout = 0;

   /* Array Counts */
   int numGoodbyeWords = sizeof(goodbyeWords) / sizeof(goodbyeWords[0]);
   int numWakeWords = sizeof(wakeWords) / sizeof(wakeWords[0]);
   int numIgnoreWords = sizeof(ignoreWords) / sizeof(ignoreWords[0]);
   int numCancelWords = sizeof(cancelWords) / sizeof(cancelWords[0]);

   int i = 0;

   listeningState recState = SILENCE;
   listeningState silenceNextState = WAKEWORD_LISTEN;

   static struct option long_options[] = {
      {"capture", required_argument, NULL, 'c'},
      {"logfile", required_argument, NULL, 'l'},
      {"playback", required_argument, NULL, 'd'},
      {"help", no_argument, NULL, 'h'},
      {0, 0, 0, 0}
   };
   int option_index = 0;

   LOG_INFO("%s Version %s: %s\n", APP_NAME, VERSION_NUMBER, GIT_SHA);

   // TODO: I'm adding this here but it will need better error clean-ups.
   curl_global_init(CURL_GLOBAL_DEFAULT);

   while ((opt = getopt_long(argc, argv, "c:d:hl:", long_options, &option_index)) != -1) {
      switch (opt) {
      case 'c':
         strncpy(pcm_capture_device, optarg, sizeof(pcm_capture_device));
         pcm_capture_device[sizeof(pcm_capture_device) - 1] = '\0';
         break;
      case 'd':
         strncpy(pcm_playback_device, optarg, sizeof(pcm_playback_device));
         pcm_playback_device[sizeof(pcm_playback_device) - 1] = '\0';
         break;
      case 'h':
         display_help(argc, argv);
         exit(EXIT_SUCCESS);
      case 'l':
         log_filename = optarg;
         break;
      case '?':
         display_help(argc, argv);
         exit(EXIT_FAILURE);
      default:
         display_help(argc, argv);
         exit(EXIT_FAILURE);
      }
   }


   // Initialize logging
   if (log_filename) {
      if (init_logging(log_filename, LOG_TO_FILE) != 0) {
         fprintf(stderr, "Failed to initialize logging to file: %s\n", log_filename);
         return 1;
      }
   } else {
      if (init_logging(NULL, LOG_TO_CONSOLE) != 0) {
         fprintf(stderr, "Failed to initialize logging to console\n");
         return 1;
      }
   }

   if (strcmp(pcm_capture_device, "") == 0) {
      strncpy(pcm_capture_device, DEFAULT_PCM_CAPTURE_DEVICE, sizeof(pcm_capture_device));
      pcm_capture_device[sizeof(pcm_capture_device) - 1] = '\0';
   }

   if (strcmp(pcm_playback_device, "") == 0) {
      strncpy(pcm_playback_device, DEFAULT_PCM_PLAYBACK_DEVICE, sizeof(pcm_playback_device));
      pcm_playback_device[sizeof(pcm_playback_device) - 1] = '\0';
   }

   // Command Processing
   initActions(actions);

   LOG_INFO("Reading json file...");
   configFile = fopen(CONFIG_FILE, "r");
   if (configFile == NULL) {
      LOG_ERROR("Unable to open config file: %s\n", CONFIG_FILE);
      return 1;
   }

   if ((bytes_read = fread(buffer, 1, sizeof(buffer), configFile)) > 0) {
      buffer[bytes_read] = '\0';
   } else {
      LOG_ERROR("Failed to read config file (%s): %s\n", CONFIG_FILE, strerror(bytes_read));
      fclose(configFile);
      return 1;
   }

   fclose(configFile);
   LOG_INFO("Done.\n");

   if (parseCommandConfig(buffer, actions, &numActions,
                          captureDevices, &numAudioCaptureDevices,
                          playbackDevices, &numAudioPlaybackDevices)) {
      LOG_ERROR("Error parsing json.\n");
      return 1;
   }

   LOG_INFO("\n");
   //printParsedData(actions, numActions);
   convertActionsToCommands(actions, &numActions, commands, &numCommands);
   LOG_INFO("Processed %d commands.", numCommands);
   //printCommands(commands, numCommands);

   // JSON setup for OpenAI
   conversation_history = json_object_new_array();
   system_message = json_object_new_object();

   json_object_object_add(system_message, "role", json_object_new_string("system"));
   json_object_object_add(system_message, "content",
                          json_object_new_string
                          (AI_DESCRIPTION));
   json_object_array_add(conversation_history, system_message);

#ifdef ALSA_DEVICE
   // Open Audio Capture Device
   rc = openAlsaPcmCaptureDevice(myAudioControls.handle, pcm_capture_device, myAudioControls.frames);
   if (rc) {
      LOG_ERROR("Error creating ALSA capture device.\n");
      return 1;
   }
   myAudioControls.full_buff_size = myAudioControls.frames * DEFAULT_CHANNELS * 2;
#else
   myAudioControls.pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
   if (myAudioControls.pa_handle == NULL) {
      LOG_ERROR("Error creating Pulse capture device.\n");
      return 1;
   }

   myAudioControls.pa_framesize = pa_frame_size(&sample_spec);

   myAudioControls.full_buff_size = myAudioControls.pa_framesize;
#endif

   LOG_INFO("max_buff_size: %u, full_buff_size: %u\n", max_buff_size, myAudioControls.full_buff_size);

   max_buff = (char *)malloc(max_buff_size);
   if (max_buff == NULL) {
      LOG_ERROR("malloc() failed on max_buff.\n");

#ifdef ALSA_DEVICE
      snd_pcm_close(myAudioControls.handle);
#else
      pa_simple_free(myAudioControls.pa_handle);
#endif

      return 1;
   }

   // Test background audio level
#if 0
   if (pthread_create(&backgroundAudioDetect, NULL, measureBackgroundAudio, (void *) &myAudioControls) != 0) {
      LOG_ERROR("Error creating background audio detection thread.\n");
   }
#else
   measureBackgroundAudio((void *) &myAudioControls);
#endif

   LOG_INFO("Init vosk.");
   // Vosk
   vosk_gpu_init();
   vosk_gpu_thread_init();

   VoskModel *model = vosk_model_new("model");
   if (model == NULL) {
      LOG_ERROR("Error creating new Vosk model.\n");

      free(max_buff);

#ifdef ALSA_DEVICE
      snd_pcm_close(myAudioControls.handle);
#else
      pa_simple_free(myAudioControls.pa_handle);
#endif

      return 1;
   }
   VoskRecognizer *recognizer = vosk_recognizer_new(model, DEFAULT_RATE);
   if (recognizer == NULL) {
      LOG_ERROR("Error creating new Vosk recognizer.\n");

      vosk_model_free(model);

      free(max_buff);

#ifdef ALSA_DEVICE
      snd_pcm_close(myAudioControls.handle);
#else
      pa_simple_free(myAudioControls.pa_handle);
#endif

      return 1;
   }

   LOG_INFO("Init mosquitto.");
   /* MQTT Setup */
   mosquitto_lib_init();

   mosq = mosquitto_new(NULL, true, NULL);
   if (mosq == NULL){
      LOG_ERROR("Error: Out of memory.\n");
      return 1;
   }

   /* Configure callbacks. This should be done before connecting ideally. */
   mosquitto_connect_callback_set(mosq, on_connect);
   mosquitto_subscribe_callback_set(mosq, on_subscribe);
   mosquitto_message_callback_set(mosq, on_message);

   /* Connect to local MQTT server. */
   rc = mosquitto_connect(mosq, MQTT_IP, MQTT_PORT, 60);
   if (rc != MOSQ_ERR_SUCCESS){
      mosquitto_destroy(mosq);
      LOG_ERROR("Error on mosquitto_connect(): %s\n", mosquitto_strerror(rc));
      return 1;
   } else {
      LOG_INFO("Connected to local MQTT server.\n");
   }

   rc = mosquitto_subscribe(mosq, NULL, APPLICATION_NAME, 0);
   if (rc != MOSQ_ERR_SUCCESS) {
      mosquitto_destroy(mosq);
      LOG_ERROR("Error on mosquitto_subscribe():\"/%s\" : %s\n",
              APPLICATION_NAME, mosquitto_strerror(rc));
      return 1;
   } else {
      LOG_INFO("Subscribed to \"%s\" MQTT.\n", APPLICATION_NAME);
   }

   /* Start processing MQTT events. */
   mosquitto_loop_start(mosq);

   LOG_INFO("Init text to speech.");
   /* Initialize text to speech processing. */
   initialize_text_to_speech(pcm_playback_device);

   text_to_speech((char *) timeOfDayGreeting());

   // Register the signal handler for SIGINT.
   if (signal(SIGINT, signal_handler) == SIG_ERR) {
      LOG_ERROR("Error: Unable to register signal handler.\n");
      exit(EXIT_FAILURE);
   }

   // Main loop
   LOG_INFO("Listening...\n");
   while (!quit) {
      if (vision_ai_ready){
         recState = VISION_AI_READY;
      }

      publish_ai_state(recState);
      switch (recState) {
         case SILENCE:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_PLAY;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));

            if (rms >= (backgroundRMS + TALKING_THRESHOLD_OFFSET)) {
               LOG_WARNING("SILENCE: Talking detected. Going into WAKEWORD_LISTENING.\n");
               recState = silenceNextState;

               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_partial_result(recognizer);
               if (vosk_output == NULL) {
                  LOG_ERROR("vosk_recognizer_partial_result() returned NULL!\n");
               } else {
                  LOG_WARNING("Partial Input: %s\n", vosk_output);
               }
            }
            break;
         case WAKEWORD_LISTEN:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PLAY) {
               tts_playback_state = TTS_PLAYBACK_PAUSE;
            }
            pthread_mutex_unlock(&tts_mutex);

            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));

            if (rms >= (backgroundRMS + TALKING_THRESHOLD_OFFSET)) {
               LOG_WARNING("WAKEWORD_LISTEN: Talking still in progress.\n");
               /* For an additional layer of "silence," I'm getting the length of the
                * vosk output to see if the volume was up but no one was saying
                * anything. */
               vosk_output_length = strlen(vosk_output);

               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_partial_result(recognizer);
               if (vosk_output == NULL) {
                  LOG_ERROR("vosk_recognizer_partial_result() returned NULL!\n");
               } else {
                  LOG_WARNING("Partial Input: %s\n", vosk_output);
                  if (strlen(vosk_output) == vosk_output_length) {
                     vosk_nochange = 1;
                  }
               }
            }

            if (rms < (backgroundRMS + TALKING_THRESHOLD_OFFSET) || vosk_nochange) {
               commandTimeout++;
               vosk_nochange = 0;
            } else {
               commandTimeout = 0;
            }

            if (commandTimeout >= DEFAULT_COMMAND_TIMEOUT) {
               commandTimeout = 0;
               LOG_WARNING("WAKEWORD_LISTEN: Checking for wake word.\n");
               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_final_result(recognizer);
               if (vosk_output == NULL) {
                  LOG_ERROR("vosk_recognizer_final_result() returned NULL!\n");
               } else {
                  LOG_WARNING("Input: %s\n", vosk_output);
                  input_text = getTextResponse(vosk_output);

                  for (i = 0; i < numGoodbyeWords; i++) {
                     if (strcmp(input_text, goodbyeWords[i]) == 0) {
                        pthread_mutex_lock(&tts_mutex);
                        if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                           tts_playback_state = TTS_PLAYBACK_DISCARD;
                           pthread_cond_signal(&tts_cond);
                        }
                        pthread_mutex_unlock(&tts_mutex);

                        text_to_speech("Goodbye sir.");

                        quit = 1;
                     }
                  }

                  pthread_mutex_lock(&tts_mutex);
                  if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                     for (i = 0; i < numCancelWords; i++) {
                        if (strcmp(input_text, cancelWords[i]) == 0) {
                           LOG_WARNING("Cancel word detected.\n");

                           tts_playback_state = TTS_PLAYBACK_DISCARD;
                           pthread_cond_signal(&tts_cond);

                           silenceNextState = WAKEWORD_LISTEN;
                           recState = SILENCE;
                        }
                     }
                  }
                  pthread_mutex_unlock(&tts_mutex);

                  for (i = 0; i < numWakeWords; i++) {
                     char *found_ptr = strstr(input_text, wakeWords[i]);
                     if (found_ptr != NULL) {
                        LOG_WARNING("Wake word detected.\n");

                        // Calculate the length of the wake word
                        size_t wakeWordLength = strlen(wakeWords[i]);

                        // Advance the pointer to the next character after wakeWords[i]
                        next_char_ptr = found_ptr + wakeWordLength;

                        break;
                     }
                  }

                  if (i < numWakeWords) {
                     pthread_mutex_lock(&tts_mutex);
                     if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                        tts_playback_state = TTS_PLAYBACK_DISCARD;
                        pthread_cond_signal(&tts_cond);
                     }
                     pthread_mutex_unlock(&tts_mutex);

                     if (*next_char_ptr == '\0') {
                        LOG_WARNING("wakeWords[i] was found at the end of input_text.\n");
                        text_to_speech("Hello sir.");

                        commandTimeout = 0;
                        silenceNextState = COMMAND_RECORDING;
                        recState = SILENCE;
                     } else {
                        /* FIXME: Below I simply add one to make sure to skip the most likely
                         * space. I need to properly remove leading spaces. */
                        command_text = strdup(next_char_ptr + 1);
                        free(input_text);

                        recState = PROCESS_COMMAND;
                     }
                  } else {
                     pthread_mutex_lock(&tts_mutex);
                     if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                        tts_playback_state = TTS_PLAYBACK_PLAY;
                        pthread_cond_signal(&tts_cond);
                     }
                     pthread_mutex_unlock(&tts_mutex);

                     silenceNextState = WAKEWORD_LISTEN;
                     recState = SILENCE;
                  }
               }
            }
            buff_size = 0;
            break;
         case COMMAND_RECORDING:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_DISCARD;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));

            if (rms >= (backgroundRMS + TALKING_THRESHOLD_OFFSET)) {
               LOG_WARNING("COMMAND_RECORDING: Talking still in progress.\n");
               /* For an additional layer of "silence," I'm getting the length of the
                * vosk output to see if the volume was up but no one was saying
                * anything. */
               vosk_output_length = strlen(vosk_output);

               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_partial_result(recognizer);
               if (vosk_output == NULL) {
                  LOG_ERROR("vosk_recognizer_partial_result() returned NULL!\n");
               } else {
                  LOG_WARNING("Partial Input: %s\n", vosk_output);
                  if (strlen(vosk_output) == vosk_output_length) {
                     vosk_nochange = 1;
                  }
               }
            }

            if (rms < (backgroundRMS + TALKING_THRESHOLD_OFFSET) || vosk_nochange) {
               commandTimeout++;
               vosk_nochange = 0;
            } else {
               commandTimeout = 0;
            }

            if (commandTimeout >= DEFAULT_COMMAND_TIMEOUT) {
               commandTimeout = 0;
               LOG_WARNING("COMMAND_RECORDING: Command processing.\n");
               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_final_result(recognizer);
               if (vosk_output == NULL) {
                  LOG_ERROR("vosk_recognizer_final_result() returned NULL!\n");
               } else {
                  LOG_WARNING("Input: %s\n", vosk_output);

                  input_text = getTextResponse(vosk_output);

                  command_text = strdup(input_text);
                  free(input_text);

                  recState = PROCESS_COMMAND;
               }
            }
            buff_size = 0;
            break;
         case PROCESS_COMMAND:
            /* Process Commands before AI. */
            for (i = 0; i < numCommands; i++) {
               if (searchString(commands[i].actionWordsWildcard, command_text) == 1) {
                  char thisValue[1024];   // FIXME: These are abnormally large.
                                          // I'm in a hurry and don't want overflows.
                  char thisCommand[2048];
                  char thisSubstring[2048];
                  int strLength = 0;

                  pthread_mutex_lock(&tts_mutex);
                  if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                     tts_playback_state = TTS_PLAYBACK_DISCARD;
                     pthread_cond_signal(&tts_cond);
                  }
                  pthread_mutex_unlock(&tts_mutex);

                  memset(thisValue, '\0', sizeof(thisValue));
                  LOG_WARNING("Found command \"%s\".\n\tLooking for value in \"%s\".\n",
                         commands[i].actionWordsWildcard, commands[i].actionWordsRegex);

                  strLength = strlen(commands[i].actionWordsRegex);
                  if ((strLength >= 2) && (commands[i].actionWordsRegex[strLength - 2] == '%') &&
                      (commands[i].actionWordsRegex[strLength - 1] == 's')) {
                     strncpy(thisSubstring, commands[i].actionWordsRegex, strLength - 2);
                     thisSubstring[strLength - 2] = '\0';
                     strcpy(thisValue, extract_remaining_after_substring(command_text, thisSubstring));
                  } else {
                     int retSs = sscanf(command_text, commands[i].actionWordsRegex, thisValue);
                  }
                  snprintf(thisCommand, sizeof(thisCommand),
                           commands[i].actionCommand, thisValue);
                  LOG_WARNING("Sending: \"%s\"\n", thisCommand);

                  rc = mosquitto_publish(mosq, NULL, commands[i].topic, strlen(thisCommand),
                                         thisCommand, 0, false);
                  if(rc != MOSQ_ERR_SUCCESS){
                     LOG_ERROR("Error publishing: %s\n", mosquitto_strerror(rc));
                  }

                  break;
               }
            }

            if (i >= numCommands) {
               LOG_WARNING("Not detected as a command.\n");
#ifndef DISABLE_AI
               int ignoreCount = 0;

               for (ignoreCount = 0; ignoreCount < numIgnoreWords; ignoreCount++) {
                  if (strcmp(command_text, ignoreWords[ignoreCount]) == 0) {
                     LOG_WARNING("Ignore word detected.\n");

                     pthread_mutex_lock(&tts_mutex);
                     if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                        tts_playback_state = TTS_PLAYBACK_PLAY;
                        pthread_cond_signal(&tts_cond);
                     }
                     pthread_mutex_unlock(&tts_mutex);

                     break;
                  }
               }

               if (ignoreCount < numIgnoreWords) {
                  LOG_WARNING("Input ignored. Found in ignore list.\n");
                  silenceNextState = WAKEWORD_LISTEN;
                  recState = SILENCE;
               } else {
                  response_text = getGptResponse(conversation_history, command_text, NULL, 0);
                  if (response_text != NULL) {
                     LOG_WARNING("AI: %s\n", response_text);

                     /* This match section was added for local AI models that return extra data that needs
                      * to be filtered out.
                      */
                     // <end_of_turn> is being added by Gemma 2B
                     char *match = NULL;
                     if ((match = strstr(response_text, "<end_of_turn>")) != NULL) {
                        *match = '\0';
                        LOG_WARNING("AI: %s\n", response_text);
                     }

                     // Now be sure to filter out special characters that give us problems.
                     remove_chars(response_text, "*");
                     remove_emojis(response_text);

                     pthread_mutex_lock(&tts_mutex);
                     if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                        tts_playback_state = TTS_PLAYBACK_DISCARD;
                        pthread_cond_signal(&tts_cond);
                     }
                     pthread_mutex_unlock(&tts_mutex);

                     // Now let's see how it sounds.
                     text_to_speech(response_text);

                     struct json_object *ai_message = json_object_new_object();
                     json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
                     json_object_object_add(ai_message, "content", json_object_new_string(response_text));
                     json_object_array_add(conversation_history, ai_message);

                     free(response_text);
                  } else {
                     pthread_mutex_lock(&tts_mutex);
                     if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                        tts_playback_state = TTS_PLAYBACK_DISCARD;
                        pthread_cond_signal(&tts_cond);
                     }
                     pthread_mutex_unlock(&tts_mutex);


                     LOG_ERROR("GPT error.\n");
                     text_to_speech("I'm sorry but I'm currently unavailable boss.");
                  }
               }
#endif
            }

            for (i = 0; i < numGoodbyeWords; i++) {
               if (strcmp(command_text, goodbyeWords[i]) == 0) {
                  quit = 1;
               }
            }

            free(command_text);
            silenceNextState = WAKEWORD_LISTEN;
            recState = SILENCE;

            break;
         case VISION_AI_READY:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_PLAY;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            // Get the AI response using the image recognition.
            response_text = getGptResponse(conversation_history,
                  "What am I looking at? Ignore the overlay unless asked about it specifically.",
                  vision_ai_image, vision_ai_image_size);
            if (response_text != NULL) {
               // AI returned successfully, vocalize response.
               LOG_WARNING("AI: %s\n", response_text);
	            char *match = NULL;
	            if ((match = strstr(response_text, "<end_of_turn>")) != NULL) {
                  *match = '\0';
                  LOG_WARNING("AI: %s\n", response_text);
	            }
               text_to_speech(response_text);

               // Add the successful AI response to the conversation.
               struct json_object *ai_message = json_object_new_object();
               json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
               json_object_object_add(ai_message, "content", json_object_new_string(response_text));
               json_object_array_add(conversation_history, ai_message);

               free(response_text);
            } else {
               // Error on AI response
               LOG_ERROR("GPT error.\n");
               text_to_speech("I'm sorry but I'm currently unavailable boss.");
            }

            // Cleanup the image
            if (vision_ai_image != NULL) {
               free(vision_ai_image);
               vision_ai_image = NULL;
            }
            vision_ai_image_size = 0;
            vision_ai_ready = 0;

            // Set the next listening state
            silenceNextState = WAKEWORD_LISTEN;
            recState = SILENCE;

            break;
         default:
            LOG_ERROR("I really shouldn't be here.\n");
      }
   }

   LOG_INFO("Quit.\n");

   cleanup_text_to_speech();

   mosquitto_disconnect(mosq);
   mosquitto_loop_stop(mosq, false);
   mosquitto_lib_cleanup();

   json_object_put(conversation_history);

   // Cleanup
   vosk_recognizer_free(recognizer);
   vosk_model_free(model);

#ifdef ALSA_DEVICE
   snd_pcm_drop(myAudioControls.handle);
   snd_pcm_close(myAudioControls.handle);
#else
   pa_simple_free(myAudioControls.pa_handle);
#endif
   free(max_buff);

   curl_global_cleanup();

   // Close the log file properly
   close_logging();

   return 0;
}
