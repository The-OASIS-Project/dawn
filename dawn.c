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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <getopt.h>

#include <mosquitto.h>

#include "vosk_api.h"

#include "audio_utils.h"
#include "dawn.h"
#include "mosquitto_comms.h"
#include "openai.h"
#include "text_to_command_nuevo.h"
#include "text_to_speech.h"

// Define the default sample rate for audio capture.
#define DEFAULT_RATE             44100

// Define the default number of audio channels (1 for mono).
#define DEFAULT_CHANNELS         1

// Define the default duration of audio capture in seconds.
#define DEFAULT_CAPTURE_SECONDS  0.5f

// Define the default command timeout in terms of iterations of DEFAULT_CAPTURE_SECONDS.
#define DEFAULT_COMMAND_TIMEOUT  4

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
   pa_usec_t pa_latency;
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
   "hi " AI_NAME
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

// Standard greeting messages based on the time of day.
const char* morning_greeting = "Good morning boss.";
const char* day_greeting = "Good day Sir.";
const char* evening_greeting = "Good evening Sir.";

// Enum representing the possible states of the AI's listening process.
// - SILENCE: The AI is not actively listening or processing commands. It's waiting for a noise threshold.
// - WAKEWORD_LISTEN: The AI is listening for a wake word to initiate interaction.
// - COMMAND_RECORDING: The AI is recording a command after recognizing a wake word alone.
// - PROCESS_COMMAND: The AI is processing a recorded command.
typedef enum { SILENCE, WAKEWORD_LISTEN, COMMAND_RECORDING, PROCESS_COMMAND } listeningState;

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
 * Callback function for text-to-speech commands.
 *
 * @param actionName The name of the action triggered this callback (unused in the current implementation).
 * @param value The text that needs to be converted to speech.
 *
 * This function prints the received text command and then calls the text_to_speech function
 * to play it through the PCM playback device.
 */
void textToSpeechCallback(const char *actionName, char *value) {
   printf("Received text to speech command: \"%s\"\n", value);
   text_to_speech(pcm_playback_device, value);
}

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
const char *getPcmPlaybackDevice(void) {
   return (const char*) pcm_playback_device;
}

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
const char *getPcmCaptureDevice(void) {
   return (const char*) pcm_capture_device;
}

/**
 * Searches for an audio playback device by name.
 *
 * @param name The name of the audio playback device to search for.
 * @return A pointer to the device identifier if found, otherwise NULL.
 *
 * This function iterates over the list of known audio playback devices, comparing each
 * device's name with the provided name. If a match is found, it returns the device identifier.
 */
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
void setPcmPlaybackDevice(const char *actionName, char *value) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];

   for (i = 0; i < numAudioPlaybackDevices; i++) {
      if (strcmp(playbackDevices[i].name, value) == 0) {
         printf("Setting audio playback device to \"%s\".\n", playbackDevices[i].device);
         strncpy(pcm_playback_device, playbackDevices[i].device, MAX_WORD_LENGTH);
         pcm_playback_device[MAX_WORD_LENGTH] = '\0'; // Ensure null termination
         snprintf(speech, MAX_COMMAND_LENGTH, "Switching playback device to %s.", value);
         text_to_speech(pcm_playback_device, speech);
         break;
      }
   }

   if (i >= numAudioPlaybackDevices) {
      fprintf(stderr, "Requested audio playback device not found.\n");
      snprintf(speech, MAX_COMMAND_LENGTH, "Sorry sir. A playback devices called %s was not found.", value);
      text_to_speech(pcm_playback_device, speech);
   }
}

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
void setPcmCaptureDevice(const char *actionName, char *value) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];

   for (i = 0; i < numAudioCaptureDevices; i++) {
      if (strcmp(captureDevices[i].name, value) == 0) {
         printf("Setting audio capture device to \"%s\".\n", captureDevices[i].device);
         strncpy(pcm_capture_device, captureDevices[i].device, MAX_WORD_LENGTH);
         pcm_capture_device[MAX_WORD_LENGTH] = '\0'; // Ensure null termination
         snprintf(speech, MAX_COMMAND_LENGTH, "Switching capture device to %s.", value);
         text_to_speech(pcm_playback_device, speech);
         break;
      }
   }

   if (i >= numAudioCaptureDevices) {
      fprintf(stderr, "Requested audio capture device not found.\n");
      snprintf(speech, MAX_COMMAND_LENGTH, "Sorry sir. A capture devices called %s was not found.", value);
      text_to_speech(pcm_playback_device, speech);
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
      fprintf(stderr, "malloc() failed on buff.\n");
      return NULL; // Early return on allocation failure for buff.
   }

   uint32_t max_buff_size = // Calculate maximum buffer size based on backend.
#ifdef ALSA_DEVICE
   DEFAULT_RATE * DEFAULT_CHANNELS * 2 * BACKGROUND_CAPTURE_SECONDS;
#else
   (myControl->pa_latency + (BACKGROUND_CAPTURE_SECONDS * DEFAULT_RATE * myControl->pa_framesize)) / myControl->pa_framesize;
#endif

   char *max_buff = (char *)malloc(max_buff_size);
   if (max_buff == NULL) {
      fprintf(stderr, "malloc() failed on max_buff.\n");
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
            printf("Error reading PCM.\n");
         }
         break; // Exit loop on read error or buffer full.
      }
   }
#else
   // PulseAudio audio capture loop.
   for (size_t i = 0; i < max_buff_size / myControl->full_buff_size; ++i) {
      if (pa_simple_read(myControl->pa_handle, buff, myControl->pa_framesize, &error) < 0) {
         printf("Could not read audio: %s\n", pa_strerror(error));
         break; // Exit loop on read error.
      }
      memcpy(max_buff + buff_size, buff, myControl->full_buff_size);
      buff_size += myControl->full_buff_size;
   }
#endif

   // Compute RMS for captured audio.
   double rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));
   printf("RMS of background recording is %g.\n", rms);
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
      fprintf(stderr, "Error: Unable to process text response.\n");
      return NULL;
   }

   // Get the "text" object from the JSON
   if (json_object_object_get_ex(parsed_json, "text", &text_object)) {
      const char *input_text = json_object_get_string(text_object);
      if (input_text == NULL) {
         fprintf(stderr, "Error: Unable to get string from input text.\n");
         json_object_put(parsed_json);
         return NULL;
      }

      return_text = malloc((strlen(input_text) + 1) * sizeof(char));
      if (return_text == NULL) {
         fprintf(stderr, "malloc() failed in getTextResponse().\n");
         json_object_put(parsed_json);
         return NULL;
      }

      // Directly copy the input text into the return buffer
      strcpy(return_text, input_text);

      // Debugging: Print the extracted text
      printf("Input Text: %s\n", return_text);
   } else {
      fprintf(stderr, "Error: 'text' field not found in JSON.\n");
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

   printf("ALSA CAPTURE DRIVER\n");

   /* Open PCM device for playback. */
   rc = snd_pcm_open(handle, pcm_device, SND_PCM_STREAM_CAPTURE, 0);
   if (rc < 0) {
      fprintf(stderr, "unable to open pcm device for capture (%s): %s\n", pcm_device, snd_strerror(rc));
      return 1;
   }

   snd_pcm_hw_params_alloca(&params);
   snd_pcm_hw_params_any(*handle, params);
   snd_pcm_hw_params_set_access(*handle, params, DEFAULT_ACCESS);
   snd_pcm_hw_params_set_format(*handle, params, DEFAULT_FORMAT);
   snd_pcm_hw_params_set_channels(*handle, params, DEFAULT_CHANNELS);
   snd_pcm_hw_params_set_rate_near(*handle, params, &rate, &dir);
   printf("Capture rate set to %u\n", rate);
   snd_pcm_hw_params_set_period_size_near(*handle, params, frames, &dir);
   printf("Frames set to %lu\n", *frames);
   rc = snd_pcm_hw_params(*handle, params);
   if (rc < 0) {
      fprintf(stderr, "unable to set hw parameters: %s\n", snd_strerror(rc));
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

   printf("PULSEAUDIO CAPTURE DRIVER: %s\n", pcm_capture_device);

   /* Create a new playback stream */
   if (!(pa_handle = pa_simple_new(NULL, APPLICATION_NAME, PA_STREAM_RECORD, pcm_capture_device, "record", &sample_spec, NULL, NULL, &rc))) {
      fprintf(stderr, "Error opening PulseAudio record: %s\n", pa_strerror(rc));
      return NULL;
   }

   printf("Capture opened successfully.\n");

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
      fprintf(stderr, "malloc() failed on buff.\n");
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
            printf("Error reading PCM.\n");
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
            fprintf(stderr, "pa_simple_read() failed: %s\n", pa_strerror(error));
            free(buff); // Free the local buffer on error.
            return 1; // Return error code on read failure.
         }
         buffer_full = 1; // Set buffer_full flag if max_buff is filled.
      }
#endif
   }

   free(buff); // Free the local buffer before exiting.
   return 0; // Return success.
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
      printf("Usage: %s [options]\n", argv[0]);
   } else {
      printf("Usage: [options]\n");
   }

   // Print the list of available command-line options.
   printf("Options:\n");
   printf("  -c, --capture DEVICE   Specify the PCM capture device.\n");
   printf("  -d, --playback DEVICE  Specify the PCM playback device.\n");
   printf("  -h, --help             Display this help message and exit.\n");
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

#ifndef ALSA_DEVICE
   // Define the Pulse parameters
   int error = 0;
#endif

   // Audio Buffer
   uint32_t buff_size = 0;
#ifdef ALSA_DEVICE
   float temp_buff_size = DEFAULT_RATE * DEFAULT_CHANNELS * 2 * DEFAULT_CAPTURE_SECONDS;
   uint32_t max_buff_size = (uint32_t)ceil(temp_buff_size);
#else
   float temp_buff_size = 0;
   uint32_t max_buff_size = 0;
#endif
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

   /* MQTT */
   struct mosquitto *mosq;

   /* Array Counts */
   int numGoodbyeWords = sizeof(goodbyeWords) / sizeof(goodbyeWords[0]);
   int numWakeWords = sizeof(wakeWords) / sizeof(wakeWords[0]);
   int numIgnoreWords = sizeof(ignoreWords) / sizeof(ignoreWords[0]);

   int i = 0;

   int quit = 0;
   listeningState recState = SILENCE;
   listeningState silenceNextState = WAKEWORD_LISTEN;

   static struct option long_options[] = {
      {"capture", required_argument, NULL, 'c'},
      {"playback", required_argument, NULL, 'd'},
      {"help", no_argument, NULL, 'h'},
      {0, 0, 0, 0}
   };
   int option_index = 0;

   // TODO: I'm adding this here but it will need better error clean-ups.
   curl_global_init(CURL_GLOBAL_DEFAULT);

   while ((opt = getopt_long(argc, argv, "c:d:h", long_options, &option_index)) != -1) {
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
      case '?':
         display_help(argc, argv);
         exit(EXIT_FAILURE);
      default:
         display_help(argc, argv);
         exit(EXIT_FAILURE);
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

   printf("Reading json file...");
   configFile = fopen(CONFIG_FILE, "r");
   if (configFile == NULL) {
      fprintf(stderr, "Unable to open config file: %s\n", CONFIG_FILE);
      return 1;
   }

   if ((bytes_read = fread(buffer, 1, sizeof(buffer), configFile)) > 0) {
      buffer[bytes_read] = '\0';
   } else {
      fprintf(stderr, "Failed to read config file (%s): %s\n", CONFIG_FILE, strerror(bytes_read));
      fclose(configFile);
      return 1;
   }

   fclose(configFile);
   printf("Done.\n");

   if (parseCommandConfig(buffer, actions, &numActions,
                          captureDevices, &numAudioCaptureDevices,
                          playbackDevices, &numAudioPlaybackDevices)) {
      fprintf(stderr, "Error parsing json.\n");
      return 1;
   }

   printf("\n");
   printParsedData(actions, numActions);
   convertActionsToCommands(actions, &numActions, commands, &numCommands);
   printCommands(commands, numCommands);

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
      fprintf(stderr, "Error creating ALSA capture device.\n");
      return 1;
   }
   myAudioControls.full_buff_size = myAudioControls.frames * DEFAULT_CHANNELS * 2;
#else
   myAudioControls.pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
   if (myAudioControls.pa_handle == NULL) {
      fprintf(stderr, "Error creating Pulse capture device.\n");
      return 1;
   }

   myAudioControls.pa_latency = pa_simple_get_latency(myAudioControls.pa_handle, &error);
   myAudioControls.pa_framesize = pa_frame_size(&sample_spec);

   myAudioControls.full_buff_size = myAudioControls.pa_framesize;

   temp_buff_size = DEFAULT_CAPTURE_SECONDS * DEFAULT_RATE * myAudioControls.pa_framesize;
   max_buff_size = (myAudioControls.pa_latency + ceil(temp_buff_size)) / myAudioControls.pa_framesize;
#endif

   printf("max_buff_size: %u, full_buff_size: %u\n", max_buff_size, myAudioControls.full_buff_size);

   max_buff = (char *)malloc(max_buff_size);
   if (max_buff == NULL) {
      fprintf(stderr, "malloc() failed on max_buff.\n");

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
      fprintf(stderr, "Error creating background audio detection thread.\n");
   }
#else
   measureBackgroundAudio((void *) &myAudioControls);
#endif

   // Vosk
   vosk_gpu_init();
   vosk_gpu_thread_init();

   VoskModel *model = vosk_model_new("model");
   if (model == NULL) {
      fprintf(stderr, "Error creating new Vosk model.\n");

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
      fprintf(stderr, "Error creating new Vosk recognizer.\n");

      vosk_model_free(model);

      free(max_buff);

#ifdef ALSA_DEVICE
      snd_pcm_close(myAudioControls.handle);
#else
      pa_simple_free(myAudioControls.pa_handle);
#endif

      return 1;
   }

   /* MQTT Setup */
   mosquitto_lib_init();

   mosq = mosquitto_new(NULL, true, NULL);
   if (mosq == NULL){
      fprintf(stderr, "Error: Out of memory.\n");
      return 1;
   }

   /* Configure callbacks. This should be done before connecting ideally. */
   mosquitto_connect_callback_set(mosq, on_connect);
   mosquitto_subscribe_callback_set(mosq, on_subscribe);
   mosquitto_message_callback_set(mosq, on_message);

   /* Connect to local MQTT server. */
   rc = mosquitto_connect(mosq, "127.0.0.1", 1883, 60);
   if (rc != MOSQ_ERR_SUCCESS){
      mosquitto_destroy(mosq);
      fprintf(stderr, "Error on mosquitto_connect(): %s\n", mosquitto_strerror(rc));
      return 1;
   } else {
      printf("Connected to local MQTT server.\n");
   }

   rc = mosquitto_subscribe(mosq, NULL, APPLICATION_NAME, 0);
   if (rc != MOSQ_ERR_SUCCESS) {
      mosquitto_destroy(mosq);
      fprintf(stderr, "Error on mosquitto_subscribe():\"/%s\" : %s\n",
              APPLICATION_NAME, mosquitto_strerror(rc));
      return 1;
   } else {
      printf("Subscribed to \"%s\" MQTT.\n", APPLICATION_NAME);
   }

   /* Start processing MQTT events. */
   mosquitto_loop_start(mosq);
   /* End MQTT Setup */

   text_to_speech(pcm_playback_device, (char *) timeOfDayGreeting());

   // Main loop
   printf("Listening...\n");

#ifdef ALSA_DEVICE
   snd_pcm_drop(myAudioControls.handle);
   rc = snd_pcm_prepare(myAudioControls.handle);
   if (rc < 0) {
   fprintf(stderr, "Cannot prepare audio interface for use (%s)\n",
      snd_strerror(rc));
      exit(1);
   }
#else
   if (pa_simple_flush(myAudioControls.pa_handle, &error) != 0) {
      printf("Unable to flush buffer: %s\n", pa_strerror(error));
   }
   pa_simple_free(myAudioControls.pa_handle);

   myAudioControls.pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
   if (myAudioControls.pa_handle == NULL) {
      fprintf(stderr, "Error creating Pulse capture device.\n");
      return 1;
   }
#endif

   while (!quit) {
      switch (recState) {
         case SILENCE:
            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));

            if (rms >= (backgroundRMS + TALKING_THRESHOLD_OFFSET)) {
               printf("SILENCE: Talking detected. Going into WAKEWORD_LISTENING.\n");
               recState = silenceNextState;

               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_partial_result(recognizer);
               if (vosk_output == NULL) {
                  fprintf(stderr, "vosk_recognizer_partial_result() returned NULL!\n");
               } else {
                  printf("Partial Input: %s\n", vosk_output);
               }
            }
            break;
         case WAKEWORD_LISTEN:
            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));

            if (rms >= (backgroundRMS + TALKING_THRESHOLD_OFFSET)) {
               printf("WAKEWORD_LISTEN: Talking still in progress.\n");
               /* For an additional layer of "silence," I'm getting the length of the
                * vosk output to see if the volume was up but no one was saying
                * anything. */
               vosk_output_length = strlen(vosk_output);

               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_partial_result(recognizer);
               if (vosk_output == NULL) {
                  fprintf(stderr, "vosk_recognizer_partial_result() returned NULL!\n");
               } else {
                  printf("Partial Input: %s\n", vosk_output);
                  if (strlen(vosk_output) == vosk_output_length) {
                     vosk_nochange = 1;
                  }
               }
            }

            if (rms < (backgroundRMS + TALKING_THRESHOLD_OFFSET) || vosk_nochange) {
               printf(".");
               commandTimeout++;
               vosk_nochange = 0;
            } else {
               commandTimeout = 0;
            }

            if (commandTimeout >= DEFAULT_COMMAND_TIMEOUT) {
               printf("\n");
               commandTimeout = 0;
               printf("WAKEWORD_LISTEN: Checking for wake word.\n");
               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_final_result(recognizer);
               if (vosk_output == NULL) {
                  fprintf(stderr, "vosk_recognizer_final_result() returned NULL!\n");
               } else {
                  printf("Input: %s\n", vosk_output);
                  input_text = getTextResponse(vosk_output);

                  for (i = 0; i < numGoodbyeWords; i++) {
                     if (strcmp(input_text, goodbyeWords[i]) == 0) {
                        quit = 1;

                        text_to_speech(pcm_playback_device, "Goodbye sir.");
                     }
                  }

                  for (i = 0; i < numWakeWords; i++) {
                     char *found_ptr = strstr(input_text, wakeWords[i]);
                     if (found_ptr != NULL) {
                        printf("Wake word detected.\n");

                        // Calculate the length of the wake word
                        size_t wakeWordLength = strlen(wakeWords[i]);

                        // Advance the pointer to the next character after wakeWords[i]
                        next_char_ptr = found_ptr + wakeWordLength;

                        break;
                     }
                  }

                  if (i < numWakeWords) {

                     if (*next_char_ptr == '\0') {
                        printf("wakeWords[i] was found at the end of input_text.\n");
                        text_to_speech(pcm_playback_device, "Hello sir.");

#ifdef ALSA_DEVICE
                        /* Remove if we can detect immediately. */
                        snd_pcm_drop(myAudioControls.handle);
                        if (snd_pcm_prepare(myAudioControls.handle) < 0) {
                           fprintf(stderr, "Cannot prepare audio interface for use (%s)\n",
                           snd_strerror(rc));
                           exit(1);
                        }
#else
                        if (pa_simple_flush(myAudioControls.pa_handle, &error) != 0) {
                           printf("Unable to flush buffer: %s\n", pa_strerror(error));
                        }
                        pa_simple_free(myAudioControls.pa_handle);

                        myAudioControls.pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
                        if (myAudioControls.pa_handle == NULL) {
                           fprintf(stderr, "Error creating Pulse capture device.\n");
                           return 1;
                        }
#endif

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
                     silenceNextState = WAKEWORD_LISTEN;
                     recState = SILENCE;
                  }
               }
            }
            buff_size = 0;
            break;
         case COMMAND_RECORDING:
            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            rms = calculateRMS((int16_t*)max_buff, buff_size / (DEFAULT_CHANNELS * 2));

            if (rms >= (backgroundRMS + TALKING_THRESHOLD_OFFSET)) {
               printf("COMMAND_RECORDING: Talking still in progress.\n");
               /* For an additional layer of "silence," I'm getting the length of the
                * vosk output to see if the volume was up but no one was saying
                * anything. */
               vosk_output_length = strlen(vosk_output);

               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_partial_result(recognizer);
               if (vosk_output == NULL) {
                  fprintf(stderr, "vosk_recognizer_partial_result() returned NULL!\n");
               } else {
                  printf("Partial Input: %s\n", vosk_output);
                  if (strlen(vosk_output) == vosk_output_length) {
                     vosk_nochange = 1;
                  }
               }
            }

            if (rms < (backgroundRMS + TALKING_THRESHOLD_OFFSET) || vosk_nochange) {
               printf(".");
               commandTimeout++;
               vosk_nochange = 0;
            } else {
               commandTimeout = 0;
            }

            if (commandTimeout >= DEFAULT_COMMAND_TIMEOUT) {
               printf("\n");
               commandTimeout = 0;
               printf("COMMAND_RECORDING: Command processing.\n");
               vosk_recognizer_accept_waveform(recognizer, max_buff, buff_size);
               vosk_output = vosk_recognizer_final_result(recognizer);
               if (vosk_output == NULL) {
                  fprintf(stderr, "vosk_recognizer_final_result() returned NULL!\n");
               } else {
                  printf("Input: %s\n", vosk_output);

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

                  memset(thisValue, '\0', sizeof(thisValue));
                  printf("Found command \"%s\".\n\tLooking for value in \"%s\".\n",
                         commands[i].actionWordsWildcard, commands[i].actionWordsRegex);

                  /* HERE */
                  strLength = strlen(commands[i].actionWordsRegex);
                  if ((strLength >= 2) && (commands[i].actionWordsRegex[strLength - 2] == '%') &&
                      (commands[i].actionWordsRegex[strLength - 1] == 's')) {
                     strncpy(thisSubstring, commands[i].actionWordsRegex, strLength - 2);
                     thisSubstring[strLength - 2] = '\0';
                     printf("extract_remaining_after_substring(\"%s\", %s\")\n", command_text, thisSubstring);
                     strcpy(thisValue, extract_remaining_after_substring(command_text, thisSubstring));
                  } else {
                     printf("sscanf(\"%s\", \"%s\", thisValue)\n",
                            command_text, commands[i].actionWordsRegex);
                     int retSs = sscanf(command_text, commands[i].actionWordsRegex, thisValue);
                     printf("sscanf() returned %d and \"%s\"\n", retSs, thisValue);
                  }
                  snprintf(thisCommand, sizeof(thisCommand),
                           commands[i].actionCommand, thisValue);
                  printf("Sending: \"%s\"\n", thisCommand);

                  rc = mosquitto_publish(mosq, NULL, commands[i].topic, strlen(thisCommand),
                                         thisCommand, 0, false);
                  if(rc != MOSQ_ERR_SUCCESS){
                     fprintf(stderr, "Error publishing: %s\n", mosquitto_strerror(rc));
                  }

                  break;
               }
            }

            if (i >= numCommands) {
               printf("Not detected as a command.\n");
#ifndef DISABLE_AI
               for (i = 0; i < numIgnoreWords; i++) {
                  if (strcmp(command_text, ignoreWords[i]) == 0) {
                     printf("Ignore word detected.\n");
                     break;
                  }
               }

               if (i < numIgnoreWords) {
                  printf("Input ignored. Found in ignore list.\n");
                  silenceNextState = WAKEWORD_LISTEN;
                  recState = SILENCE;
               } else {
                  response_text = getGptResponse(conversation_history, command_text);
                  if (response_text != NULL) {
                     printf("AI: %s\n", response_text);
                     text_to_speech(pcm_playback_device, response_text);

                     struct json_object *ai_message = json_object_new_object();
                     json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
                     json_object_object_add(ai_message, "content", json_object_new_string(response_text));
                     json_object_array_add(conversation_history, ai_message);

                     free(response_text);
                  } else {
                     fprintf(stderr, "GPT error.\n");
                     text_to_speech(pcm_playback_device, "I'm sorry but I'm currently unavailable boss.");
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

#ifdef ALSA_DEVICE
            snd_pcm_drop(myAudioControls.handle);
            if (snd_pcm_prepare(myAudioControls.handle) < 0) {
            fprintf(stderr, "Cannot prepare audio interface for use (%s)\n",
               snd_strerror(rc));
               exit(1);
            }
#else
            if (pa_simple_flush(myAudioControls.pa_handle, &error) != 0) {
               printf("Unable to flush buffer: %s\n", pa_strerror(error));
            }
            pa_simple_free(myAudioControls.pa_handle);

            myAudioControls.pa_handle = openPulseaudioCaptureDevice(pcm_capture_device);
            if (myAudioControls.pa_handle == NULL) {
               fprintf(stderr, "Error creating Pulse capture device.\n");
               return 1;
            }
#endif
            silenceNextState = WAKEWORD_LISTEN;
            recState = SILENCE;

            break;
         default:
            printf("I really shouldn't be here.\n");
      }
   }

   printf("Quit.\n");

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

   return 0;
}
