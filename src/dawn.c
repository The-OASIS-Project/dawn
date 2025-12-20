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

/* Enable GNU extensions for pthread_timedjoin_np() */
#define _GNU_SOURCE

/* Std C */
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* JSON */
#include <json-c/json.h>

/* CURL */
#include <curl/curl.h>

/* Mosquitto */
#include <mosquitto.h>

/* Speech to Text */
#include "asr/asr_interface.h"

/* Local */
#include "asr/vad_silero.h"
#include "audio/audio_backend.h"
#include "audio/audio_capture_thread.h"
#include "audio/flac_playback.h"
#include "core/command_router.h"
#include "core/session_manager.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "input_queue.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "network/accept_thread.h"
#include "network/dawn_network_audio.h"
#include "network/dawn_server.h"
#include "network/dawn_wav_utils.h"
#include "state_machine.h"
#include "text_to_command_nuevo.h"
#include "tools/search_summarizer.h"
#include "tools/smartthings_service.h"
#include "tts/text_to_speech.h"
#include "tts/tts_preprocessing.h"
#include "ui/metrics.h"
#ifdef ENABLE_TUI
#include "ui/tui.h"
#endif
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif
#ifdef ENABLE_AEC
#include "audio/aec_processor.h"
#endif
#include "version.h"

/* Configuration */
#include "config/config_env.h"
#include "config/config_parser.h"
#include "config/config_validate.h"
#include "config/dawn_config.h"

// Whisper chunking manager
#include "asr/chunking_manager.h"

// Define the default sample rate for audio capture.
#define DEFAULT_RATE 16000

// Define the default number of audio channels (1 for mono).
#define DEFAULT_CHANNELS 1

// Define the default duration of audio capture in seconds.
#define DEFAULT_CAPTURE_SECONDS 0.05f  // 50ms for responsive VAD (optimized for latency)

// Define the default command timeout in terms of iterations of DEFAULT_CAPTURE_SECONDS.
#define DEFAULT_COMMAND_TIMEOUT 24  // 24 * 0.05s = 1.2 seconds of silence before timeout

// VAD configuration - compile-time constants that don't need runtime config
#define VAD_SAMPLE_SIZE 512       // Silero VAD requires 512 samples (32ms at 16kHz)
#define VAD_TTS_DEBOUNCE_COUNT 3  // Consecutive detections required during TTS playback

// VAD thresholds and timing now come from g_config.vad.* and g_config.audio.bargein.*
// See config/dawn_config.h for the config struct definitions

// Music ducking configuration (reduces music volume during speech detection)
#define MUSIC_DUCK_FACTOR 0.3f         // Reduce volume to 30% when speech detected
#define MUSIC_DUCK_RESTORE_DELAY 2.0f  // Seconds of silence before restoring volume

// Default number of frames per audio period (used for buffer size calculations)
#define DEFAULT_FRAMES 64

static char pcm_capture_device[MAX_WORD_LENGTH + 1] = "";
static char pcm_playback_device[MAX_WORD_LENGTH + 1] = "";

/* Parsed audio devices. */
static audioDevices captureDevices[MAX_AUDIO_DEVICES]; /**< Audio capture devices. */
static int numAudioCaptureDevices = 0;                 /**< How many capture devices. */

static audioDevices playbackDevices[MAX_AUDIO_DEVICES]; /**< Audio playback devices. */
static int numAudioPlaybackDevices = 0;                 /**< How many playback devices. */

/**
 * @struct audioControl
 * @brief Legacy structure for audio buffer size calculations.
 *
 * Previously held ALSA/PulseAudio handles, now only used for buffer size tracking
 * since actual audio capture is done by audio_capture_thread using the audio_backend API.
 *
 * @var audioControl::full_buff_size
 * Size of the buffer to be filled in each read operation.
 */
typedef struct {
   uint32_t full_buff_size;
} audioControl;

// Audio capture thread context (dedicated thread for continuous audio capture)
static audio_capture_context_t *audio_capture_ctx = NULL;

// Silero VAD context for voice activity detection
static silero_vad_context_t *vad_ctx = NULL;

// Wake word prefixes that get combined with ai_name at runtime
static const char *wakeWordPrefixes[] = { "hello ",    "okay ",         "alright ",
                                          "hey ",      "hi ",           "good evening ",
                                          "good day ", "good morning ", "yeah ",
                                          "k " };
#define NUM_WAKE_WORDS (sizeof(wakeWordPrefixes) / sizeof(wakeWordPrefixes[0]))
#define WAKE_WORD_BUF_SIZE 64

// Static buffers for wake words (built at runtime from config)
static char wakeWordBuffers[NUM_WAKE_WORDS][WAKE_WORD_BUF_SIZE];
static char *wakeWords[NUM_WAKE_WORDS];

// Initialize wake words from config ai_name
static void init_wake_words(void) {
   for (size_t i = 0; i < NUM_WAKE_WORDS; i++) {
      snprintf(wakeWordBuffers[i], WAKE_WORD_BUF_SIZE, "%s%s", wakeWordPrefixes[i],
               g_config.general.ai_name);
      wakeWords[i] = wakeWordBuffers[i];
   }
}

// Array of words/phrases used to signal the end of an interaction with the AI.
static char *goodbyeWords[] = { "good bye", "goodbye", "good night", "bye", "quit", "exit" };

// Array of predefined responses the AI can use upon recognizing a wake word/phrase.
const char *wakeResponses[] = { "Hello Sir.", "At your service Sir.", "Yes Sir?",
                                "How may I assist you Sir?", "Listening Sir." };

// Array of words/phrases that the AI should explicitly ignore during interaction.
// This includes common filler words or phrases signaling to disregard the prior input.
static char *ignoreWords[] = { "", "the", "cancel", "never mind", "nevermind", "ignore" };

// Array of words/phrases that we accept as a way to tell the AI to cancel its current
// text to speech instead of requiring another command.
static char *cancelWords[] = { "stop",      "stop it",        "cancel",        "hold on",
                               "wait",      "never mind",     "abort",         "pause",
                               "enough",    "disregard",      "no thanks",     "forget it",
                               "leave it",  "drop it",        "stand by",      "cease",
                               "interrupt", "say no more",    "shut up",       "silence",
                               "zip it",    "enough already", "that's enough", "stop right there" };

// Standard greeting messages based on the time of day.
const char *morning_greeting = "Good morning boss.";
const char *day_greeting = "Good day Sir.";
const char *evening_greeting = "Good evening Sir.";

/* State machine enum defined in state_machine.h (dawn_state_t) */

// Define the shared variables for tts state
pthread_cond_t tts_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t tts_mutex = PTHREAD_MUTEX_INITIALIZER;
// Note: tts_playback_state is defined in text_to_speech.cpp (mutex-protected, not atomic)

// Define the shared variables for LLM thread state
pthread_mutex_t llm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t llm_thread;
volatile int llm_processing = 0;     // Flag: 1 if LLM thread is running, 0 otherwise
struct timeval pipeline_start_time;  // Track when pipeline processing starts

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

/**
 * @brief Flag to request application restart via self-exec
 *
 * When set to 1, the main loop will exit and re-execute the application
 * using execve() to apply configuration changes that require a restart.
 * The PID remains the same, but all state is reset.
 */
volatile sig_atomic_t g_restart_requested = 0;

/* Saved command-line arguments for restart via execve()
 * Using static allocation to avoid heap fragmentation on embedded systems.
 * Limits: 32 arguments max, 256 chars per argument (8KB total in BSS). */
#define MAX_SAVED_ARGC 32
#define MAX_SAVED_ARG_LEN 256

static int s_saved_argc = 0;
static char *s_saved_argv[MAX_SAVED_ARGC + 1];  // Pointer array (NULL terminated)
static char s_saved_argv_storage[MAX_SAVED_ARGC][MAX_SAVED_ARG_LEN];  // String storage

/**
 * @brief Save command-line arguments for restart via execve()
 *
 * This function must be called early in main() to preserve the original
 * argv array. The arguments are copied into static storage to ensure they
 * remain valid for the lifetime of the process.
 *
 * @param argc Argument count from main()
 * @param argv Argument vector from main()
 */
static void dawn_save_argv(int argc, char *argv[]) {
   if (argc > MAX_SAVED_ARGC) {
      LOG_WARNING("Too many arguments (%d > %d), truncating for restart", argc, MAX_SAVED_ARGC);
      argc = MAX_SAVED_ARGC;
   }

   s_saved_argc = argc;

   // Copy each argument into static storage
   for (int i = 0; i < argc; i++) {
      size_t len = strlen(argv[i]);
      if (len >= MAX_SAVED_ARG_LEN) {
         LOG_WARNING("Argument %d too long (%zu >= %d), truncating", i, len, MAX_SAVED_ARG_LEN);
         len = MAX_SAVED_ARG_LEN - 1;
      }
      memcpy(s_saved_argv_storage[i], argv[i], len);
      s_saved_argv_storage[i][len] = '\0';
      s_saved_argv[i] = s_saved_argv_storage[i];
   }
   s_saved_argv[argc] = NULL;  // NULL terminate for execve()

   LOG_INFO("Saved %d command-line arguments for potential restart", argc);
}

/**
 * @brief Request application restart via self-exec
 *
 * This function sets the restart flag which causes the main loop to exit
 * and re-execute the application using execve(). This is used to apply
 * configuration changes that require a full restart.
 *
 * Thread-safe: Uses sig_atomic_t for the flag.
 */
void dawn_request_restart(void) {
   LOG_INFO("Restart requested - will restart after cleanup");
   g_restart_requested = 1;
   quit = 1;  // Trigger main loop exit
}

/* MQTT */
static struct mosquitto *mosq;

// Is remote DAWN enabled
static int enable_network_audio = 0;

#ifdef ENABLE_TUI
// TUI configuration
static int enable_tui = 0;
static tui_theme_t tui_theme = TUI_THEME_GREEN;  // Default: Apple ][ green
#endif
pthread_mutex_t processing_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t processing_done = PTHREAD_COND_INITIALIZER;
uint8_t *processing_result_data = NULL;
size_t processing_result_size = 0;
int processing_complete = 0;

// NetworkPCMData struct is defined in network/dawn_wav_utils.h
// NOTE: Network audio is now handled by worker threads (see worker_pool.c)

// Global variable for command processing mode
command_processing_mode_t command_processing_mode = CMD_MODE_DIRECT_ONLY;

// Pointer to local session's conversation history
// NOTE: This is a convenience pointer to session_get_local()->conversation_history
// For multi-client support, each session has its own history (see session_manager.c)
struct json_object *conversation_history = NULL;

// =============================================================================
// Direct-only mode system prompt (persona + system instructions)
// =============================================================================
// Used when command_processing_mode is CMD_MODE_DIRECT_ONLY.
// Combines custom persona (or default AI_PERSONA) with dynamic system instructions.
#define DIRECT_PROMPT_BUFFER_SIZE 16384
static char direct_mode_prompt[DIRECT_PROMPT_BUFFER_SIZE];
static int direct_mode_prompt_initialized = 0;

/**
 * @brief Gets the system prompt for direct-only command processing mode
 *
 * Returns a combined prompt with:
 * - Persona (from config or AI_PERSONA default)
 * - System instructions (dynamically built based on enabled features)
 *
 * This ensures consistent LLM behavior (response length, formatting rules)
 * even when using a custom persona. Only includes instructions for
 * features that are actually enabled (search, vision, etc.).
 */
static const char *get_direct_mode_prompt(void) {
   if (!direct_mode_prompt_initialized) {
      const char *persona = (g_config.persona.description[0] != '\0') ? g_config.persona.description
                                                                      : AI_PERSONA;
      snprintf(direct_mode_prompt, DIRECT_PROMPT_BUFFER_SIZE, "%s\n\n%s", persona,
               get_system_instructions());
      direct_mode_prompt_initialized = 1;
   }
   return direct_mode_prompt;
}

// Barge-in control: when true, speech detection is disabled during TTS playback
static int g_bargein_disabled = 1;       // Start disabled until after boot calibration
static int g_bargein_user_disabled = 0;  // Set by --no-bargein CLI option

// Shared buffers for LLM thread communication (protected by llm_mutex)
static char *llm_request_text = NULL;     // Input: command text for LLM
static char *llm_response_text = NULL;    // Output: LLM response
static char *llm_vision_image = NULL;     // Input: vision image data
static size_t llm_vision_image_size = 0;  // Input: vision image size

/**
 * @brief Callback for streaming LLM responses with TTS
 *
 * This callback is called for each complete sentence from the streaming LLM.
 * It cleans the sentence and sends it directly to TTS for immediate playback.
 */
static void dawn_tts_sentence_callback(const char *sentence, void *userdata) {
   if (!sentence || strlen(sentence) == 0) {
      return;
   }

   // Make a copy for cleaning
   char *cleaned = strdup(sentence);
   if (!cleaned) {
      return;
   }

   // Remove command tags (they'll be processed from the full response later)
   char *cmd_start, *cmd_end;
   while ((cmd_start = strstr(cleaned, "<command>")) != NULL) {
      cmd_end = strstr(cmd_start, "</command>");
      if (cmd_end) {
         cmd_end += strlen("</command>");
         memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
      } else {
         // Incomplete command tag - strip from <command> to end of string
         // This handles cases where the stream breaks between tags
         *cmd_start = '\0';
         break;
      }
   }

   // Remove <end_of_turn> tags (local AI models)
   char *match = NULL;
   if ((match = strstr(cleaned, "<end_of_turn>")) != NULL) {
      *match = '\0';
   }

   // Remove special characters that cause problems
   remove_chars(cleaned, "*");
   remove_emojis(cleaned);

   // Trim whitespace
   size_t len = strlen(cleaned);
   while (len > 0 && (cleaned[len - 1] == ' ' || cleaned[len - 1] == '\t' ||
                      cleaned[len - 1] == '\n' || cleaned[len - 1] == '\r')) {
      cleaned[--len] = '\0';
   }

   // Only send to TTS if there's actual content
   if (len > 0) {
      text_to_speech(cleaned);

      // Add natural pause between sentences (300ms)
      usleep(300000);
   }

   free(cleaned);
}

sig_atomic_t get_quit(void) {
   return quit;
}

int is_llm_processing(void) {
   return llm_processing;
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
      // Request LLM interrupt (safe to call from signal handler - uses sig_atomic_t)
      llm_request_interrupt();
      quit = 1;
   }
}

char *textToSpeechCallback(const char *actionName, char *value, int *should_respond) {
   LOG_INFO("Received text to speech command: \"%s\"\n", value);

   // TTS commands always execute directly, regardless of mode
   if (should_respond != NULL) {
      *should_respond = 0;  // No need to respond about TTS
   }
   text_to_speech(value);
   return NULL;
}

const char *getPcmPlaybackDevice(void) {
   return (const char *)pcm_playback_device;
}

const char *getPcmCaptureDevice(void) {
   return (const char *)pcm_capture_device;
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

char *setPcmPlaybackDevice(const char *actionName, char *value, int *should_respond) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];
   static char return_buffer[MAX_COMMAND_LENGTH];

   *should_respond = 1;  // We always respond for device changes

   // Handle NULL value (e.g., from "get" action without a value)
   if (value == NULL) {
      LOG_WARNING("setPcmPlaybackDevice called with NULL value (action: %s)",
                  actionName ? actionName : "NULL");
      snprintf(return_buffer, MAX_COMMAND_LENGTH, "Current audio playback device: %s",
               pcm_playback_device);
      return return_buffer;
   }

   for (i = 0; i < numAudioPlaybackDevices; i++) {
      if (strcmp(playbackDevices[i].name, value) == 0) {
         LOG_INFO("Setting audio playback device to \"%s\".\n", playbackDevices[i].device);
         strncpy(pcm_playback_device, playbackDevices[i].device, MAX_WORD_LENGTH);
         pcm_playback_device[MAX_WORD_LENGTH] = '\0';  // Ensure null termination

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            snprintf(speech, MAX_COMMAND_LENGTH, "Switching playback device to %s.", value);
            text_to_speech(speech);
            return NULL;  // Already handled
         } else {
            // AI modes: return the result for AI to process
            snprintf(return_buffer, MAX_COMMAND_LENGTH, "Audio playback device switched to %s",
                     value);
            return return_buffer;
         }
      }
   }

   if (i >= numAudioPlaybackDevices) {
      LOG_ERROR("Requested audio playback device not found.\n");

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         snprintf(speech, MAX_COMMAND_LENGTH,
                  "Sorry sir. A playback device called %s was not found.", value);
         text_to_speech(speech);
         return NULL;
      } else {
         snprintf(return_buffer, MAX_COMMAND_LENGTH, "Audio playback device '%s' not found", value);
         return return_buffer;
      }
   }

   return NULL;  // Should never reach here
}

char *setPcmCaptureDevice(const char *actionName, char *value, int *should_respond) {
   int i = 0;
   char speech[MAX_COMMAND_LENGTH];
   static char return_buffer[MAX_COMMAND_LENGTH];

   *should_respond = 1;

   // Handle NULL value (e.g., from "get" action without a value)
   if (value == NULL) {
      LOG_WARNING("setPcmCaptureDevice called with NULL value (action: %s)",
                  actionName ? actionName : "NULL");
      snprintf(return_buffer, MAX_COMMAND_LENGTH, "Current audio capture device: %s",
               pcm_capture_device);
      return return_buffer;
   }

   for (i = 0; i < numAudioCaptureDevices; i++) {
      if (strcmp(captureDevices[i].name, value) == 0) {
         LOG_INFO("Setting audio capture device to \"%s\".\n", captureDevices[i].device);
         strncpy(pcm_capture_device, captureDevices[i].device, MAX_WORD_LENGTH);
         pcm_capture_device[MAX_WORD_LENGTH] = '\0';  // Ensure null termination

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            snprintf(speech, MAX_COMMAND_LENGTH, "Switching capture device to %s.", value);
            text_to_speech(speech);
            return NULL;
         } else {
            snprintf(return_buffer, MAX_COMMAND_LENGTH, "Audio capture device switched to %s",
                     value);
            return return_buffer;
         }
      }
   }

   if (i >= numAudioCaptureDevices) {
      LOG_ERROR("Requested audio capture device not found.\n");

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         snprintf(speech, MAX_COMMAND_LENGTH,
                  "Sorry sir. A capture device called %s was not found.", value);
         text_to_speech(speech);
         return NULL;
      } else {
         snprintf(return_buffer, MAX_COMMAND_LENGTH, "Audio capture device '%s' not found", value);
         return return_buffer;
      }
   }

   return NULL;
}

/**
 * Normalize text for wake word/command matching
 * Converts to lowercase and removes punctuation/special characters
 *
 * @param input Original text
 * @return Normalized text (caller must free), or NULL on error
 */
static char *normalize_for_matching(const char *input) {
   if (!input) {
      return NULL;
   }

   size_t len = strlen(input);
   char *normalized = (char *)malloc(len + 1);
   if (!normalized) {
      return NULL;
   }

   size_t j = 0;
   for (size_t i = 0; i < len; i++) {
      char c = input[i];
      // Convert to lowercase
      if (c >= 'A' && c <= 'Z') {
         normalized[j++] = c + 32;
      }
      // Keep lowercase letters, digits, and spaces
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ') {
         normalized[j++] = c;
      }
      // Skip punctuation and other characters
   }
   normalized[j] = '\0';

   return normalized;
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

// Legacy openAlsaPcmCaptureDevice() and openPulseaudioCaptureDevice() removed.
// Audio capture is now handled by audio_capture_thread using the audio_backend API.

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
const char *timeOfDayGreeting(void) {
   time_t t = time(NULL);
   struct tm *local_time = localtime(&t);  // Obtain local time from the system clock.

   int hour = local_time->tm_hour;  // Extract the hour component of the current time.

   // Select the appropriate greeting based on the hour of the day.
   if (hour >= 3 && hour < 12) {
      return morning_greeting;  // Morning greeting from 3 AM to before 12 PM.
   } else if (hour >= 12 && hour < 18) {
      return day_greeting;  // Day greeting from 12 PM to before 6 PM.
   } else {
      return evening_greeting;  // Evening greeting for 6 PM onwards.
   }
}

/**
 * Selects a random acknowledgment response to a wake word detection.
 *
 * This function is designed to provide variability in the AI's response to
 * being activated by a wake word. It randomly selects one of the predefined
 * responses from the global `wakeResponses` array each time it's called.
 *
 * @return A pointer to a constant character string containing the selected wake word
 * acknowledgment. The return value points to an element within the global `wakeResponses` array and
 *         should not be modified or freed.
 */
const char *wakeWordAcknowledgment() {
   int numWakeResponses = sizeof(wakeResponses) /
                          sizeof(wakeResponses[0]);  // Calculate the number of available responses.
   int choice;

   srand(time(NULL));                   // Seed the random number generator.
   choice = rand() % numWakeResponses;  // Generate a random index to select a response.

   return wakeResponses[choice];  // Return the randomly selected wake word acknowledgment.
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
 * @param myAudioControls A pointer to an audioControl structure containing audio capture settings
 * and state.
 * @param max_buff A pointer to the buffer where captured audio data will be accumulated.
 * @param max_buff_size The maximum size of max_buff, indicating how much data can be stored.
 * @param ret_buff_size A pointer to an integer where the function will store the total size of
 * captured audio data stored in max_buff upon successful completion.
 * @return Returns 0 on successful capture of audio data without filling the buffer.
 *         Returns 1 if a memory allocation failure occurs or an error occurred during audio
 * capture, such as a failure to read from the audio device.
 *
 * @note The function updates *ret_buff_size with the total size of captured audio data.
 *       It's crucial to ensure that max_buff is large enough to hold the expected amount of data.
 */
/**
 * @brief Capture audio from the ring buffer filled by the capture thread
 *
 * This function reads audio data from the ring buffer that is continuously
 * filled by the dedicated audio capture thread. This replaces the old
 * direct device reading approach with a non-blocking ring buffer read.
 *
 * @param myAudioControls Audio control structure (unused, kept for API compatibility)
 * @param max_buff Destination buffer for audio data
 * @param max_buff_size Maximum size of destination buffer
 * @param ret_buff_size Pointer to store actual bytes read
 * @return 0 on success, 1 on error
 */
int capture_buffer(audioControl *myAudioControls,
                   char *max_buff,
                   uint32_t max_buff_size,
                   int *ret_buff_size) {
   (void)myAudioControls;  // Unused parameter (kept for API compatibility)

   *ret_buff_size = 0;

   if (!audio_capture_ctx) {
      LOG_ERROR("Audio capture thread not initialized");
      return 1;
   }

   // Check if capture thread is still running
   if (!audio_capture_is_running(audio_capture_ctx)) {
      LOG_ERROR("Audio capture thread has stopped unexpectedly");
      return 1;
   }

   // Wait for enough data to be available in ring buffer (at least max_buff_size)
   // Timeout of 2 seconds to prevent indefinite blocking
   size_t available = audio_capture_wait_for_data(audio_capture_ctx, max_buff_size, 2000);
   if (available < max_buff_size) {
      LOG_WARNING("Timeout waiting for audio data (got %zu bytes, needed %u)", available,
                  max_buff_size);
      // Read whatever is available
      if (available > 0) {
         *ret_buff_size = audio_capture_read(audio_capture_ctx, max_buff, available);
      }
      return 0;  // Not a fatal error, return partial data
   }

   // Read the requested amount of data from ring buffer
   *ret_buff_size = audio_capture_read(audio_capture_ctx, max_buff, max_buff_size);

   return 0;  // Success
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

/**
 * @brief Check if vision AI image is ready for processing
 *
 * @return 1 if vision image is ready, 0 otherwise
 */
int dawn_vision_is_ready(void) {
   return vision_ai_ready;
}

/**
 * @brief Get the pending vision AI image
 *
 * @param size_out Output parameter for image size
 * @return Base64-encoded image data, or NULL if not ready
 */
const char *dawn_vision_get_image(size_t *size_out) {
   if (!vision_ai_ready || !vision_ai_image) {
      return NULL;
   }
   if (size_out) {
      *size_out = vision_ai_image_size;
   }
   return vision_ai_image;
}

/**
 * @brief Clear the vision AI image after it has been used
 *
 * Call this after consuming the vision image to free resources
 * and allow new images to be captured.
 */
void dawn_vision_clear(void) {
   if (vision_ai_image) {
      free(vision_ai_image);
      vision_ai_image = NULL;
   }
   vision_ai_image_size = 0;
   vision_ai_ready = 0;
}

dawn_state_t currentState = DAWN_STATE_INVALID;

/**
 * @brief Saves the conversation history JSON to a timestamped file.
 *
 * This function creates a filename with the current date and time, then writes
 * the entire conversation history JSON object to that file for later analysis
 * or debugging purposes.
 *
 * @param conversation_history The JSON object containing the chat conversation
 * @return 0 on success, -1 on failure
 */
int save_conversation_history(struct json_object *conversation_history) {
   FILE *chat_file = NULL;
   time_t current_time;
   struct tm *time_info;
   char filename[256];
   const char *json_string = NULL;

   if (conversation_history == NULL) {
      LOG_ERROR("Cannot save NULL conversation history");
      return -1;
   }

   // Get current time for filename
   time(&current_time);
   time_info = localtime(&current_time);

   // Create timestamped filename: chat_history_YYYYMMDD_HHMMSS.json
   strftime(filename, sizeof(filename), "chat_history_%Y%m%d_%H%M%S.json", time_info);

   // Open file for writing
   chat_file = fopen(filename, "w");
   if (chat_file == NULL) {
      LOG_ERROR("Failed to open chat history file: %s", filename);
      return -1;
   }

   // Convert JSON object to pretty-printed string
   json_string = json_object_to_json_string_ext(
       conversation_history, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
   if (json_string == NULL) {
      LOG_ERROR("Failed to convert conversation history to JSON string");
      fclose(chat_file);
      return -1;
   }

   // Write JSON string to file
   if (fprintf(chat_file, "%s\n", json_string) < 0) {
      LOG_ERROR("Failed to write conversation history to file: %s", filename);
      fclose(chat_file);
      return -1;
   }

   fclose(chat_file);
   LOG_INFO("Conversation history saved to: %s", filename);

   return 0;
}

/* Mutex for thread-safe conversation management */
static pthread_mutex_t conversation_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Reset conversation context (save current, start fresh)
 *
 * Saves the current conversation to a JSON file, then resets the
 * conversation history to just the system message. Also resets metrics.
 * Can be triggered by TUI 'R' key or voice command.
 *
 * @note Thread-safe: Uses conversation_mutex for synchronization.
 */
void reset_conversation(void) {
   pthread_mutex_lock(&conversation_mutex);

   LOG_INFO("Resetting conversation context...");

   session_t *local_session = session_get_local();
   if (!local_session) {
      LOG_ERROR("Failed to get local session for reset");
      pthread_mutex_unlock(&conversation_mutex);
      return;
   }

   /* Save current conversation before clearing */
   if (conversation_history != NULL && json_object_array_length(conversation_history) > 1) {
      save_conversation_history(conversation_history);
   }

   /* Reset local session with appropriate system prompt */
   const char *system_prompt;
   if (command_processing_mode == CMD_MODE_LLM_ONLY ||
       command_processing_mode == CMD_MODE_DIRECT_FIRST) {
      system_prompt = get_local_command_prompt();
   } else {
      /* Direct-only mode: use persona + system instructions for consistent behavior */
      system_prompt = get_direct_mode_prompt();
   }
   session_init_system_prompt(local_session, system_prompt);

   /* Update global pointer to the new history */
   conversation_history = local_session->conversation_history;

   /* Reset metrics */
   metrics_reset();

   LOG_INFO("Conversation reset complete - fresh context ready");
   metrics_log_activity("Conversation reset - fresh context");

   pthread_mutex_unlock(&conversation_mutex);
}

/**
 * @brief Reset all subsystems for a new utterance
 *
 * Resets VAD, ASR, chunking manager, duration tracking, and pre-roll buffer.
 * Call this when transitioning back to DAWN_STATE_WAKEWORD_LISTEN or DAWN_STATE_SILENCE state
 * to ensure clean state for the next interaction.
 *
 * @param vad_ctx Silero VAD context to reset (can be NULL)
 * @param asr_ctx ASR context to reset (must not be NULL)
 * @param chunk_mgr Chunking manager to reset (can be NULL if Vosk mode)
 * @param silence_duration Pointer to silence duration to reset
 * @param speech_duration Pointer to speech duration to reset
 * @param recording_duration Pointer to recording duration to reset
 * @param preroll_write_pos Pointer to pre-roll write position to reset
 * @param preroll_valid_bytes Pointer to pre-roll valid bytes to reset
 */
static void reset_for_new_utterance(silero_vad_context_t *vad_ctx,
                                    asr_context_t *asr_ctx,
                                    chunking_manager_t *chunk_mgr,
                                    float *silence_duration,
                                    float *speech_duration,
                                    float *recording_duration,
                                    size_t *preroll_write_pos,
                                    size_t *preroll_valid_bytes) {
   // Reset VAD state for new interaction boundary
   if (vad_ctx) {
      vad_silero_reset(vad_ctx);
   }

   // Reset chunking manager (Whisper only)
   if (chunk_mgr) {
      chunking_manager_reset(chunk_mgr);
   }

   // Reset ASR for next utterance
   asr_reset(asr_ctx);

   // Reset duration tracking
   *silence_duration = 0.0f;
   *speech_duration = 0.0f;
   *recording_duration = 0.0f;

   // Reset pre-roll buffer
   *preroll_write_pos = 0;
   *preroll_valid_bytes = 0;
}

/**
 * @brief Check input queue and process any pending text commands.
 *
 * Checks the unified input queue for pending commands from any source
 * (TUI, future REST, WebSocket, etc.) and processes them if found.
 *
 * @param command_text_out Pointer to command text buffer to populate.
 * @param rec_state Output state to transition to on success.
 * @param silence_next_state State to return to after processing.
 * @return 1 if input was processed and state should change, 0 otherwise.
 */
static int check_and_process_input_queue(char **command_text_out,
                                         dawn_state_t *rec_state,
                                         dawn_state_t *silence_next_state) {
   if (!input_queue_has_item()) {
      return 0;
   }

   queued_input_t input;
   if (!input_queue_pop(&input)) {
      return 0;
   }

   LOG_INFO("Text input from %s: %s", input_source_name(input.source), input.text);

   *command_text_out = strdup(input.text);
   if (*command_text_out == NULL) {
      LOG_ERROR("Failed to allocate memory for text input from %s",
                input_source_name(input.source));
      return 0;
   }

   *silence_next_state = DAWN_STATE_WAKEWORD_LISTEN;
   *rec_state = DAWN_STATE_PROCESS_COMMAND;
   return 1;
}

/* Publish AI State. Only send state if it's changed.
 * Note: If LLM is processing in background thread, we override the state
 * to PROCESS_COMMAND so the HUD shows "thinking/working" status. */
int publish_ai_state(dawn_state_t newState) {
   int rc = 0;

   /* Override state to PROCESS_COMMAND while LLM thread is running.
    * The main state machine returns to SILENCE for barge-in detection,
    * but externally we want to show that the AI is "thinking". */
   if (llm_processing && newState == DAWN_STATE_SILENCE) {
      newState = DAWN_STATE_PROCESS_COMMAND;
   }

   if (newState == currentState || newState == DAWN_STATE_INVALID) {
      return 0;
   }

   const char *state_name = dawn_state_name(newState);
   if (strcmp(state_name, "UNKNOWN") == 0) {
      LOG_ERROR("Unknown state: %d", newState);
      return 1;
   }

   // Build JSON using json-c
   struct json_object *json = json_object_new_object();
   if (!json) {
      LOG_ERROR("Error creating JSON object for AI state.");
      return 1;
   }

   json_object_object_add(json, "device", json_object_new_string("ai"));
   json_object_object_add(json, "name", json_object_new_string(g_config.general.ai_name));
   json_object_object_add(json, "state", json_object_new_string(state_name));

   const char *json_str = json_object_to_json_string(json);
   rc = mosquitto_publish(mosq, NULL, "hud", strlen(json_str), json_str, 0, false);
   if (rc != MOSQ_ERR_SUCCESS) {
      LOG_ERROR("Error publishing: %s\n", mosquitto_strerror(rc));
      json_object_put(json);
      return 1;
   }

   json_object_put(json);

   // Update metrics with state transition
   metrics_update_state(newState);

   currentState = newState;  // Update the state after successful publish

   return 0;
}

/**
 * Displays help information for the program, outlining the usage and available command-line
 * options. The function dynamically adjusts the usage message based on whether the program name is
 * available from the command-line arguments.
 *
 * @param argc The number of command-line arguments passed to the program.
 * @param argv The array of command-line arguments. argv[0] is expected to contain the program name.
 */
void display_help(int argc, char *argv[]) {
   if (argc > 0) {
      printf("Usage: %s [options]\n\n", argv[0]);
   } else {
      printf("Usage: [options]\n\n");
   }

   // Print the list of available command-line options.
   printf("Options:\n");
   printf("  -c, --capture DEVICE   Specify the PCM capture device.\n");
   printf("  -d, --playback DEVICE  Specify the PCM playback device.\n");
   printf("  -l, --logfile LOGFILE  Specify the log filename instead of stdout/stderr.\n");
   printf("  -N, --network-audio    Enable network audio processing server\n");
   printf("  -h, --help             Display this help message and exit.\n");
   printf("\nConfiguration:\n");
   printf("  --config PATH          Load configuration from PATH (default: search order).\n");
   printf("  --dump-config          Print effective configuration and exit.\n");
   printf("  --dump-settings        Print all settings with env vars and sources, then exit.\n");
   printf("  --audio-backend TYPE   Audio backend: auto (default), alsa, or pulse.\n");
   printf("  -m, --llm TYPE         Set default LLM type (cloud or local).\n");
   printf("  -P, --cloud-provider PROVIDER    Set cloud provider (openai or claude).\n");
   printf(
       "  -A, --asr-engine ENGINE          Set ASR engine (vosk or whisper, default: whisper).\n");
   printf("  -W, --whisper-model MODEL        Whisper model size (tiny, base, small, default: "
          "base).\n");
   printf("  -w, --whisper-path PATH          Path to Whisper models directory (default: "
          "../whisper.cpp/models).\n");
   printf("  -M, --music-dir PATH             Absolute path to music directory (default: "
          "~/Music).\n");
   printf("Search Summarization:\n");
   printf("  --summarize-backend BACKEND      Summarize large search results (disabled, local, "
          "default).\n");
   printf("  --summarize-threshold BYTES      Size threshold for summarization (default: 3072).\n");
   printf("Command Processing Modes:\n");
   printf("  -D, --commands-only    Direct command processing only (default).\n");
   printf("  -C, --llm-commands     Try direct commands first, then LLM if no match.\n");
   printf("  -L, --llm-only         LLM handles all commands, skip direct processing.\n");
#ifdef ENABLE_TUI
   printf("\nTerminal UI Options:\n");
   printf("  -T, --tui              Enable terminal UI dashboard.\n");
   printf("  -t, --theme THEME      TUI color theme (green, blue, bw). Default: green.\n");
#endif
#ifdef ENABLE_AEC
   printf("\nAEC Debug Options:\n");
   printf("  -R, --aec-record[=DIR] Record mic/ref/out audio during TTS (default dir: /tmp).\n");
#endif
   printf("\nMic Debug Options:\n");
   printf("  -r, --mic-record[=DIR] Record mic input during TTS (what VAD sees). "
          "Default: /tmp.\n");
   printf("  -a, --asr-record[=DIR] Record ASR input pre/post normalization. Default: /tmp.\n");
   printf("  -B, --no-bargein       Disable barge-in (ignore speech during TTS playback).\n");
}

/**
 * @brief LLM worker thread function
 *
 * This function runs in a separate thread to process LLM requests without blocking
 * the main audio processing loop. It reads the request data from shared buffers,
 * calls the LLM API, stores the response, and signals completion.
 *
 * @param arg Unused (required for pthread compatibility)
 * @return NULL
 */
void *llm_worker_thread(void *arg) {
   (void)arg;  // Unused

   // Lock mutex to access shared data
   pthread_mutex_lock(&llm_mutex);

   // Copy request data to local variables (minimize time holding mutex)
   char *request_text = llm_request_text ? strdup(llm_request_text) : NULL;
   char *vision_image = llm_vision_image ? malloc(llm_vision_image_size) : NULL;
   size_t vision_image_size = llm_vision_image_size;

   if (vision_image && llm_vision_image) {
      memcpy(vision_image, llm_vision_image, llm_vision_image_size);
   }

   pthread_mutex_unlock(&llm_mutex);

   // Clear interrupt flag before calling LLM
   llm_clear_interrupt();

   // Get local session's LLM config (for per-session LLM settings)
   session_t *local_session = session_get_local();
   session_llm_config_t session_config;
   session_get_llm_config(local_session, &session_config);

   // Resolve to final config (merges session override with global)
   llm_resolved_config_t resolved_config;
   llm_resolve_config(&session_config, &resolved_config);

   // Call LLM with resolved config (this can take 10+ seconds)
   // Note: conversation_history is a global, but only main thread modifies it during setup
   // and we only read it here, so no mutex needed for conversation_history itself
   char *response = llm_chat_completion_streaming_tts_with_config(
       conversation_history, request_text, vision_image, vision_image_size,
       dawn_tts_sentence_callback, NULL, &resolved_config);

   // Free local copies
   if (request_text) {
      free(request_text);
   }
   if (vision_image) {
      free(vision_image);
   }

   // Lock mutex to store response
   pthread_mutex_lock(&llm_mutex);

   // Store response (or NULL if interrupted/failed)
   if (llm_response_text) {
      free(llm_response_text);
   }
   llm_response_text = response;  // Transfer ownership

   // Mark processing complete
   llm_processing = 0;


   pthread_mutex_unlock(&llm_mutex);

   LOG_INFO("LLM worker thread finished");
   return NULL;
}

int main(int argc, char *argv[]) {
   char *input_text = NULL;
   char *command_text = NULL;
   char *response_text = NULL;
   asr_result_t *asr_result = NULL;
   size_t prev_text_length = 0;
   int text_nochange = 0;
   int rc = 0;
   int opt = 0;
   const char *log_filename = NULL;

   // Audio Buffer
   uint32_t buff_size = 0;
   float temp_buff_size = DEFAULT_RATE * DEFAULT_CHANNELS * sizeof(int16_t) *
                          DEFAULT_CAPTURE_SECONDS;
   uint32_t max_buff_size = (uint32_t)ceil(temp_buff_size);
   char *max_buff = NULL;

   // Command Configuration
   FILE *configFile = NULL;
   char buffer[10 * 1024];
   int bytes_read = 0;

   // Command Parsing
   actionType actions[MAX_ACTIONS]; /**< All of the available actions read from the JSON. */
   int numActions = 0;              /**< Total actions in the actions array. */

   commandSearchElement commands[MAX_COMMANDS];
   int numCommands = 0;

   char *next_char_ptr = NULL;

   audioControl myAudioControls;

   int commandTimeout = 0;

   /* Array Counts */
   int numGoodbyeWords = sizeof(goodbyeWords) / sizeof(goodbyeWords[0]);
   int numWakeWords = NUM_WAKE_WORDS;
   int numIgnoreWords = sizeof(ignoreWords) / sizeof(ignoreWords[0]);
   int numCancelWords = sizeof(cancelWords) / sizeof(cancelWords[0]);

   int i = 0;

   dawn_state_t recState = DAWN_STATE_SILENCE;
   dawn_state_t silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;

   // VAD state tracking
   float vad_speech_prob = 0.0f;
   int silence_count = 0;
   float silence_duration = 0.0f;
   float speech_duration = 0.0f;     // For pause detection / chunking
   float recording_duration = 0.0f;  // Total recording time (prevents buffer overflow)

// Pre-roll buffer: captures audio before VAD trigger to avoid missing first words
// Size calculated from config: (sample_rate * channels * bytes_per_sample * ms / 1000)
// Maximum 2 seconds (64000 bytes at 16kHz mono 16-bit) for safety
// Using static array for predictable embedded memory footprint (avoids heap fragmentation)
#define VAD_PREROLL_MAX_MS 2000
#define VAD_PREROLL_MAX_BYTES 64000
#define VAD_PREROLL_DEFAULT_MS 500
   static unsigned char preroll_buffer[VAD_PREROLL_MAX_BYTES];
   static size_t preroll_buffer_size = 0;  // Actual usable size from config
   size_t preroll_write_pos = 0;
   size_t preroll_valid_bytes = 0;

   static struct option long_options[] = {
      { "capture", required_argument, NULL, 'c' },
      { "logfile", required_argument, NULL, 'l' },
      { "playback", required_argument, NULL, 'd' },
      { "help", no_argument, NULL, 'h' },
      { "llm-only", no_argument, NULL, 'L' },                 // LLM handles everything
      { "llm-commands", no_argument, NULL, 'C' },             // Commands first, then LLM
      { "commands-only", no_argument, NULL, 'D' },            // Direct commands only (explicit)
      { "network-audio", no_argument, NULL, 'N' },            // Start server for remote DAWN access
      { "llm", required_argument, NULL, 'm' },                // Set default LLM type (local/cloud)
      { "cloud-provider", required_argument, NULL, 'P' },     // Cloud provider (openai/claude)
      { "asr-engine", required_argument, NULL, 'A' },         // ASR engine (vosk/whisper)
      { "whisper-model", required_argument, NULL, 'W' },      // Whisper model (tiny/base/small)
      { "whisper-path", required_argument, NULL, 'w' },       // Whisper models directory
      { "music-dir", required_argument, NULL, 'M' },          // Music directory (absolute path)
      { "mic-record", optional_argument, NULL, 'r' },         // Record mic input for debugging
      { "asr-record", optional_argument, NULL, 'a' },         // Record ASR pre/post normalization
      { "no-bargein", no_argument, NULL, 'B' },               // Disable barge-in during TTS
      { "summarize-backend", required_argument, NULL, 256 },  // Search summarizer backend
      { "summarize-threshold", required_argument, NULL, 257 },  // Search summarizer threshold
      { "config", required_argument, NULL, 258 },               // Config file path
      { "dump-config", no_argument, NULL, 259 },                // Dump effective config and exit
      { "dump-settings", no_argument, NULL, 261 },              // Dump all settings with sources
      { "audio-backend", required_argument, NULL, 260 },        // Audio backend (auto/alsa/pulse)
#ifdef ENABLE_AEC
      { "aec-record", optional_argument, NULL, 'R' },  // Record AEC audio for debugging
#endif
#ifdef ENABLE_TUI
      { "tui", no_argument, NULL, 'T' },          // Enable TUI mode
      { "theme", required_argument, NULL, 't' },  // TUI theme (green/blue/bw)
#endif
      { 0, 0, 0, 0 }
   };
   int option_index = 0;
   const char *cloud_provider_override = NULL;
   llm_type_t llm_type_override = LLM_UNDEFINED;
   asr_engine_type_t asr_engine =
       ASR_ENGINE_WHISPER;              // Default: Whisper base (best performance + accuracy)
   const char *asr_model_path = NULL;   // Will be set based on engine
   const char *whisper_model = "base";  // Default Whisper model (tiny/base/small)
   const char *whisper_path =
       "../whisper.cpp/models";  // Default Whisper models directory (relative to build/)
   char whisper_full_path[512];  // Buffer to construct full model path

   // Search summarizer config (defaults)
   summarizer_config_t summarizer_config = {
      .backend = SUMMARIZER_BACKEND_DISABLED,
      .failure_policy = SUMMARIZER_ON_FAILURE_PASSTHROUGH,
      .threshold_bytes = SUMMARIZER_DEFAULT_THRESHOLD,
      .target_summary_words = SUMMARIZER_DEFAULT_TARGET_WORDS
   };

   // Config system variables
   const char *config_path = NULL;  // Explicit config file path from --config
   int dump_config = 0;             // --dump-config flag
   int dump_settings = 0;           // --dump-settings flag

   // CLI override flags - track which settings were set via CLI (takes priority over config file)
   // Using bitfield for efficiency: 4 bytes instead of 48+ bytes for individual ints
   enum {
      CLI_OVERRIDE_COMMAND_MODE = (1 << 0),
      CLI_OVERRIDE_WHISPER_MODEL = (1 << 1),
      CLI_OVERRIDE_WHISPER_PATH = (1 << 2),
      CLI_OVERRIDE_NETWORK_AUDIO = (1 << 3),
      CLI_OVERRIDE_LOG_FILE = (1 << 4),
      CLI_OVERRIDE_MIC_RECORD = (1 << 5),
      CLI_OVERRIDE_ASR_RECORD = (1 << 6),
      CLI_OVERRIDE_AEC_RECORD = (1 << 7),
      CLI_OVERRIDE_TUI = (1 << 8),
      CLI_OVERRIDE_AUDIO_BACKEND = (1 << 9),
      CLI_OVERRIDE_SUMMARIZER_BACKEND = (1 << 10),
      CLI_OVERRIDE_SUMMARIZER_THRESHOLD = (1 << 11),
      CLI_OVERRIDE_RECORD_PATH = (1 << 12),
   };
   uint32_t cli_overrides = 0;

   // Audio backend selection (default: auto-detect)
   audio_backend_type_t audio_backend_type = AUDIO_BACKEND_AUTO;

   // Construct default Whisper base model path
   snprintf(whisper_full_path, sizeof(whisper_full_path), "%s/ggml-%s.bin", whisper_path,
            whisper_model);
   asr_model_path = whisper_full_path;

   // Save argv early for potential restart via execve()
   dawn_save_argv(argc, argv);

   LOG_INFO("%s Version %s: %s\n", APP_NAME, VERSION_NUMBER, GIT_SHA);

   // Initialize curl globally and register cleanup handler
   curl_global_init(CURL_GLOBAL_DEFAULT);
   atexit(curl_global_cleanup);

#if defined(ENABLE_TUI) && defined(ENABLE_AEC)
   while ((opt = getopt_long(argc, argv, "c:d:hl:LCDNm:P:A:W:w:M:r::a::R::Tt:B", long_options,
                             &option_index)) != -1) {
#elif defined(ENABLE_TUI)
   while ((opt = getopt_long(argc, argv, "c:d:hl:LCDNm:P:A:W:w:M:r::a::Tt:B", long_options,
                             &option_index)) != -1) {
#elif defined(ENABLE_AEC)
   while ((opt = getopt_long(argc, argv, "c:d:hl:LCDNm:P:A:W:w:M:r::a::R::B", long_options,
                             &option_index)) != -1) {
#else
   while ((opt = getopt_long(argc, argv, "c:d:hl:LCDNm:P:A:W:w:M:r::a::B", long_options,
                             &option_index)) != -1) {
#endif
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
            cli_overrides |= CLI_OVERRIDE_LOG_FILE;
            break;
         case 'L':
            command_processing_mode = CMD_MODE_LLM_ONLY;
            cli_overrides |= CLI_OVERRIDE_COMMAND_MODE;
            LOG_INFO("LLM-only command processing enabled (CLI override)");
            break;
         case 'C':
            command_processing_mode = CMD_MODE_DIRECT_FIRST;
            cli_overrides |= CLI_OVERRIDE_COMMAND_MODE;
            LOG_INFO("Commands-first with LLM fallback enabled (CLI override)");
            break;
         case 'D':
            command_processing_mode = CMD_MODE_DIRECT_ONLY;
            cli_overrides |= CLI_OVERRIDE_COMMAND_MODE;
            LOG_INFO("Direct commands only mode enabled (CLI override)");
            break;
         case 'N':
            enable_network_audio = 1;
            cli_overrides |= CLI_OVERRIDE_NETWORK_AUDIO;
            LOG_INFO("Network audio enabled (CLI override)");
            break;
         case 'm':
            if (strcasecmp(optarg, "cloud") == 0) {
               llm_type_override = LLM_CLOUD;
               LOG_INFO("Using cloud LLM by default");
            } else if (strcasecmp(optarg, "local") == 0) {
               llm_type_override = LLM_LOCAL;
               LOG_INFO("Using local LLM by default");
            } else {
               LOG_ERROR("Unknown LLM type: %s. Using auto-detection.", optarg);
            }
            break;
         case 'P':
            cloud_provider_override = optarg;
            LOG_INFO("Cloud provider override: %s", cloud_provider_override);
            break;
         case 'W':
            whisper_model = optarg;
            cli_overrides |= CLI_OVERRIDE_WHISPER_MODEL;
            LOG_INFO("Whisper model set to: %s (CLI override)", whisper_model);
            break;
         case 'w':
            whisper_path = optarg;
            cli_overrides |= CLI_OVERRIDE_WHISPER_PATH;
            LOG_INFO("Whisper models path set to: %s (CLI override)", whisper_path);
            break;
         case 'A':
#ifdef ENABLE_VOSK
            if (strcmp(optarg, "vosk") == 0) {
               asr_engine = ASR_ENGINE_VOSK;
               asr_model_path = "../models/vosk-model";  // Relative to build/
               LOG_INFO("Using Vosk ASR engine");
            } else
#endif
                if (strcmp(optarg, "whisper") == 0) {
               asr_engine = ASR_ENGINE_WHISPER;
               // Construct full path: whisper_path/ggml-MODEL.bin
               snprintf(whisper_full_path, sizeof(whisper_full_path), "%s/ggml-%s.bin",
                        whisper_path, whisper_model);
               asr_model_path = whisper_full_path;
               LOG_INFO("Using Whisper ASR engine with model: %s", asr_model_path);
            } else {
#ifdef ENABLE_VOSK
               LOG_ERROR("Unknown ASR engine: %s. Using Whisper (default).", optarg);
#else
               LOG_ERROR("Unknown ASR engine: %s (Vosk support not compiled in). Using Whisper.",
                         optarg);
#endif
            }
            break;
         case 'M':
            set_music_directory(optarg);
            LOG_INFO("Music directory set to: %s", optarg);
            break;
#ifdef ENABLE_TUI
         case 'T':
            enable_tui = 1;
            cli_overrides |= CLI_OVERRIDE_TUI;
            LOG_INFO("TUI mode enabled via CLI");
            break;
         case 't':
            if (strcasecmp(optarg, "green") == 0) {
               tui_theme = TUI_THEME_GREEN;
            } else if (strcasecmp(optarg, "blue") == 0) {
               tui_theme = TUI_THEME_BLUE;
            } else if (strcasecmp(optarg, "bw") == 0) {
               tui_theme = TUI_THEME_BW;
            } else {
               LOG_WARNING("Unknown TUI theme: %s. Using green.", optarg);
               tui_theme = TUI_THEME_GREEN;
            }
            LOG_INFO("TUI theme set to: %s", optarg);
            break;
#endif
         case 'r':
            // Enable mic recording, optionally with a directory
            mic_enable_recording(true);
            cli_overrides |= CLI_OVERRIDE_MIC_RECORD;
            if (optarg) {
               mic_set_recording_dir(optarg);
               cli_overrides |= CLI_OVERRIDE_RECORD_PATH;
            }
            LOG_INFO("Mic recording enabled via CLI (dir: %s)", optarg ? optarg : "/tmp");
            break;
         case 'a':
            // Enable ASR recording, optionally with a directory
            // Recording starts/stops per-utterance in asr_reset()/asr_finalize()
            asr_enable_recording(true);
            cli_overrides |= CLI_OVERRIDE_ASR_RECORD;
            if (optarg) {
               asr_set_recording_dir(optarg);
               cli_overrides |= CLI_OVERRIDE_RECORD_PATH;
            }
            LOG_INFO("ASR recording enabled via CLI (dir: %s)", optarg ? optarg : "/tmp");
            break;
#ifdef ENABLE_AEC
         case 'R':
            // Enable AEC recording, optionally with a directory
            aec_enable_recording(true);
            cli_overrides |= CLI_OVERRIDE_AEC_RECORD;
            if (optarg) {
               aec_set_recording_dir(optarg);
               cli_overrides |= CLI_OVERRIDE_RECORD_PATH;
            }
            LOG_INFO("AEC recording enabled via CLI (dir: %s)", optarg ? optarg : "/tmp");
            break;
#endif
         case 'B':
            g_bargein_user_disabled = 1;
            LOG_INFO("Barge-in disabled: speech during TTS will be ignored");
            break;
         case 256:  // --summarize-backend
            summarizer_config.backend = search_summarizer_parse_backend(optarg);
            cli_overrides |= CLI_OVERRIDE_SUMMARIZER_BACKEND;
            LOG_INFO("Search summarization backend: %s (CLI override)",
                     search_summarizer_backend_name(summarizer_config.backend));
            break;
         case 257:  // --summarize-threshold
            summarizer_config.threshold_bytes = (size_t)atol(optarg);
            cli_overrides |= CLI_OVERRIDE_SUMMARIZER_THRESHOLD;
            LOG_INFO("Search summarization threshold: %zu bytes (CLI override)",
                     summarizer_config.threshold_bytes);
            break;
         case 258:  // --config
            config_path = optarg;
            break;
         case 259:  // --dump-config
            dump_config = 1;
            break;
         case 261:  // --dump-settings
            dump_settings = 1;
            break;
         case 260:  // --audio-backend
            audio_backend_type = audio_backend_parse_type(optarg);
            cli_overrides |= CLI_OVERRIDE_AUDIO_BACKEND;
            LOG_INFO("Audio backend set to: %s (CLI override)",
                     audio_backend_type_name(audio_backend_type));
            break;
         case '?':
            display_help(argc, argv);
            exit(EXIT_FAILURE);
         default:
            display_help(argc, argv);
            exit(EXIT_FAILURE);
      }
   }

   // =========================================================================
   // Configuration System Initialization
   // Priority: Defaults -> Config file -> Environment -> CLI args (later)
   // =========================================================================

   // Step 1: Initialize with compile-time defaults
   config_set_defaults(&g_config);
   config_set_secrets_defaults(&g_secrets);

   // Step 2: Load configuration from file (if available)
   // config_load_from_search handles both explicit path and search logic
   int config_result = config_load_from_search(config_path, &g_config);
   if (config_path && config_result != 0) {
      // Explicit path was given but failed to load - this is an error
      fprintf(stderr, "Error: Failed to load config file: %s\n", config_path);
      return 1;
   }
   // If config_result != 0 without explicit path, no config file found - using defaults (OK)
   (void)config_result;  // Suppress unused warning when no explicit path given

   // Step 3: Load secrets file (search default locations)
   config_load_secrets_from_search(&g_secrets);

   // Step 4: Apply environment variable overrides (highest priority before CLI)
   config_apply_env(&g_config, &g_secrets);

   // Step 4b: Apply config-based settings (CLI overrides take precedence)
   if (!(cli_overrides & CLI_OVERRIDE_COMMAND_MODE) &&
       g_config.commands.processing_mode[0] != '\0') {
      if (strcmp(g_config.commands.processing_mode, "llm_only") == 0) {
         command_processing_mode = CMD_MODE_LLM_ONLY;
      } else if (strcmp(g_config.commands.processing_mode, "direct_first") == 0) {
         command_processing_mode = CMD_MODE_DIRECT_FIRST;
      } else if (strcmp(g_config.commands.processing_mode, "direct_only") == 0) {
         command_processing_mode = CMD_MODE_DIRECT_ONLY;
      }
      /* Note: Invalid values would be caught by config_validate() above */
   }

   // Apply ASR config (CLI overrides take precedence)
   if (!(cli_overrides & CLI_OVERRIDE_WHISPER_MODEL) && g_config.asr.model[0] != '\0') {
      whisper_model = g_config.asr.model;
      LOG_INFO("Whisper model from config: %s", whisper_model);
   }
   if (!(cli_overrides & CLI_OVERRIDE_WHISPER_PATH) && g_config.asr.models_path[0] != '\0') {
      whisper_path = g_config.asr.models_path;
      LOG_INFO("Whisper models path from config: %s", whisper_path);
   }

   // Reconstruct Whisper path if either model or path came from config
   if (asr_engine == ASR_ENGINE_WHISPER) {
      snprintf(whisper_full_path, sizeof(whisper_full_path), "%s/ggml-%s.bin", whisper_path,
               whisper_model);
      asr_model_path = whisper_full_path;
   }

   // Apply network config (CLI overrides take precedence)
   if (!(cli_overrides & CLI_OVERRIDE_NETWORK_AUDIO) && g_config.network.enabled) {
      enable_network_audio = 1;
      LOG_INFO("Network audio enabled from config");
   }

#ifdef ENABLE_TUI
   // Apply TUI config (CLI overrides take precedence)
   if (!(cli_overrides & CLI_OVERRIDE_TUI) && g_config.tui.enabled) {
      enable_tui = 1;
      LOG_INFO("TUI mode enabled from config");
   }
#endif

   // Apply log file config (CLI overrides take precedence)
   if (!(cli_overrides & CLI_OVERRIDE_LOG_FILE) && g_config.general.log_file[0] != '\0') {
      log_filename = g_config.general.log_file;
      LOG_INFO("Log file from config: %s", log_filename);
   }

   // Apply debug recording config (CLI overrides take precedence)
   // Set recording directory first if configured (but not if CLI provided a path)
   if (!(cli_overrides & CLI_OVERRIDE_RECORD_PATH) && g_config.debug.record_path[0] != '\0') {
      mic_set_recording_dir(g_config.debug.record_path);
      asr_set_recording_dir(g_config.debug.record_path);
#ifdef ENABLE_AEC
      aec_set_recording_dir(g_config.debug.record_path);
#endif
      LOG_INFO("Debug recording path from config: %s", g_config.debug.record_path);
   }
   // Enable recordings from config if CLI didn't set them
   if (!(cli_overrides & CLI_OVERRIDE_MIC_RECORD) && g_config.debug.mic_record) {
      mic_enable_recording(true);
      LOG_INFO("Mic recording enabled from config");
   }
   if (!(cli_overrides & CLI_OVERRIDE_ASR_RECORD) && g_config.debug.asr_record) {
      asr_enable_recording(true);
      LOG_INFO("ASR recording enabled from config");
   }
#ifdef ENABLE_AEC
   if (!(cli_overrides & CLI_OVERRIDE_AEC_RECORD) && g_config.debug.aec_record) {
      aec_enable_recording(true);
      LOG_INFO("AEC recording enabled from config");
   }
#endif
   // Apply audio backend from config if CLI didn't set it
   if (!(cli_overrides & CLI_OVERRIDE_AUDIO_BACKEND) && g_config.audio.backend[0] != '\0') {
      audio_backend_type = audio_backend_parse_type(g_config.audio.backend);
      LOG_INFO("Audio backend from config: %s", audio_backend_type_name(audio_backend_type));
   }
   // Apply search summarizer settings from config if CLI didn't set them
   if (!(cli_overrides & CLI_OVERRIDE_SUMMARIZER_BACKEND) &&
       g_config.search.summarizer.backend[0] != '\0') {
      summarizer_config.backend = search_summarizer_parse_backend(
          g_config.search.summarizer.backend);
      LOG_INFO("Search summarizer backend from config: %s",
               search_summarizer_backend_name(summarizer_config.backend));
   }
   if (!(cli_overrides & CLI_OVERRIDE_SUMMARIZER_THRESHOLD) &&
       g_config.search.summarizer.threshold_bytes > 0) {
      summarizer_config.threshold_bytes = g_config.search.summarizer.threshold_bytes;
      LOG_INFO("Search summarizer threshold from config: %zu bytes",
               summarizer_config.threshold_bytes);
   }
   if (g_config.search.summarizer.target_words > 0) {
      summarizer_config.target_summary_words = g_config.search.summarizer.target_words;
      LOG_INFO("Search summarizer target words from config: %zu",
               summarizer_config.target_summary_words);
   }

   // Apply CLI LLM type override to g_config (needed for validation)
   if (llm_type_override == LLM_LOCAL) {
      strncpy(g_config.llm.type, "local", sizeof(g_config.llm.type) - 1);
      g_config.llm.type[sizeof(g_config.llm.type) - 1] = '\0';
      LOG_INFO("LLM type set to 'local' (CLI override)");
   } else if (llm_type_override == LLM_CLOUD) {
      strncpy(g_config.llm.type, "cloud", sizeof(g_config.llm.type) - 1);
      g_config.llm.type[sizeof(g_config.llm.type) - 1] = '\0';
      LOG_INFO("LLM type set to 'cloud' (CLI override)");
   }

   // Step 5: Handle --dump-config (print effective config and exit)
   if (dump_config) {
      config_dump(&g_config);
      return 0;
   }

   // Step 5b: Handle --dump-settings (print all settings with sources and exit)
   if (dump_settings) {
      config_dump_settings(&g_config, &g_secrets, config_get_loaded_path());
      return 0;
   }

   // Step 6: Validate configuration
   config_error_t config_errors[32];
   int error_count = config_validate(&g_config, &g_secrets, config_errors, 32);
   if (error_count > 0) {
      config_print_errors(config_errors, error_count);
      return 1;
   }

   // Step 6b: Check cloud LLM API key availability (graceful fallback to local)
   if (strcmp(g_config.llm.type, "cloud") == 0) {
      int has_api_key = 0;
      const char *provider = g_config.llm.cloud.provider;

      if (strcmp(provider, "openai") == 0) {
         has_api_key = llm_has_openai_key();
      } else if (strcmp(provider, "claude") == 0) {
         has_api_key = llm_has_claude_key();
      }

      if (!has_api_key) {
         LOG_WARNING("Cloud LLM provider '%s' requires API key in secrets.toml - falling back to "
                     "local LLM",
                     provider);
         strncpy(g_config.llm.type, "local", sizeof(g_config.llm.type) - 1);
         g_config.llm.type[sizeof(g_config.llm.type) - 1] = '\0';
         // Also update the runtime override so llm_init uses local
         llm_type_override = LLM_LOCAL;
      }
   }

   // Step 7: Initialize wake words from config (must be after config is finalized)
   init_wake_words();

   // Step 8: Initialize preroll buffer size from config
   // Buffer is statically allocated (VAD_PREROLL_MAX_BYTES), we just set the usable portion
   {
      int preroll_ms = g_config.vad.preroll_ms;
      if (preroll_ms <= 0) {
         preroll_ms = VAD_PREROLL_DEFAULT_MS;
      } else if (preroll_ms > VAD_PREROLL_MAX_MS) {
         fprintf(stderr, "Warning: vad.preroll_ms=%d exceeds max %d, clamping\n", preroll_ms,
                 VAD_PREROLL_MAX_MS);
         preroll_ms = VAD_PREROLL_MAX_MS;
      }
      // Calculate usable buffer size: 16kHz * 1 channel * 2 bytes * ms / 1000
      preroll_buffer_size = (size_t)(16000 * 1 * 2 * preroll_ms / 1000);
      // Static array is zero-initialized, no malloc needed
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

#ifdef ENABLE_TUI
   // Initialize TUI if enabled
   if (enable_tui) {
      if (tui_init(tui_theme) != 0) {
         LOG_WARNING("TUI initialization failed, falling back to console mode");
         enable_tui = 0;
      } else {
         LOG_INFO("TUI initialized with %s theme", tui_theme == TUI_THEME_GREEN  ? "green"
                                                   : tui_theme == TUI_THEME_BLUE ? "blue"
                                                   : tui_theme == TUI_THEME_BW   ? "black/white"
                                                                                 : "unknown");
         // Suppress console logging to avoid interfering with TUI
         logging_suppress_console(1);
      }
   }
#endif

   if (strcmp(pcm_capture_device, "") == 0) {
      strncpy(pcm_capture_device, g_config.audio.capture_device, sizeof(pcm_capture_device));
      pcm_capture_device[sizeof(pcm_capture_device) - 1] = '\0';
   }

   if (strcmp(pcm_playback_device, "") == 0) {
      strncpy(pcm_playback_device, g_config.audio.playback_device, sizeof(pcm_playback_device));
      pcm_playback_device[sizeof(pcm_playback_device) - 1] = '\0';
   }

   // Command Processing
   initActions(actions);

   LOG_INFO("Reading commands config file: %s", g_config.paths.commands_config);
   configFile = fopen(g_config.paths.commands_config, "r");
   if (configFile == NULL) {
      LOG_ERROR("Unable to open commands config file: %s\n", g_config.paths.commands_config);
      return 1;
   }

   if ((bytes_read = fread(buffer, 1, sizeof(buffer), configFile)) > 0) {
      buffer[bytes_read] = '\0';
   } else {
      LOG_ERROR("Failed to read commands config file (%s): %s\n", g_config.paths.commands_config,
                strerror(bytes_read));
      fclose(configFile);
      return 1;
   }

   fclose(configFile);
   LOG_INFO("Done.\n");

   if (parseCommandConfig(buffer, actions, &numActions, captureDevices, &numAudioCaptureDevices,
                          playbackDevices, &numAudioPlaybackDevices)) {
      LOG_ERROR("Error parsing json.\n");
      return 1;
   }

   LOG_INFO("\n");
   //printParsedData(actions, numActions);
   convertActionsToCommands(actions, &numActions, commands, &numCommands);
   LOG_INFO("Processed %d commands.", numCommands);
   //printCommands(commands, numCommands);

   // Initialize LLM system early - must happen before prompt is built
   // so llm_tools_enabled() returns correct value for native tool calling
   llm_init(cloud_provider_override);

   // Force early initialization of system instructions before any threads start.
   // This ensures thread-safe access since the buffer is built once and cached.
   (void)get_system_instructions();

   // Initialize session manager first (creates local session)
   // NOTE: accept_thread_start() will skip re-initialization since it's already done
   if (session_manager_init() != 0) {
      LOG_ERROR("Failed to initialize session manager");
      return 1;
   }

   // Initialize command router for worker thread request/response
   if (command_router_init() != 0) {
      LOG_ERROR("Failed to initialize command router");
      return 1;
   }

   session_t *local_session = session_get_local();
   if (!local_session) {
      LOG_ERROR("Failed to get local session");
      return 1;
   }

   // Set the appropriate system message content based on processing mode
   const char *system_prompt;
   if (command_processing_mode == CMD_MODE_LLM_ONLY ||
       command_processing_mode == CMD_MODE_DIRECT_FIRST) {
      // LLM modes get the enhanced prompt with command information
      system_prompt = get_local_command_prompt();
      LOG_INFO("Using enhanced system prompt for LLM command processing");
   } else {
      // Direct-only mode: use persona + system instructions for consistent behavior
      system_prompt = get_direct_mode_prompt();
      LOG_INFO("Using standard system prompt for direct command processing");
   }

   session_init_system_prompt(local_session, system_prompt);

   // Point global conversation_history to local session's history for compatibility
   // NOTE: This allows existing code to continue using the global pointer
   conversation_history = local_session->conversation_history;

   // Initialize audio backend (runtime selection between ALSA and PulseAudio)
   int audio_init_result = audio_backend_init(audio_backend_type);
   if (audio_init_result != AUDIO_SUCCESS) {
      LOG_ERROR("Failed to initialize audio backend: %s", audio_error_string(audio_init_result));
      return 1;
   }
   LOG_INFO("Audio backend initialized: %s", audio_backend_type_name(audio_backend_get_type()));

   // Start dedicated audio capture thread with ring buffer
   // Ring buffer size: 262144 bytes = ~8 seconds of audio at 16kHz mono 16-bit
   // Increased to prevent audio loss during Vosk processing which can take 100-500ms per iteration
   // Realtime priority: enabled (requires cap_sys_nice capability or root)
   LOG_INFO("Starting audio capture thread...");
   audio_capture_ctx = audio_capture_start(pcm_capture_device, 262144, 1);
   if (!audio_capture_ctx) {
      LOG_ERROR("Failed to start audio capture thread");
      return 1;
   }

   myAudioControls.full_buff_size = DEFAULT_FRAMES * DEFAULT_CHANNELS * sizeof(int16_t);

   LOG_INFO("max_buff_size: %u, full_buff_size: %u\n", max_buff_size,
            myAudioControls.full_buff_size);

   max_buff = (char *)malloc(max_buff_size);
   if (max_buff == NULL) {
      LOG_ERROR("malloc() failed on max_buff.\n");

      if (audio_capture_ctx) {
         audio_capture_stop(audio_capture_ctx);
         audio_capture_ctx = NULL;
      }

      return 1;
   }

   LOG_INFO("Init ASR: %s", asr_engine_name(asr_engine));
   // Initialize ASR engine (Vosk or Whisper)
   asr_context_t *asr_ctx = asr_init(asr_engine, asr_model_path, DEFAULT_RATE);
   if (asr_ctx == NULL) {
      LOG_ERROR("Error initializing ASR engine: %s\n", asr_engine_name(asr_engine));

      free(max_buff);

      if (audio_capture_ctx) {
         audio_capture_stop(audio_capture_ctx);
         audio_capture_ctx = NULL;
      }

      return 1;
   }

   // Initialize chunking manager (Whisper only, persistent lifecycle)
   // Only if chunking is enabled in config
   chunking_manager_t *chunk_mgr = NULL;
   if (asr_engine == ASR_ENGINE_WHISPER && g_config.vad.chunking.enabled) {
      chunk_mgr = chunking_manager_init(asr_ctx);
      if (!chunk_mgr) {
         LOG_ERROR("Failed to initialize chunking manager");
         asr_cleanup(asr_ctx);
         free(max_buff);
         if (audio_capture_ctx) {
            audio_capture_stop(audio_capture_ctx);
            audio_capture_ctx = NULL;
         }
         return 1;
      }
      LOG_INFO("Chunking enabled via config");
   } else if (asr_engine == ASR_ENGINE_WHISPER) {
      LOG_INFO("Chunking disabled via config");
   }

   /* SmartThings service initialization */
   st_error_t st_err = smartthings_init();
   if (st_err == ST_OK) {
      LOG_INFO("SmartThings service initialized");
   } else if (st_err != ST_ERR_NOT_CONFIGURED) {
      LOG_WARNING("SmartThings init failed: %s", smartthings_error_str(st_err));
   }

   /* MQTT Setup - conditionally enabled via config */
   if (g_config.mqtt.enabled) {
      LOG_INFO("Init mosquitto.");
      mosquitto_lib_init();

      mosq = mosquitto_new(NULL, true, NULL);
      if (mosq == NULL) {
         LOG_ERROR("Error: Out of memory.\n");
         return 1;
      }

      /* Configure callbacks. This should be done before connecting ideally. */
      mosquitto_connect_callback_set(mosq, on_connect);
      mosquitto_subscribe_callback_set(mosq, on_subscribe);
      mosquitto_message_callback_set(mosq, on_message);

      /* Set reconnect parameters (min delay, max delay, exponential backoff) */
      mosquitto_reconnect_delay_set(mosq, 2, 30, true);

      /* Set MQTT authentication if credentials are configured */
      if (g_secrets.mqtt_username[0] != '\0') {
         rc = mosquitto_username_pw_set(mosq, g_secrets.mqtt_username,
                                        g_secrets.mqtt_password[0] != '\0' ? g_secrets.mqtt_password
                                                                           : NULL);
         if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERROR("Failed to set MQTT credentials: %s", mosquitto_strerror(rc));
         } else {
            LOG_INFO("MQTT authentication configured for user: %s", g_secrets.mqtt_username);
         }
      } else {
         /* SECURITY WARNING: MQTT running without authentication */
         LOG_WARNING("========================================");
         LOG_WARNING("MQTT SECURITY WARNING: No authentication configured!");
         LOG_WARNING("Anyone on the network can send commands to DAWN.");
         LOG_WARNING("Configure mqtt_username/mqtt_password in secrets.toml");
         LOG_WARNING("========================================");
      }

      /* Connect to MQTT server (broker from config). */
      rc = mosquitto_connect(mosq, g_config.mqtt.broker, g_config.mqtt.port, 60);
      if (rc != MOSQ_ERR_SUCCESS) {
         mosquitto_destroy(mosq);
         LOG_ERROR("Error on mosquitto_connect(): %s\n", mosquitto_strerror(rc));
         return 1;
      } else {
         LOG_INFO("Connected to local MQTT server.\n");
      }

      /* Start processing MQTT events. */
      mosquitto_loop_start(mosq);

      /* Make MQTT available for command processing (WebUI, worker pool) */
      worker_pool_set_mosq(mosq);
   } else {
      LOG_INFO("MQTT disabled by config");
   }

   LOG_INFO("Init text to speech.");
   /* Initialize text to speech processing. */
   initialize_text_to_speech(pcm_playback_device);

#ifdef ENABLE_AEC
   // Initialize AEC (must be after TTS which creates the resampler)
   LOG_INFO("Init AEC for echo cancellation.");
   aec_config_t aec_config = aec_get_default_config();

   // Auto-detect platform for mobile mode
#ifdef PLATFORM_RPI
   aec_config.mobile_mode = true;
   LOG_INFO("AEC: Using mobile mode for Raspberry Pi");
#endif

   if (aec_init(&aec_config) != 0) {
      LOG_WARNING("AEC initialization failed - continuing without echo cancellation");
   }
#endif

   // Initialize Silero VAD
   LOG_INFO("Init Silero VAD for voice activity detection.");
   const char *home_dir = getenv("HOME");
   char vad_model_path[512];
   if (home_dir) {
      snprintf(vad_model_path, sizeof(vad_model_path),
               "%s/code/The-OASIS-Project/dawn/models/silero_vad_16k_op15.onnx", home_dir);
      vad_ctx = vad_silero_init(vad_model_path, NULL);  // Option B: separate OrtEnv
      if (!vad_ctx) {
         LOG_WARNING("Failed to initialize Silero VAD - proceeding without VAD");
      } else {
         LOG_INFO("Silero VAD initialized successfully (opset15 model, 0.311ms inference)");
      }
   } else {
      LOG_WARNING("HOME environment variable not set - VAD initialization skipped");
   }

   // Speak greeting with AEC delay calibration (uses boot greeting to measure acoustic delay)
   tts_speak_greeting_with_calibration(timeOfDayGreeting());

   // Wait for greeting to complete before enabling barge-in
   // This prevents VAD from interrupting the greeting during calibration
   LOG_INFO("Waiting for boot greeting to complete...");
   int tts_result = tts_wait_for_completion(10000);  // 10 second timeout
   if (tts_result == 0) {
      LOG_INFO("Boot greeting completed successfully");
   } else {
      LOG_WARNING("Boot greeting wait timed out - continuing anyway");
   }

   // Flush audio buffer to discard any speech captured during greeting
   if (audio_capture_ctx) {
      audio_capture_clear(audio_capture_ctx);
      LOG_INFO("Audio buffer cleared after boot greeting");
   }

   // Reset VAD state to clear any detections from greeting playback
   if (vad_ctx) {
      vad_silero_reset(vad_ctx);
      LOG_INFO("VAD state reset after boot greeting");
   }

   // Enable barge-in now that greeting is complete
   // Priority: CLI (--no-bargein) > config (audio.bargein.enabled)
   if (g_bargein_user_disabled) {
      g_bargein_disabled = 1;
      LOG_INFO("Barge-in disabled by CLI option");
   } else if (!g_config.audio.bargein.enabled) {
      g_bargein_disabled = 1;
      LOG_INFO("Barge-in disabled by config file");
   } else {
      g_bargein_disabled = 0;
      LOG_INFO("Barge-in enabled after boot greeting");
   }

   // Register the signal handler for SIGINT.
   if (signal(SIGINT, signal_handler) == SIG_ERR) {
      LOG_ERROR("Error: Unable to register signal handler.\n");
      exit(EXIT_FAILURE);
   }

   // Initialize metrics system (before LLM so config is tracked)
   if (metrics_init() != 0) {
      LOG_WARNING("Failed to initialize metrics system - continuing without metrics");
   }

   // Initialize search summarizer
   search_summarizer_init(&summarizer_config);

   // Set LLM type: CLI override > config file > auto-detect
   if (llm_type_override != LLM_UNDEFINED) {
      llm_set_type(llm_type_override);
   } else if (strcmp(g_config.llm.type, "cloud") == 0) {
      llm_set_type(LLM_CLOUD);
   } else if (strcmp(g_config.llm.type, "local") == 0) {
      llm_set_type(LLM_LOCAL);
   } else if (llm_check_connection("https://api.openai.com", 4)) {
      llm_set_type(LLM_CLOUD);
   } else {
      llm_set_type(LLM_LOCAL);
   }

   /* Determine effective worker count for ASR pool.
    * If both network and webui are enabled, use the larger of the two.
    * Worker pool is shared between network clients and WebUI. */
#ifdef ENABLE_WEBUI
   bool webui_needs_workers = g_config.webui.enabled;
#else
   bool webui_needs_workers = false;
#endif

   if (enable_network_audio || webui_needs_workers) {
      int effective_workers = 0;
      if (enable_network_audio && webui_needs_workers) {
         /* Both enabled: use max of the two */
         effective_workers = (g_config.network.workers > g_config.webui.workers)
                                 ? g_config.network.workers
                                 : g_config.webui.workers;
         LOG_INFO("Both network and WebUI enabled, using %d workers (max of network=%d, webui=%d)",
                  effective_workers, g_config.network.workers, g_config.webui.workers);
      } else if (enable_network_audio) {
         effective_workers = g_config.network.workers;
      } else {
         effective_workers = g_config.webui.workers;
      }

      /* Temporarily override network.workers since worker_pool_init reads from there */
      int saved_workers = g_config.network.workers;
      g_config.network.workers = effective_workers;

      if (enable_network_audio) {
         LOG_INFO("Initializing network audio system...");
         if (dawn_network_audio_init() != 0) {
            LOG_ERROR("Failed to initialize network audio system");
            enable_network_audio = 0;
         } else {
            LOG_INFO("Starting DAWN network server (multi-client worker pool with %d workers)...",
                     effective_workers);
            if (accept_thread_start(asr_engine, asr_model_path) != 0) {
               LOG_ERROR("Failed to start accept thread - network audio disabled");
               dawn_network_audio_cleanup();
               enable_network_audio = 0;
            } else {
               LOG_INFO("DAWN network server started successfully on port 5000");
               LOG_INFO("Network TTS will use existing Piper instance");
            }
         }
      }

      /* If network didn't start but WebUI needs workers, init pool now */
      if (!enable_network_audio && webui_needs_workers && !worker_pool_is_initialized()) {
         LOG_INFO("Initializing worker pool for WebUI audio (%d workers)...", effective_workers);
         if (worker_pool_init(asr_engine, asr_model_path) != 0) {
            LOG_WARNING("Failed to init worker pool - WebUI voice input disabled");
         }
      }

      g_config.network.workers = saved_workers;
   }

#ifdef ENABLE_WEBUI
   /* Initialize WebUI server if enabled */
   if (g_config.webui.enabled) {
      LOG_INFO("Initializing WebUI server...");
      if (webui_server_init(0, NULL) == WEBUI_SUCCESS) {
         LOG_INFO("WebUI server started on port %d", webui_server_get_port());
      } else {
         LOG_WARNING("Failed to start WebUI server - continuing without WebUI");
      }
   }
#endif

   // Main loop
   LOG_INFO("Listening...\n");
   while (!quit) {
#ifdef ENABLE_TUI
      // TUI update and input handling (runs at roughly 10-20 Hz based on loop timing)
      if (enable_tui) {
         tui_update();
         if (tui_handle_input()) {
            // User requested quit from TUI
            quit = 1;
            break;
         }
      }
#endif

      if (vision_ai_ready) {
         recState = DAWN_STATE_VISION_AI_READY;
         // Reset VAD state at interaction boundary (vision AI entry)
         if (vad_ctx) {
            vad_silero_reset(vad_ctx);
         }
      }

      // NOTE: Network audio handling removed - will be handled by worker threads (Phase 3/4)
      // Legacy network client support via dawn_server is disabled until worker pipeline is
      // complete.

      // Check if LLM thread has completed (non-blocking check)
      static int prev_llm_processing = 0;  // Track previous state
      if (prev_llm_processing == 1 && llm_processing == 0) {
         // LLM just completed - process the response
         LOG_INFO("LLM thread completed - processing response");

         // Record pipeline completion time (ASR to LLM response complete)
         struct timeval pipeline_end_time;
         gettimeofday(&pipeline_end_time, NULL);
         double pipeline_ms = (pipeline_end_time.tv_sec - pipeline_start_time.tv_sec) * 1000.0 +
                              (pipeline_end_time.tv_usec - pipeline_start_time.tv_usec) / 1000.0;
         metrics_record_pipeline_total(pipeline_ms);
         LOG_INFO("Pipeline time (ASR to LLM complete): %.1f ms", pipeline_ms);

         pthread_mutex_lock(&llm_mutex);
         char *response_text = llm_response_text;
         llm_response_text = NULL;  // Transfer ownership
         pthread_mutex_unlock(&llm_mutex);

         // Join the thread to clean up resources
         pthread_join(llm_thread, NULL);

         // Check if response was interrupted
         if (response_text == NULL && llm_is_interrupt_requested()) {
            LOG_INFO("LLM was interrupted - discarding partial response");
            llm_clear_interrupt();

            // Remove the user message from conversation history (last item)
            int history_len = json_object_array_length(conversation_history);
            if (history_len > 0) {
               json_object_array_del_idx(conversation_history, history_len - 1, 1);
            }
         } else if (response_text != NULL) {
            // Process successful response
            LOG_WARNING("AI: %s\n", response_text);

            // Update TUI with full LLM response (including commands for debugging)
            metrics_set_last_ai_response(response_text);

            // Create cleaned version for TTS (keep original for conversation history)
            char *tts_response = strdup(response_text);

            // Process any commands in the LLM response
            if (command_processing_mode == CMD_MODE_LLM_ONLY ||
                command_processing_mode == CMD_MODE_DIRECT_FIRST) {
               // Set command context so LLM callbacks use local session's config
               session_set_command_context(session_get_local());
               int cmds_processed = parse_llm_response_for_commands(response_text, mosq);
               session_set_command_context(NULL);  // Clear context
               if (cmds_processed > 0) {
                  LOG_INFO("Processed %d commands from LLM response", cmds_processed);
               }
               if (tts_response) {
                  // Remove command tags
                  char *cmd_start, *cmd_end;
                  while ((cmd_start = strstr(tts_response, "<command>")) != NULL) {
                     cmd_end = strstr(cmd_start, "</command>");
                     if (cmd_end) {
                        cmd_end += strlen("</command>");
                        memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
                     } else {
                        break;
                     }
                  }

                  // Remove <end_of_turn> tags (local AI models)
                  char *match = NULL;
                  if ((match = strstr(tts_response, "<end_of_turn>")) != NULL) {
                     *match = '\0';
                  }

                  // Remove special characters that cause problems
                  remove_chars(tts_response, "*");
                  remove_emojis(tts_response);

                  // Trim trailing whitespace
                  size_t len = strlen(tts_response);
                  while (len > 0 &&
                         (tts_response[len - 1] == ' ' || tts_response[len - 1] == '\t' ||
                          tts_response[len - 1] == '\n' || tts_response[len - 1] == '\r')) {
                     tts_response[--len] = '\0';
                  }
               }
            }

            // TTS already handled by streaming callback - no need to call text_to_speech here
            // Note: We don't touch TTS state here - let the state machine handle it
            // If user interrupts with wake word, state machine will discard TTS
            // If user interrupts without wake word, state machine will resume TTS

            // Save original response (with command tags) to conversation history
            // but trim trailing whitespace for Claude API compatibility
            char *history_response = strdup(response_text);
            if (history_response) {
               size_t len = strlen(history_response);
               while (len > 0 &&
                      (history_response[len - 1] == ' ' || history_response[len - 1] == '\t' ||
                       history_response[len - 1] == '\n' || history_response[len - 1] == '\r')) {
                  history_response[--len] = '\0';
               }
            }

            struct json_object *ai_message = json_object_new_object();
            json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
            json_object_object_add(ai_message, "content",
                                   json_object_new_string(history_response ? history_response
                                                                           : response_text));
            json_object_array_add(conversation_history, ai_message);

            free(response_text);
            free(tts_response);
            free(history_response);
         } else {
            // Response is NULL but not interrupted - error case
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_DISCARD;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            LOG_ERROR("LLM error - no response");
            text_to_speech("I'm sorry but I'm currently unavailable boss.");
         }
      }
      prev_llm_processing = llm_processing;  // Update previous state

      publish_ai_state(recState);
      switch (recState) {
         case DAWN_STATE_SILENCE:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_PLAY;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            /* Check for text input from any source (TUI, future REST, etc.) */
            if (check_and_process_input_queue(&command_text, &recState, &silenceNextState)) {
               break;
            }

            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            // Store audio in pre-roll buffer (circular buffer for speech padding)
            if (buff_size > 0) {
               size_t bytes_to_copy = buff_size;
               // Wrap around if necessary
               if (preroll_write_pos + bytes_to_copy > preroll_buffer_size) {
                  size_t first_chunk = preroll_buffer_size - preroll_write_pos;
                  size_t second_chunk = bytes_to_copy - first_chunk;
                  memcpy(preroll_buffer + preroll_write_pos, max_buff, first_chunk);
                  memcpy(preroll_buffer, max_buff + first_chunk, second_chunk);
                  preroll_write_pos = second_chunk;
               } else {
                  memcpy(preroll_buffer + preroll_write_pos, max_buff, bytes_to_copy);
                  preroll_write_pos += bytes_to_copy;
               }
               // Track valid bytes (up to buffer capacity)
               preroll_valid_bytes = (preroll_valid_bytes + bytes_to_copy > preroll_buffer_size)
                                         ? preroll_buffer_size
                                         : preroll_valid_bytes + bytes_to_copy;
            }

            // VAD-based wake word detection
            int speech_detected = 0;
            static int vad_debug_counter = 0;
            static int tts_vad_debounce = 0;  // Consecutive VAD detections during TTS
            static struct timespec
                tts_last_active;  // Timestamp when TTS was last active (monotonic)
            static struct timespec
                tts_started_at;  // Timestamp when TTS started playing (for startup cooldown)
            static int tts_was_playing = 0;  // Track TTS state transitions
            static int tts_timer_initialized = 0;
            static dawn_state_t prev_vad_state = DAWN_STATE_INVALID;  // Track state transitions

            // Music ducking state (reduces volume during speech detection)
            static int music_ducked = 0;                // Currently ducked?
            static float music_pre_duck_volume = 0.5f;  // Volume before ducking
            static struct timespec music_last_speech;   // Last speech detection time
            static int music_duck_initialized = 0;

            // Initialize timer on first run (ideally would be in main(), but keeping locality)
            if (!tts_timer_initialized) {
               clock_gettime(CLOCK_MONOTONIC, &tts_last_active);
               tts_last_active.tv_sec -= 10;      // Start 10s in the past (no cooldown at startup)
               tts_started_at = tts_last_active;  // Also initialize startup timer
               tts_timer_initialized = 1;
            }

            // Initialize music ducking timer
            if (!music_duck_initialized) {
               clock_gettime(CLOCK_MONOTONIC, &music_last_speech);
               music_last_speech.tv_sec -= 10;  // Start in the past (no ducking at startup)
               music_duck_initialized = 1;
            }

            // Reset debounce counter on state entry (prevents stale state from previous TTS)
            if (prev_vad_state != DAWN_STATE_SILENCE) {
               tts_vad_debounce = 0;
            }
            prev_vad_state = DAWN_STATE_SILENCE;

            if (vad_ctx && buff_size >= VAD_SAMPLE_SIZE * sizeof(int16_t)) {
               // Process through VAD (requires 512 samples = 32ms at 16kHz)
               const int16_t *samples = (const int16_t *)max_buff;
               vad_speech_prob = vad_silero_process(vad_ctx, samples, VAD_SAMPLE_SIZE);

               // Debug logging every 50 iterations (~5 seconds)
               if (vad_debug_counter++ % 50 == 0) {
                  LOG_INFO("SILENCE: VAD=%.3f", vad_speech_prob);
               }

#ifdef ENABLE_AEC
               // AEC statistics logging every 600 iterations (~60 seconds)
               static unsigned int aec_stats_counter = 0;
               if (++aec_stats_counter >= 600) {
                  aec_stats_counter = 0;
                  aec_stats_t stats;
                  if (aec_get_stats(&stats) == 0 && stats.is_active) {
                     LOG_INFO("AEC Stats: delay=%dms, processed=%llu, passed=%llu, errors=%d, "
                              "avg_time=%.1fus",
                              stats.estimated_delay_ms, (unsigned long long)stats.frames_processed,
                              (unsigned long long)stats.frames_passed_through,
                              stats.consecutive_errors, stats.avg_processing_time_us);
                  }
               }
#endif

               if (vad_speech_prob < 0.0f) {
                  LOG_ERROR("SILENCE: VAD processing failed - assuming silence");
                  speech_detected = 0;  // Assume silence on error
                  tts_vad_debounce = 0;
               } else {
                  // Check if TTS is currently playing or paused (mutex-protected read)
                  pthread_mutex_lock(&tts_mutex);
                  int tts_playing_now = (tts_playback_state == TTS_PLAYBACK_PLAY ||
                                         tts_playback_state == TTS_PLAYBACK_PAUSE);
                  pthread_mutex_unlock(&tts_mutex);

                  // Track TTS start transition for startup cooldown
                  struct timespec now;
                  clock_gettime(CLOCK_MONOTONIC, &now);

                  if (tts_playing_now && !tts_was_playing) {
                     // TTS just started - record start time for startup cooldown
                     tts_started_at = now;
                  }
                  tts_was_playing = tts_playing_now;

                  // Update last active timestamp if TTS is playing (monotonic clock)
                  if (tts_playing_now) {
                     tts_last_active = now;
                  }

                  // Calculate time since TTS was last active (for post-TTS cooldown)
                  long elapsed_ms = (now.tv_sec - tts_last_active.tv_sec) * 1000 +
                                    (now.tv_nsec - tts_last_active.tv_nsec) / 1000000;

                  // Calculate time since TTS started (for startup cooldown)
                  long startup_elapsed_ms = (now.tv_sec - tts_started_at.tv_sec) * 1000 +
                                            (now.tv_nsec - tts_started_at.tv_nsec) / 1000000;

                  // TTS-aware VAD: use higher threshold if TTS playing OR within cooldown
                  int tts_is_active = tts_playing_now ||
                                      (elapsed_ms < g_config.audio.bargein.cooldown_ms);
                  float threshold = tts_is_active ? g_config.vad.speech_threshold_tts
                                                  : g_config.vad.speech_threshold;

                  // ERLE-based VAD gating: when AEC is struggling, be more conservative
                  float erle_db = 0.0f;
                  bool erle_valid = false;
#ifdef ENABLE_AEC
                  if (tts_is_active) {
                     erle_valid = aec_get_erle(&erle_db);
                     // TODO: ERLE gating disabled for debugging - AEC3 not converging
                     // When ERLE works (>6dB), re-enable this block
                     if (false && erle_valid && erle_db < 6.0f) {
                        // Poor echo cancellation - reject VAD entirely during TTS
                        // This prevents false triggers from residual echo
                        if (vad_speech_prob >= threshold) {
                           LOG_INFO("ERLE gating: rejecting VAD=%.3f (ERLE=%.1fdB < 6dB)",
                                    vad_speech_prob, erle_db);
                        }
                        tts_vad_debounce = 0;
                        continue;  // Skip this VAD frame
                     }
                  }
#endif

                  if (vad_speech_prob >= threshold) {
                     // If barge-in is disabled (boot phase or CLI option), ignore all speech
                     if (g_bargein_disabled) {
                        tts_vad_debounce = 0;
                     } else if (tts_playing_now &&
                                startup_elapsed_ms < g_config.audio.bargein.startup_cooldown_ms) {
                        // During TTS startup cooldown: AEC needs time to converge, block barge-in
                        tts_vad_debounce = 0;  // Reset debounce during startup
                     } else if (tts_is_active) {
                        // During TTS (or cooldown): require consecutive detections (debounce)
                        tts_vad_debounce++;
#ifdef ENABLE_AEC
                        LOG_INFO("TTS_VAD: prob=%.3f debounce=%d/%d ERLE=%.1fdB tts_playing=%d",
                                 vad_speech_prob, tts_vad_debounce, VAD_TTS_DEBOUNCE_COUNT, erle_db,
                                 tts_playing_now);
#else
                        LOG_INFO("TTS_VAD: prob=%.3f debounce=%d/%d tts_playing=%d",
                                 vad_speech_prob, tts_vad_debounce, VAD_TTS_DEBOUNCE_COUNT,
                                 tts_playing_now);
#endif
                        if (tts_vad_debounce >= VAD_TTS_DEBOUNCE_COUNT) {
                           speech_detected = 1;
#ifdef ENABLE_AEC
                           LOG_INFO("SILENCE: TTS barge-in confirmed (debounce=%d, VAD=%.3f, "
                                    "ERLE=%.1fdB, startup=%ldms)",
                                    tts_vad_debounce, vad_speech_prob, erle_db, startup_elapsed_ms);
#else
                           LOG_INFO("SILENCE: TTS barge-in confirmed (debounce=%d, VAD=%.3f, "
                                    "startup=%ldms)",
                                    tts_vad_debounce, vad_speech_prob, startup_elapsed_ms);
#endif
                           metrics_record_bargein();
                        }
                     } else {
                        // No TTS: immediate detection
                        speech_detected = 1;
                     }
                  } else {
                     // Below threshold - reset debounce counter
                     if (tts_is_active && tts_vad_debounce > 0) {
                        LOG_INFO("TTS_VAD: prob=%.3f RESET (was %d)", vad_speech_prob,
                                 tts_vad_debounce);
                     }
                     tts_vad_debounce = 0;
                  }
               }
            } else {
               // VAD not available - log error
               if (vad_debug_counter++ % 50 == 0) {
                  LOG_ERROR("SILENCE: VAD unavailable - vad_ctx=%p, buff_size=%u, need=%zu",
                            vad_ctx, buff_size, VAD_SAMPLE_SIZE * sizeof(int16_t));
               }
               speech_detected = 0;  // Assume silence if VAD unavailable
            }

            // Music ducking: reduce volume when speech detected, restore after cooldown
            if (speech_detected && getMusicPlay()) {
               clock_gettime(CLOCK_MONOTONIC, &music_last_speech);
               if (!music_ducked) {
                  // Duck the music - save current volume and reduce
                  music_pre_duck_volume = getMusicVolume();
                  float ducked_volume = music_pre_duck_volume * MUSIC_DUCK_FACTOR;
                  setMusicVolume(ducked_volume);
                  music_ducked = 1;
                  LOG_INFO("Music ducked: %.2f -> %.2f (speech detected)", music_pre_duck_volume,
                           ducked_volume);
               }
            } else if (music_ducked && getMusicPlay()) {
               // Check if cooldown has elapsed since last speech
               struct timespec now;
               clock_gettime(CLOCK_MONOTONIC, &now);
               double elapsed = (now.tv_sec - music_last_speech.tv_sec) +
                                (now.tv_nsec - music_last_speech.tv_nsec) / 1e9;
               if (elapsed >= MUSIC_DUCK_RESTORE_DELAY) {
                  // Restore original volume
                  setMusicVolume(music_pre_duck_volume);
                  LOG_INFO("Music restored: %.2f (%.1fs since speech)", music_pre_duck_volume,
                           elapsed);
                  music_ducked = 0;
               }
            } else if (music_ducked && !getMusicPlay()) {
               // Music stopped - reset ducking state
               music_ducked = 0;
            }

            if (speech_detected) {
               LOG_INFO("SILENCE: Speech detected (VAD: %.3f), transitioning to %s (pre-roll: %zu "
                        "bytes)\n",
                        vad_speech_prob, dawn_state_name(silenceNextState), preroll_valid_bytes);
               recState = silenceNextState;

               // Reset VAD state for new interaction (critical for preventing state accumulation)
               if (vad_ctx) {
                  vad_silero_reset(vad_ctx);
               }
               silence_count = 0;
               silence_duration = 0.0f;
               speech_duration = 0.0f;
               recording_duration = 0.0f;

               // Feed pre-roll buffer to ASR first (audio before VAD trigger)
               // Note: Pre-roll buffer will be reset AFTER feeding to ASR (line 2011-2013)
               // Pre-roll buffer is circular, so read from correct position
               if (preroll_valid_bytes > 0) {
                  // Defensive assertions for buffer invariants
                  assert(preroll_write_pos < preroll_buffer_size);
                  assert(preroll_valid_bytes <= preroll_buffer_size);

                  if (preroll_valid_bytes < preroll_buffer_size) {
                     // Buffer not yet full, read from beginning
                     asr_result = asr_process_partial(asr_ctx, (const int16_t *)preroll_buffer,
                                                      preroll_valid_bytes / sizeof(int16_t));
                  } else {
                     // Buffer full, read from write_pos to end, then beginning to write_pos
                     size_t first_chunk_bytes = preroll_buffer_size - preroll_write_pos;
                     asr_result = asr_process_partial(
                         asr_ctx, (const int16_t *)(preroll_buffer + preroll_write_pos),
                         first_chunk_bytes / sizeof(int16_t));
                     if (asr_result != NULL) {
                        asr_result_free(asr_result);
                        asr_result = NULL;
                     }
                     asr_result = asr_process_partial(asr_ctx, (const int16_t *)preroll_buffer,
                                                      preroll_write_pos / sizeof(int16_t));
                  }
                  if (asr_result != NULL) {
                     asr_result_free(asr_result);
                     asr_result = NULL;
                  }
               }

               // Then feed current chunk
               asr_result = asr_process_partial(asr_ctx, (const int16_t *)max_buff,
                                                buff_size / sizeof(int16_t));
               if (asr_result == NULL) {
                  LOG_ERROR("asr_process_partial() returned NULL!\n");
               } else {
                  asr_result_free(asr_result);
                  asr_result = NULL;
               }

               // Reset pre-roll buffer for next detection
               preroll_write_pos = 0;
               preroll_valid_bytes = 0;
            }
            break;
         case DAWN_STATE_WAKEWORD_LISTEN:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PLAY) {
               tts_playback_state = TTS_PLAYBACK_PAUSE;
            }
            pthread_mutex_unlock(&tts_mutex);

            /* Check for text input from any source (TUI, future REST, etc.) */
            if (check_and_process_input_queue(&command_text, &recState, &silenceNextState)) {
               break;
            }

            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            // VAD-based speech end detection
            int is_silence = 0;
            static int ww_vad_debug_counter = 0;
            if (vad_ctx && buff_size >= VAD_SAMPLE_SIZE * sizeof(int16_t)) {
               // Process through VAD
               vad_speech_prob = vad_silero_process(vad_ctx, (const int16_t *)max_buff,
                                                    VAD_SAMPLE_SIZE);

               if (ww_vad_debug_counter++ % 50 == 0) {
                  LOG_INFO("WAKEWORD_LISTEN: VAD=%.3f", vad_speech_prob);
               }

               if (vad_speech_prob < 0.0f) {
                  LOG_ERROR("WAKEWORD_LISTEN: VAD processing failed - assuming silence");
                  is_silence = 1;  // Assume silence on error
               } else {
                  is_silence = (vad_speech_prob < g_config.vad.silence_threshold);

                  // ENGINE-AWARE AUDIO ROUTING (chunk for Whisper, stream for Vosk)
                  if (!is_silence) {
                     if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
                        // Whisper: Feed to chunking manager
                        int result = chunking_manager_add_audio(chunk_mgr,
                                                                (const int16_t *)max_buff,
                                                                buff_size / sizeof(int16_t));
                        if (result != 0) {
                           LOG_ERROR("WAKEWORD_LISTEN: chunking_manager_add_audio() failed");
                        }
                     } else if (asr_engine == ASR_ENGINE_VOSK) {
                        // Vosk: Direct streaming path
                        asr_result = asr_process_partial(asr_ctx, (const int16_t *)max_buff,
                                                         buff_size / sizeof(int16_t));
                        if (asr_result == NULL) {
                           LOG_ERROR("asr_process_partial() returned NULL!\n");
                        } else {
                           // Record what ASR is hearing for TUI display
                           if (asr_result->text && strlen(asr_result->text) > 0) {
                              metrics_set_last_asr_text(asr_result->text, 0);
                           }
                           asr_result_free(asr_result);
                           asr_result = NULL;
                        }
                     }
                  }
               }
            } else {
               // VAD not available - log error
               if (ww_vad_debug_counter++ % 50 == 0) {
                  LOG_ERROR("WAKEWORD_LISTEN: VAD unavailable - vad_ctx=%p, buff_size=%u, need=%zu",
                            vad_ctx, buff_size, VAD_SAMPLE_SIZE * sizeof(int16_t));
               }
               is_silence = 1;  // Assume silence if VAD unavailable
            }

            // Track silence and speech duration for speech end detection and chunking
            if (is_silence) {
               silence_duration += DEFAULT_CAPTURE_SECONDS;
            } else {
               speech_duration += DEFAULT_CAPTURE_SECONDS;
               silence_duration = 0.0f;  // Reset on speech
            }

            // Pause detection for chunking (Whisper only, similar to DAWN_STATE_COMMAND_RECORDING)
            if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
               int should_finalize_chunk = 0;

               // Detect natural pauses (silence after sufficient speech)
               if (is_silence && speech_duration >= g_config.vad.chunking.min_duration &&
                   silence_duration >= g_config.vad.chunking.pause_duration) {
                  LOG_INFO("WAKEWORD_LISTEN: Pause detected (%.1fs) after %.1fs speech - "
                           "finalizing chunk",
                           silence_duration, speech_duration);
                  should_finalize_chunk = 1;
               }

               // Force chunk on max duration
               if (speech_duration >= g_config.vad.chunking.max_duration) {
                  LOG_INFO("WAKEWORD_LISTEN: Max chunk duration reached (%.1fs) - forcing chunk",
                           speech_duration);
                  should_finalize_chunk = 1;
               }

               // Finalize chunk if triggered
               if (should_finalize_chunk && !chunking_manager_is_finalizing(chunk_mgr)) {
                  char *chunk_text = NULL;
                  int result = chunking_manager_finalize_chunk(chunk_mgr, &chunk_text);

                  if (result == 0) {
                     if (chunk_text) {
                        LOG_INFO("WAKEWORD_LISTEN: Chunk %zu finalized: \"%s\"",
                                 chunking_manager_get_num_chunks(chunk_mgr) - 1, chunk_text);
                        // Update TUI with what was heard
                        metrics_set_last_asr_text(chunk_text, 0);
                        free(chunk_text);
                     }
                     // Reset speech duration after successful chunk
                     speech_duration = 0.0f;
                  } else {
                     LOG_ERROR("WAKEWORD_LISTEN: Chunk finalization failed");
                  }
               }
            }

            // Track total recording duration (prevents buffer overflow from background
            // conversation)
            recording_duration += DEFAULT_CAPTURE_SECONDS;

            // Flag to trigger wake word processing after finalization
            int should_check_wake_word = 0;

            // Check for maximum recording duration (safety limit for buffer)
            if (recording_duration >= g_config.vad.max_recording_duration) {
               LOG_WARNING("WAKEWORD_LISTEN: Max recording duration reached (%.1fs), forcing "
                           "finalization to prevent buffer overflow.\n",
                           recording_duration);
               recording_duration = 0.0f;
               silence_duration = 0.0f;
               speech_duration = 0.0f;
               // Reset pre-roll buffer
               preroll_write_pos = 0;
               preroll_valid_bytes = 0;

               // ENGINE-AWARE FINALIZATION
               if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
                  // Whisper: Finalize any pending audio then get concatenated text
                  if (chunking_manager_get_buffer_usage(chunk_mgr) > 0) {
                     char *pending_chunk = NULL;
                     LOG_INFO("WAKEWORD_LISTEN: Finalizing pending audio at max duration");
                     int result = chunking_manager_finalize_chunk(chunk_mgr, &pending_chunk);
                     if (result == 0 && pending_chunk) {
                        LOG_INFO("WAKEWORD_LISTEN: Pending chunk finalized: \"%s\"", pending_chunk);
                        free(pending_chunk);
                     }
                  }

                  size_t num_chunks = chunking_manager_get_num_chunks(chunk_mgr);
                  input_text = chunking_manager_get_full_text(chunk_mgr);
                  if (input_text) {
                     LOG_WARNING("Input (from %zu chunks): %s\n", num_chunks, input_text);
                  } else {
                     LOG_WARNING("Input: (no chunks at max duration timeout)\n");
                  }
                  should_check_wake_word =
                      1;  // Set flag even with NULL input to trigger state transition
               } else {
                  // Vosk: Direct finalization
                  asr_result = asr_finalize(asr_ctx);
                  if (asr_result && asr_result->text) {
                     input_text = strdup(asr_result->text);
                     LOG_WARNING("Input: %s\n", input_text);
                  } else {
                     LOG_WARNING("Input: (Vosk finalize returned NULL or empty at max duration)\n");
                  }
                  if (asr_result) {
                     asr_result_free(asr_result);
                     asr_result = NULL;
                  }
                  should_check_wake_word =
                      1;  // Set flag even with NULL input to trigger state transition
               }
            }
            // Check if speech has ended (silence threshold)
            else if (silence_duration >= g_config.vad.end_of_speech_duration) {
               silence_duration = 0.0f;
               speech_duration = 0.0f;
               recording_duration = 0.0f;
               // Reset pre-roll buffer
               preroll_write_pos = 0;
               preroll_valid_bytes = 0;
               LOG_WARNING(
                   "WAKEWORD_LISTEN: Speech ended (%.1fs silence), checking for wake word.\n",
                   g_config.vad.end_of_speech_duration);

               // ENGINE-AWARE FINALIZATION
               if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
                  // Whisper: Finalize any pending audio then get concatenated text
                  // This ensures short utterances (< VAD_MIN_CHUNK_DURATION) are transcribed
                  if (chunking_manager_get_buffer_usage(chunk_mgr) > 0) {
                     char *pending_chunk = NULL;
                     LOG_INFO(
                         "WAKEWORD_LISTEN: Finalizing pending audio buffer before get_full_text");
                     int result = chunking_manager_finalize_chunk(chunk_mgr, &pending_chunk);
                     if (result == 0 && pending_chunk) {
                        LOG_INFO("WAKEWORD_LISTEN: Pending chunk finalized: \"%s\"", pending_chunk);
                        free(pending_chunk);
                     }
                  }

                  size_t num_chunks = chunking_manager_get_num_chunks(chunk_mgr);
                  input_text = chunking_manager_get_full_text(chunk_mgr);
                  if (input_text) {
                     LOG_WARNING("Input (from %zu chunks): %s\n", num_chunks, input_text);
                  } else {
                     LOG_WARNING("Input: (no chunks finalized)\n");
                  }
                  should_check_wake_word =
                      1;  // Set flag even with NULL input to trigger state transition
               } else {
                  // Vosk: Direct finalization
                  asr_result = asr_finalize(asr_ctx);
                  if (asr_result == NULL) {
                     LOG_ERROR("asr_finalize() returned NULL!\n");
                  } else {
                     LOG_WARNING("Input: %s\n", asr_result->text ? asr_result->text : "");
                     if (asr_result->text) {
                        input_text = strdup(asr_result->text);
                        metrics_set_last_user_command(asr_result->text);
                     } else {
                        input_text = NULL;
                     }
                     asr_result_free(asr_result);
                     asr_result = NULL;
                  }
                  should_check_wake_word =
                      1;  // Set flag even with NULL input to trigger state transition
               }
            }

            // WAKE WORD PROCESSING (shared by max duration and speech end paths)
            if (should_check_wake_word) {
               if (input_text) {
                  // Normalize for wake word/command matching
                  char *normalized_text = normalize_for_matching(input_text);

                  for (i = 0; i < numGoodbyeWords; i++) {
                     if (normalized_text && strcmp(normalized_text, goodbyeWords[i]) == 0) {
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
                        if (normalized_text && strcmp(normalized_text, cancelWords[i]) == 0) {
                           LOG_WARNING("Cancel word detected.\n");

                           tts_playback_state = TTS_PLAYBACK_DISCARD;
                           pthread_cond_signal(&tts_cond);

                           silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;
                           recState = DAWN_STATE_SILENCE;
                           // Reset all subsystems for new utterance
                           reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                                   &speech_duration, &recording_duration,
                                                   &preroll_write_pos, &preroll_valid_bytes);
                        }
                     }
                  }
                  pthread_mutex_unlock(&tts_mutex);

                  // Search for wake word in normalized text
                  for (i = 0; i < numWakeWords; i++) {
                     char *found_ptr = normalized_text ? strstr(normalized_text, wakeWords[i])
                                                       : NULL;
                     if (found_ptr != NULL) {
                        LOG_WARNING("Wake word detected.\n");

                        // Check if LLM is currently processing - if so, interrupt it
                        if (llm_processing) {
                           LOG_INFO(
                               "Wake word detected during LLM processing - requesting interrupt");
                           llm_request_interrupt();
                        }

                        // Calculate offset in normalized text
                        size_t offset = found_ptr - normalized_text;
                        size_t wakeWordLength = strlen(wakeWords[i]);

                        // Find corresponding position in original text
                        // (accounting for removed punctuation)
                        size_t orig_offset = 0;
                        size_t norm_offset = 0;
                        while (norm_offset < offset && input_text[orig_offset] != '\0') {
                           char c = input_text[orig_offset];
                           if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == ' ') {
                              norm_offset++;
                           }
                           orig_offset++;
                        }

                        // Advance past wake word in original text
                        while (norm_offset < offset + wakeWordLength &&
                               input_text[orig_offset] != '\0') {
                           char c = input_text[orig_offset];
                           if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == ' ') {
                              norm_offset++;
                           }
                           orig_offset++;
                        }

                        next_char_ptr = input_text + orig_offset;
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

                     // Check if there's any meaningful text after wake word
                     // Skip whitespace and punctuation to see if there's actual command text
                     const char *check_ptr = next_char_ptr;
                     LOG_INFO("Wake word found. next_char_ptr='%s'",
                              next_char_ptr ? next_char_ptr : "(null)");
                     while (*check_ptr != '\0' &&
                            (*check_ptr == ' ' || *check_ptr == '.' || *check_ptr == ',' ||
                             *check_ptr == '!' || *check_ptr == '?')) {
                        check_ptr++;
                     }
                     LOG_INFO("After skip, check_ptr='%s' (len=%zu)", check_ptr, strlen(check_ptr));

                     if (*check_ptr == '\0') {
                        // No command after wake word - transition to DAWN_STATE_COMMAND_RECORDING
                        LOG_WARNING("Wake word detected with no command, transitioning to "
                                    "COMMAND_RECORDING.\n");
                        text_to_speech("Hello sir.");

                        commandTimeout = 0;
                        silenceNextState = DAWN_STATE_COMMAND_RECORDING;
                        recState = DAWN_STATE_SILENCE;

                        // Reset all subsystems for new utterance
                        reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                                &speech_duration, &recording_duration,
                                                &preroll_write_pos, &preroll_valid_bytes);
                     } else {
                        // Skip leading whitespace and punctuation to get actual command text
                        const char *cmd_start = next_char_ptr;
                        while (*cmd_start != '\0' &&
                               (*cmd_start == ' ' || *cmd_start == '.' || *cmd_start == ',' ||
                                *cmd_start == '!' || *cmd_start == '?')) {
                           cmd_start++;
                        }
                        command_text = strdup(cmd_start);
                        free(input_text);
                        input_text = NULL;

                        // Log the user command for TUI activity feed
                        if (command_text) {
                           metrics_set_last_user_command(command_text);
                        }

                        recState = DAWN_STATE_PROCESS_COMMAND;

                        // Reset all subsystems for new utterance
                        reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                                &speech_duration, &recording_duration,
                                                &preroll_write_pos, &preroll_valid_bytes);
                     }
                  } else {
                     pthread_mutex_lock(&tts_mutex);
                     if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                        tts_playback_state = TTS_PLAYBACK_PLAY;
                        pthread_cond_signal(&tts_cond);
                     }
                     pthread_mutex_unlock(&tts_mutex);

                     silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;
                     recState = DAWN_STATE_SILENCE;

                     // Reset all subsystems for new utterance (no wake word detected)
                     reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                             &speech_duration, &recording_duration,
                                             &preroll_write_pos, &preroll_valid_bytes);
                  }

                  // Cleanup normalized text
                  if (normalized_text) {
                     free(normalized_text);
                  }

                  // Cleanup input text
                  if (input_text) {
                     free(input_text);
                     input_text = NULL;
                  }
               } else {
                  // No content to process, transition back to DAWN_STATE_SILENCE to avoid infinite
                  // loop
                  LOG_INFO(
                      "WAKEWORD_LISTEN: No content to process, returning to DAWN_STATE_SILENCE.\n");
                  silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;
                  recState = DAWN_STATE_SILENCE;
                  // Reset all subsystems for new utterance
                  reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                          &speech_duration, &recording_duration, &preroll_write_pos,
                                          &preroll_valid_bytes);
               }
            }
            buff_size = 0;
            break;
         case DAWN_STATE_COMMAND_RECORDING:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_DISCARD;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            capture_buffer(&myAudioControls, max_buff, max_buff_size, &buff_size);

            // Use VAD for speech end detection in command recording
            int cmd_speech_detected = 0;
            static int cmd_vad_debug_counter = 0;
            if (vad_ctx && buff_size >= VAD_SAMPLE_SIZE * sizeof(int16_t)) {
               vad_speech_prob = vad_silero_process(vad_ctx, (const int16_t *)max_buff,
                                                    VAD_SAMPLE_SIZE);

               if (cmd_vad_debug_counter++ % 50 == 0) {
                  LOG_INFO("COMMAND_RECORDING: VAD=%.3f", vad_speech_prob);
               }

               if (vad_speech_prob < 0.0f) {
                  LOG_ERROR("COMMAND_RECORDING: VAD processing failed - assuming silence");
                  cmd_speech_detected = 0;
               } else {
                  cmd_speech_detected = (vad_speech_prob >= g_config.vad.speech_threshold);
               }
            } else {
               if (cmd_vad_debug_counter++ % 50 == 0) {
                  LOG_ERROR(
                      "COMMAND_RECORDING: VAD unavailable - vad_ctx=%p, buff_size=%u, need=%zu",
                      vad_ctx, buff_size, VAD_SAMPLE_SIZE * sizeof(int16_t));
               }
               cmd_speech_detected = 0;
            }

            // ENGINE-AWARE AUDIO ROUTING (Architecture Review Issues #1, #2, #3)
            if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
               // Whisper mode: Feed audio to chunking manager (which mediates ASR)
               int result = chunking_manager_add_audio(chunk_mgr, (const int16_t *)max_buff,
                                                       buff_size / sizeof(int16_t));
               if (result != 0) {
                  LOG_ERROR("COMMAND_RECORDING: chunking_manager_add_audio() failed");
               }
               // NO direct ASR calls in Whisper mode - chunking_manager owns ASR interactions
            } else if (asr_engine == ASR_ENGINE_VOSK) {
               // Vosk mode: Direct ASR path for streaming partials (unchanged)
               if (cmd_speech_detected) {
                  // Reduced logging to avoid noise with 100ms polling
                  /* For an additional layer of "silence," check if ASR output is changing */
                  asr_result = asr_process_partial(asr_ctx, (const int16_t *)max_buff,
                                                   buff_size / sizeof(int16_t));
                  if (asr_result == NULL) {
                     LOG_ERROR("asr_process_partial() returned NULL!\n");
                  } else {
                     size_t current_length = asr_result->text ? strlen(asr_result->text) : 0;
                     // Record what ASR is hearing for TUI display
                     if (current_length > 0) {
                        metrics_set_last_asr_text(asr_result->text, 0);
                     }
                     if (current_length == prev_text_length) {
                        text_nochange = 1;
                     }
                     prev_text_length = current_length;
                     asr_result_free(asr_result);
                     asr_result = NULL;
                  }
               }
            }

            // Track speech and silence duration for pause detection
            if (cmd_speech_detected) {
               speech_duration += DEFAULT_CAPTURE_SECONDS;
               silence_duration = 0.0f;  // Reset silence when speech detected
            } else {
               silence_duration += DEFAULT_CAPTURE_SECONDS;  // Track silence duration
            }

            // Pause detection for chunking (Whisper only - Week 3 implementation)
            if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
               int should_finalize_chunk = 0;

               // Detect natural pauses for chunking (silence after sufficient speech)
               if (!cmd_speech_detected && speech_duration >= g_config.vad.chunking.min_duration &&
                   silence_duration >= g_config.vad.chunking.pause_duration) {
                  LOG_INFO("COMMAND_RECORDING: Pause detected (%.1fs) after %.1fs speech - "
                           "finalizing chunk",
                           silence_duration, speech_duration);
                  should_finalize_chunk = 1;
               }

               // Force chunk on max duration (even if speech continues)
               if (speech_duration >= g_config.vad.chunking.max_duration) {
                  LOG_INFO("COMMAND_RECORDING: Max chunk duration reached (%.1fs) - forcing chunk",
                           speech_duration);
                  should_finalize_chunk = 1;
               }

               // Finalize chunk if triggered (with re-entrance check)
               if (should_finalize_chunk && !chunking_manager_is_finalizing(chunk_mgr)) {
                  char *chunk_text = NULL;
                  int result = chunking_manager_finalize_chunk(chunk_mgr, &chunk_text);

                  if (result == 0) {
                     if (chunk_text) {
                        LOG_INFO("COMMAND_RECORDING: Chunk %zu finalized: \"%s\"",
                                 chunking_manager_get_num_chunks(chunk_mgr) - 1, chunk_text);
                        // Update TUI with what was heard
                        metrics_set_last_asr_text(chunk_text, 0);
                        free(chunk_text);
                     }
                     // Reset speech duration after successful chunk finalization
                     speech_duration = 0.0f;
                  } else {
                     LOG_ERROR("COMMAND_RECORDING: Chunk finalization failed");
                  }
               }
            }

            // Only use text_nochange for Vosk (which has meaningful partials).
            // Whisper always returns empty partials, so rely solely on VAD for silence detection.
            if (!cmd_speech_detected || (text_nochange && asr_engine == ASR_ENGINE_VOSK)) {
               commandTimeout++;
               text_nochange = 0;
            } else {
               commandTimeout = 0;
               text_nochange = 0;
            }

            if (commandTimeout >= DEFAULT_COMMAND_TIMEOUT) {
               commandTimeout = 0;
               LOG_WARNING("COMMAND_RECORDING: Command processing.\n");

               // ENGINE-AWARE FINALIZATION (Architecture Review Issue #3)
               if (asr_engine == ASR_ENGINE_WHISPER && chunk_mgr) {
                  // Whisper mode: Finalize any pending audio then get concatenated text
                  // This ensures short commands (< VAD_MIN_CHUNK_DURATION) are transcribed
                  if (chunking_manager_get_buffer_usage(chunk_mgr) > 0) {
                     char *pending_chunk = NULL;
                     LOG_INFO("COMMAND_RECORDING: Finalizing pending audio buffer");
                     int result = chunking_manager_finalize_chunk(chunk_mgr, &pending_chunk);
                     if (result == 0 && pending_chunk) {
                        LOG_INFO("COMMAND_RECORDING: Pending chunk finalized: \"%s\"",
                                 pending_chunk);
                        free(pending_chunk);
                     }
                  }

                  size_t num_chunks = chunking_manager_get_num_chunks(chunk_mgr);
                  command_text = chunking_manager_get_full_text(chunk_mgr);

                  if (command_text) {
                     LOG_WARNING("Input (from %zu chunks): %s\n", num_chunks, command_text);
                     metrics_set_last_user_command(command_text);
                  } else {
                     LOG_WARNING("Input: (no chunks finalized)\n");
                  }

                  // chunking_manager_get_full_text() internally calls reset(), so we're ready
                  // for next utterance. ASR context is still owned by chunking_manager.
                  recState = DAWN_STATE_PROCESS_COMMAND;
               } else {
                  // Vosk mode: Direct ASR finalization (unchanged)
                  asr_result = asr_finalize(asr_ctx);
                  if (asr_result == NULL) {
                     LOG_ERROR("asr_finalize() returned NULL!\n");
                  } else {
                     LOG_WARNING("Input: %s\n", asr_result->text ? asr_result->text : "");

                     if (asr_result->text) {
                        command_text = strdup(asr_result->text);
                        metrics_set_last_user_command(asr_result->text);
                     } else {
                        command_text = NULL;
                     }
                     asr_result_free(asr_result);
                     asr_result = NULL;

                     recState = DAWN_STATE_PROCESS_COMMAND;

                     // Reset ASR and chunking manager (durations will be reset when returning to
                     // DAWN_STATE_WAKEWORD_LISTEN)
                     if (chunk_mgr) {
                        chunking_manager_reset(chunk_mgr);
                     }
                     asr_reset(asr_ctx);
                  }
               }
            }
            buff_size = 0;
            break;
         case DAWN_STATE_PROCESS_COMMAND:
            // Skip processing if command is empty, whitespace, or [BLANK_AUDIO]
            if (!command_text || strlen(command_text) == 0 ||
                strspn(command_text, " \t\n\r") == strlen(command_text) ||
                strstr(command_text, "[BLANK_AUDIO]") != NULL) {
               LOG_INFO("Ignoring empty or invalid command\n");
               if (command_text) {
                  free(command_text);
                  command_text = NULL;
               }
               silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;
               recState = DAWN_STATE_SILENCE;
               // Reset all subsystems for new utterance
               reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                       &speech_duration, &recording_duration, &preroll_write_pos,
                                       &preroll_valid_bytes);
               break;
            }

            int direct_command_found = 0;

            // Add user message to conversation history first (needed for vision context)
            // We'll add it regardless of whether it's a direct command or LLM processing
            if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
               struct json_object *user_message_early = json_object_new_object();
               json_object_object_add(user_message_early, "role", json_object_new_string("user"));
               json_object_object_add(user_message_early, "content",
                                      json_object_new_string(command_text));
               json_object_array_add(conversation_history, user_message_early);
            }

            /* Process Commands before AI if LLM command processing is disabled */
            if (command_processing_mode != CMD_MODE_LLM_ONLY) {
               /* Process Commands before AI. */
               for (i = 0; i < numCommands; i++) {
                  if (searchString(commands[i].actionWordsWildcard, command_text) == 1) {
                     // Buffer sizes based on MAX_COMMAND_LENGTH (512) and MAX_WORD_LENGTH (256)
                     // from text_to_command_nuevo.h
                     char thisValue[MAX_WORD_LENGTH];  // Extracted value (device/song name)
                     char thisCommand[MAX_COMMAND_LENGTH + MAX_WORD_LENGTH];  // Template + value
                     char thisSubstring[MAX_COMMAND_LENGTH];  // actionWordsRegex minus "%s"
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
                        strcpy(thisValue,
                               extract_remaining_after_substring(command_text, thisSubstring));
                     } else {
                        int retSs = sscanf(command_text, commands[i].actionWordsRegex, thisValue);
                     }

                     /* Trim trailing punctuation from extracted value (e.g., "Iron Man." -> "Iron
                      * Man") */
                     size_t valueLen = strlen(thisValue);
                     while (valueLen > 0 &&
                            (thisValue[valueLen - 1] == '.' || thisValue[valueLen - 1] == ',' ||
                             thisValue[valueLen - 1] == '!' || thisValue[valueLen - 1] == '?' ||
                             thisValue[valueLen - 1] == ';' || thisValue[valueLen - 1] == ':')) {
                        thisValue[--valueLen] = '\0';
                     }

                     snprintf(thisCommand, sizeof(thisCommand), commands[i].actionCommand,
                              thisValue);
                     LOG_WARNING("Sending: \"%s\"\n", thisCommand);

                     // Log direct match to TUI (command bypassed LLM)
                     metrics_log_activity("Direct match: %s", commands[i].actionWordsWildcard);

                     rc = mosquitto_publish(mosq, NULL, commands[i].topic, strlen(thisCommand),
                                            thisCommand, 0, false);
                     if (rc != MOSQ_ERR_SUCCESS) {
                        LOG_ERROR("Error publishing: %s\n", mosquitto_strerror(rc));
                     } else {
                        // Log direct command to TUI activity
                        metrics_log_activity("MQTT: %s", thisCommand);
                     }

                     direct_command_found = 1;
                     break;
                  }
               }
            }

            // Handle direct command found - transition back to listening state
            if (direct_command_found) {
               // Free command text since we're done processing
               if (command_text) {
                  free(command_text);
                  command_text = NULL;
               }

               // Return to listening state
               silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;
               recState = DAWN_STATE_SILENCE;

               // Reset all subsystems for new utterance
               reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                       &speech_duration, &recording_duration, &preroll_write_pos,
                                       &preroll_valid_bytes);
               break;  // Exit DAWN_STATE_PROCESS_COMMAND case
            }

            /* Try LLM processing if:
             * - We're in LLM-only mode, OR
             * - We're in direct-first mode but no direct command was found, OR
             * - We're in direct-only mode but no direct command was found (for ignore word
             * processing)
             */
            if (command_processing_mode == CMD_MODE_LLM_ONLY ||
                (command_processing_mode == CMD_MODE_DIRECT_FIRST && !direct_command_found) ||
                (command_processing_mode == CMD_MODE_DIRECT_ONLY && !direct_command_found)) {
               LOG_WARNING("Processing with LLM (mode: %d, direct found: %d).\n",
                           command_processing_mode, direct_command_found);

               int ignoreCount = 0;

               // Check ignore words (only if we're in direct-only mode and no command found)
               if (command_processing_mode == CMD_MODE_DIRECT_ONLY && !direct_command_found) {
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
               }

               if (ignoreCount < numIgnoreWords &&
                   command_processing_mode == CMD_MODE_DIRECT_ONLY) {
                  LOG_WARNING("Input ignored. Found in ignore list.\n");
                  silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;
                  recState = DAWN_STATE_SILENCE;

                  // Reset all subsystems for new utterance (ignored empty command)
                  reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                          &speech_duration, &recording_duration, &preroll_write_pos,
                                          &preroll_valid_bytes);
               } else {
                  // User message already added at top of DAWN_STATE_PROCESS_COMMAND

                  // Check if an LLM thread is already running (from a previous request or
                  // interrupt)
                  if (llm_processing) {
                     LOG_WARNING(
                         "LLM thread already running - ignoring new request (say command again "
                         "after response completes)");

                     // Remove the user message we just added (last item in conversation history)
                     int history_len = json_object_array_length(conversation_history);
                     if (history_len > 0) {
                        json_object_array_del_idx(conversation_history, history_len - 1, 1);
                     }

                     // Free command text
                     if (command_text) {
                        free(command_text);
                        command_text = NULL;
                     }

                     // Return to listening state
                     silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;
                     recState = DAWN_STATE_SILENCE;

                     // Reset all subsystems for new utterance
                     reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                             &speech_duration, &recording_duration,
                                             &preroll_write_pos, &preroll_valid_bytes);
                     break;  // Exit DAWN_STATE_PROCESS_COMMAND case
                  }

                  // Spawn LLM thread to process request (non-blocking)
                  pthread_mutex_lock(&llm_mutex);

                  // Free any previous request/response data
                  if (llm_request_text) {
                     free(llm_request_text);
                  }
                  if (llm_response_text) {
                     free(llm_response_text);
                     llm_response_text = NULL;
                  }

                  // Set up request data (transfer ownership of command_text to thread)
                  llm_request_text = command_text;
                  command_text = NULL;  // Thread will free this

                  // Pass vision data if available
                  if (vision_ai_ready && vision_ai_image != NULL) {
                     llm_vision_image = vision_ai_image;
                     llm_vision_image_size = vision_ai_image_size;
                  } else {
                     llm_vision_image = NULL;
                     llm_vision_image_size = 0;
                  }

                  // Mark LLM as processing and record pipeline start time
                  llm_processing = 1;
                  gettimeofday(&pipeline_start_time, NULL);

                  pthread_mutex_unlock(&llm_mutex);

                  // Spawn worker thread
                  int thread_result = pthread_create(&llm_thread, NULL, llm_worker_thread, NULL);
                  if (thread_result != 0) {
                     LOG_ERROR("Failed to create LLM thread: %d", thread_result);
                     pthread_mutex_lock(&llm_mutex);
                     llm_processing = 0;
                     if (llm_request_text) {
                        free(llm_request_text);
                        llm_request_text = NULL;
                     }
                     pthread_mutex_unlock(&llm_mutex);

                     text_to_speech("I'm sorry but I'm currently unavailable boss.");
                  } else {
                     LOG_INFO("LLM thread spawned - continuing audio processing");
                  }

                  // Return to listening state - audio processing continues while LLM works
                  silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;
                  recState = DAWN_STATE_SILENCE;

                  // Reset all subsystems for new utterance
                  reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                          &speech_duration, &recording_duration, &preroll_write_pos,
                                          &preroll_valid_bytes);
                  break;  // Exit DAWN_STATE_PROCESS_COMMAND case - LLM will complete asynchronously
               }
            }

            break;  // DAWN_STATE_PROCESS_COMMAND case end
         case DAWN_STATE_VISION_AI_READY:
            pthread_mutex_lock(&tts_mutex);
            if (tts_playback_state == TTS_PLAYBACK_PAUSE) {
               tts_playback_state = TTS_PLAYBACK_PLAY;
               pthread_cond_signal(&tts_cond);
            }
            pthread_mutex_unlock(&tts_mutex);

            // Remove assistant's viewing command and replace user's trigger phrase
            // (otherwise LLM sees "what am I looking at?" and Rule #3 triggers again)
            size_t history_len = json_object_array_length(conversation_history);

            // Remove assistant's command response (should be last message)
            if (history_len > 0) {
               struct json_object *last_msg = json_object_array_get_idx(conversation_history,
                                                                        history_len - 1);
               struct json_object *role_obj;
               if (json_object_object_get_ex(last_msg, "role", &role_obj)) {
                  const char *role = json_object_get_string(role_obj);
                  if (strcmp(role, "assistant") == 0) {
                     json_object_array_del_idx(conversation_history, history_len - 1, 1);
                     history_len--;  // Update length after deletion
                  }
               }
            }

            // Replace user's vision request with neutral prompt (avoids Rule #3 trigger)
            if (history_len > 0) {
               struct json_object *last_msg = json_object_array_get_idx(conversation_history,
                                                                        history_len - 1);
               struct json_object *role_obj;
               if (json_object_object_get_ex(last_msg, "role", &role_obj)) {
                  const char *role = json_object_get_string(role_obj);
                  if (strcmp(role, "user") == 0) {
                     // Replace the content with a neutral vision description request
                     json_object_object_del(last_msg, "content");
                     json_object_object_add(last_msg, "content",
                                            json_object_new_string(
                                                "Please describe this captured image."));
                  }
               }
            }

            // Get the AI response using the image recognition.
            // The user message was already added in DAWN_STATE_PROCESS_COMMAND state
            // The vision image will be added by llm_claude.c/llm_openai.c to the last user message
            // Use streaming with TTS sentence buffering for immediate response
            // Use local session's LLM config for per-session settings
            session_llm_config_t vision_session_config;
            session_get_llm_config(local_session, &vision_session_config);
            llm_resolved_config_t vision_resolved_config;
            llm_resolve_config(&vision_session_config, &vision_resolved_config);

            response_text = llm_chat_completion_streaming_tts_with_config(
                conversation_history,
                "Describe this image in detail. Ignore any overlay graphics unless specifically "
                "asked.",
                vision_ai_image, vision_ai_image_size, dawn_tts_sentence_callback, NULL,
                &vision_resolved_config);
            if (response_text != NULL) {
               // AI returned successfully
               LOG_WARNING("AI: %s\n", response_text);
               metrics_set_last_ai_response(response_text);
               // TTS already handled by streaming callback

               // Add the successful AI response to the conversation.
               struct json_object *ai_message = json_object_new_object();
               json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
               json_object_object_add(ai_message, "content", json_object_new_string(response_text));
               json_object_array_add(conversation_history, ai_message);

               free(response_text);
            } else {
               // Error on AI response
               LOG_ERROR("GPT error.\n");
               metrics_record_error();
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
            silenceNextState = DAWN_STATE_WAKEWORD_LISTEN;
            recState = DAWN_STATE_SILENCE;

            // Reset all subsystems for new utterance (vision AI complete)
            reset_for_new_utterance(vad_ctx, asr_ctx, chunk_mgr, &silence_duration,
                                    &speech_duration, &recording_duration, &preroll_write_pos,
                                    &preroll_valid_bytes);

            break;
         // NOTE: DAWN_STATE_NETWORK_PROCESSING removed - worker threads handle network clients
         default:
            LOG_ERROR("I really shouldn't be here.\n");
      }
   }

   LOG_INFO("Quit.\n");

   // Ensure LLM thread is stopped before cleanup
   // This prevents resource leaks if restart was requested during LLM processing
   if (llm_processing) {
      LOG_INFO("Waiting for LLM thread to complete...");
      llm_request_interrupt();

      // Wait for thread with timeout (5 seconds max for responsive restart)
      struct timespec timeout;
      clock_gettime(CLOCK_REALTIME, &timeout);
      timeout.tv_sec += 5;

      int join_result = pthread_timedjoin_np(llm_thread, NULL, &timeout);
      if (join_result == 0) {
         LOG_INFO("LLM thread completed");
      } else if (join_result == ETIMEDOUT) {
         LOG_WARNING("LLM thread did not complete within timeout, cancelling");
         pthread_cancel(llm_thread);
         pthread_join(llm_thread, NULL);
      } else {
         LOG_ERROR("Error joining LLM thread: %d", join_result);
      }
      llm_processing = 0;
   }

   if (enable_network_audio) {
      LOG_INFO("Stopping network audio system...");
      accept_thread_stop();
      dawn_network_audio_cleanup();

      // Cleanup IPC resources
      pthread_mutex_lock(&processing_mutex);
      if (processing_result_data) {
         free(processing_result_data);
         processing_result_data = NULL;
         processing_complete = 0;
      }
      pthread_mutex_unlock(&processing_mutex);
      // NOTE: Network audio resources are now cleaned up by worker threads
   }

   cleanup_text_to_speech();

   // Cleanup Silero VAD
   if (vad_ctx) {
      LOG_INFO("Cleaning up Silero VAD");
      vad_silero_cleanup(vad_ctx);
      vad_ctx = NULL;
   }

   mosquitto_disconnect(mosq);
   mosquitto_loop_stop(mosq, false);
   mosquitto_lib_cleanup();

   // Cleanup SmartThings service
   smartthings_cleanup();

   // Save conversation history before cleanup
   // NOTE: conversation_history points to local session's history (owned by session_manager)
   if (conversation_history != NULL) {
      save_conversation_history(conversation_history);
   }

   // Don't json_object_put here - session_manager owns the history
   // Clear the global pointer before session cleanup to prevent use-after-free
   conversation_history = NULL;

   // NOTE: session_manager_cleanup() moved to after webui_server_shutdown()
   // WebSocket sessions hold references that are only released when WebUI shuts down

   // Cleanup command router (after workers are stopped)
   command_router_shutdown();

   // Cleanup chunking manager (if initialized)
   if (chunk_mgr) {
      chunking_manager_cleanup(chunk_mgr);
   }

   // Stop ASR recording if active and cleanup ASR
   asr_stop_recording();
   asr_cleanup(asr_ctx);

#ifdef ENABLE_AEC
   // Cleanup AEC before stopping audio capture
   aec_cleanup();
#endif

   // Stop audio capture thread and clean up resources
   if (audio_capture_ctx) {
      audio_capture_stop(audio_capture_ctx);
      audio_capture_ctx = NULL;
   }

   // Clean up audio backend (after all audio handles are closed)
   audio_backend_cleanup();

   free(max_buff);

   // Note: preroll_buffer is statically allocated, no free needed

   // Note: curl_global_cleanup() is called via atexit() registered at startup

   // Export metrics on exit with timestamped filename
   {
      time_t current_time;
      struct tm *time_info;
      char stats_filename[64];

      time(&current_time);
      time_info = localtime(&current_time);
      strftime(stats_filename, sizeof(stats_filename), "dawn_stats_%Y%m%d_%H%M%S.json", time_info);
      metrics_export_json(stats_filename);
   }

#ifdef ENABLE_WEBUI
   /* Shutdown WebUI server before session cleanup - WebSocket sessions hold references */
   if (webui_server_is_running()) {
      webui_server_shutdown();
   }
   /* Shutdown worker pool if we initialized it for WebUI (not network server) */
   if (!enable_network_audio && worker_pool_is_initialized()) {
      worker_pool_shutdown();
   }
#endif

   /* Cleanup session manager AFTER WebUI shutdown so WebSocket sessions are released */
   session_manager_cleanup();

   metrics_cleanup();

#ifdef ENABLE_TUI
   // Cleanup TUI before closing logging
   if (enable_tui) {
      tui_cleanup();
      // Re-enable console logging for any final messages
      logging_suppress_console(0);
   }
#endif

   // Close the log file properly
   close_logging();

   // Check if restart was requested (e.g., from WebUI after config change)
   // s_saved_argv[0] is set if dawn_save_argv() succeeded
   if (g_restart_requested && s_saved_argv[0] != NULL) {
      // Use /proc/self/exe for reliable path to current executable
      // This handles cases where argv[0] might be relative or modified
      extern char **environ;
      printf("Restarting DAWN...\n");
      fflush(stdout);

      // execve() replaces the entire process image, so:
      // - All memory (heap, stack, BSS) is released by the kernel
      // - No need to free s_saved_argv or other resources
      // - File descriptors without FD_CLOEXEC remain open (none expected here)
      execve("/proc/self/exe", s_saved_argv, environ);

      // If we get here, execve failed
      perror("Failed to restart DAWN");
      return 1;
   }

   // s_saved_argv uses static storage, no cleanup needed
   return 0;
}
