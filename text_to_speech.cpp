#include <atomic>
#include <queue>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sstream>
#include <vector>

#include "dawn.h"
#include "logging.h"
#include "piper.hpp"
#include "text_to_command_nuevo.h"
#include "text_to_speech.h"

#define DEFAULT_RATE       22050
#define DEFAULT_CHANNELS   1
#define DEFAULT_FRAMES     2

#ifdef ALSA_DEVICE
#include <alsa/asoundlib.h>

#define DEFAULT_ACCESS     SND_PCM_ACCESS_RW_INTERLEAVED
#define DEFAULT_FORMAT     SND_PCM_FORMAT_S16_LE

#else
#include <pulse/simple.h>
#include <pulse/error.h>

#define DEFAULT_PULSE_FORMAT  PA_SAMPLE_S16LE
#endif

using namespace std;
using namespace piper;

typedef struct {
    PiperConfig config;
    Voice voice;
    int is_initialized;
    char pcm_capture_device[MAX_WORD_LENGTH + 1];
#ifdef ALSA_DEVICE
    snd_pcm_t *handle;
    snd_pcm_uframes_t frames;
#else
    pa_simple *pa_handle;
#endif
} TTS_Handle;

// Global TTS_Handle object
static TTS_Handle tts_handle;

#ifdef ALSA_DEVICE
int openAlsaPcmPlaybackDevice(snd_pcm_t **handle, char *pcm_device, snd_pcm_uframes_t *frames)
{
   snd_pcm_hw_params_t *params = NULL;
   unsigned int rate = DEFAULT_RATE;
   int dir = 0;
   *frames = DEFAULT_FRAMES;
   int rc = 0;

   LOG_INFO("ALSA PLAYBACK DRIVER\n");
   /* Open PCM device for playback. */
   rc = snd_pcm_open(handle, pcm_device, SND_PCM_STREAM_PLAYBACK, 0);
   if (rc < 0) {
      LOG_ERROR("unable to open pcm device for playback (%s): %s", pcm_device, snd_strerror(rc));
      return 1;
   }

   snd_pcm_hw_params_alloca(&params);
   snd_pcm_hw_params_any(*handle, params);
   snd_pcm_hw_params_set_access(*handle, params, DEFAULT_ACCESS);
   snd_pcm_hw_params_set_format(*handle, params, DEFAULT_FORMAT);
   snd_pcm_hw_params_set_channels(*handle, params, DEFAULT_CHANNELS);
   snd_pcm_hw_params_set_rate_near(*handle, params, &rate, &dir);
   snd_pcm_hw_params_set_period_size_near(*handle, params, frames, &dir);
   rc = snd_pcm_hw_params(*handle, params);
   if (rc < 0) {
      LOG_ERROR("unable to set hw parameters: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   return 0;
}
#else
pa_simple *openPulseaudioPlaybackDevice(char *pcm_playback_device)
{
   static const pa_sample_spec ss = {
      .format = DEFAULT_PULSE_FORMAT,
      .rate = DEFAULT_RATE,
      .channels = DEFAULT_CHANNELS
   };

   int rc = 0;
   pa_simple *pa_handle = NULL;

   LOG_INFO("PULSEAUDIO PLAYBACK DRIVER: %s", pcm_playback_device);

   /* Create a new PulseAudio simple connection for playback. */
   pa_handle = pa_simple_new(NULL, APPLICATION_NAME, PA_STREAM_PLAYBACK, pcm_playback_device, "playback", &ss, NULL, NULL, &rc);
   if (!pa_handle) {
      LOG_ERROR("PA simple error: %s", pa_strerror(rc));
      return NULL;
   }

   return pa_handle;
}
#endif

/**
 * @brief Thread-safe queue for text-to-speech requests.
 */
std::queue<std::string> tts_queue;

/**
 * @brief Mutex for synchronizing access to the text-to-speech queue.
 */
pthread_mutex_t tts_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Condition variable to signal the worker thread when new data is available.
 */
pthread_cond_t tts_queue_cond = PTHREAD_COND_INITIALIZER;

/**
 * @brief Thread handle for the text-to-speech worker thread.
 */
pthread_t tts_thread;

/**
 * @brief Flag indicating whether the worker thread should continue running.
 */
bool tts_thread_running = false;

extern "C" {
   /**
    * @brief Worker thread function that processes text-to-speech requests.
    *
    * This function runs in a separate thread and continuously processes text strings
    * from the queue, converting them to speech and playing them back using the audio
    * system.
    *
    * @param arg Unused parameter.
    * @return Always returns NULL.
    */
   void* tts_thread_function(void* arg) {
      LOG_INFO("tts_thread_function() started.");

      // Declare the interruption flag
      std::atomic<bool> tts_stop_processing(false);

      while (!get_quit()) {
         pthread_mutex_lock(&tts_queue_mutex);

         LOG_INFO("Waiting on text...");
         // Wait until there's text to process or the thread is signaled to exit
         while (tts_queue.empty() && tts_thread_running) {
            pthread_cond_wait(&tts_queue_cond, &tts_queue_mutex);
         }
         LOG_INFO("Text acquired.");

         // Exit if the thread is no longer running and the queue is empty
         if (!tts_thread_running && tts_queue.empty()) {
            pthread_mutex_unlock(&tts_queue_mutex);
            break;
         }

         // Retrieve text from the queue
         std::string inputText = tts_queue.front();
         tts_queue.pop();
         pthread_mutex_unlock(&tts_queue_mutex);

         // Process text-to-speech
         SynthesisResult result;
         std::vector<int16_t> audioBuffer;
         int rc = 0;
         int error = 0;

         // Reset the interruption flag
         tts_stop_processing.store(false);

         // Convert text to audio data
         textToAudio(tts_handle.config, tts_handle.voice, inputText, audioBuffer, result, tts_stop_processing, [&]() {
            pthread_mutex_lock(&tts_mutex);
            tts_playback_state = TTS_PLAYBACK_PLAY;
            pthread_mutex_unlock(&tts_mutex);

#ifdef ALSA_DEVICE
            // Play audio data using ALSA
            for (size_t i = 0; i < audioBuffer.size(); i += tts_handle.frames) {
               // Check playback state
               pthread_mutex_lock(&tts_mutex);
               while (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                  LOG_WARNING("TTS playback is PAUSED.");
                  pthread_cond_wait(&tts_cond, &tts_mutex);
               }
               if (tts_playback_state == TTS_PLAYBACK_DISCARD) {
                  LOG_WARNING("TTS unpaused to DISCARD.");
                  tts_playback_state = TTS_PLAYBACK_IDLE;
                  audioBuffer.clear();
                  LOG_WARNING("Emptying TTS queue.");
                  while (!tts_queue.empty()) {
                     tts_queue.pop();
                  }
                  pthread_mutex_unlock(&tts_mutex);

                  tts_stop_processing.store(true);
                  return;
               } else if (tts_playback_state == TTS_PLAYBACK_PLAY) {
                  LOG_WARNING("TTS unpaused to PLAY.");
               } else if (tts_playback_state == TTS_PLAYBACK_IDLE) {
                  LOG_WARNING("TTS unpaused to IDLE.");
               } else {
                  LOG_ERROR("TTS unpaused to UNKNOWN.");
               }
               pthread_mutex_unlock(&tts_mutex);

               // Write audio frames
               rc = snd_pcm_writei(tts_handle.handle, &audioBuffer[i],
                                   std::min(tts_handle.frames, audioBuffer.size() - i));
               if (rc == -EPIPE) {
                  LOG_ERROR("ALSA underrun occurred");
                  snd_pcm_prepare(tts_handle.handle);
               } else if (rc < 0) {
                  LOG_ERROR("ALSA error from writei: %s", snd_strerror(rc));
               }
            }
#else
            const size_t chunk_frames = 1024;  // Adjust as needed
            const size_t chunk_bytes = chunk_frames * sizeof(int16_t);
            size_t total_bytes = audioBuffer.size() * sizeof(int16_t);

            for (size_t i = 0; i < total_bytes; i += chunk_bytes) {
               // Check playback state
               pthread_mutex_lock(&tts_mutex);
               while (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                  LOG_WARNING("TTS playback is PAUSED.");
                  pthread_cond_wait(&tts_cond, &tts_mutex);
               }
               if (tts_playback_state == TTS_PLAYBACK_DISCARD) {
                  LOG_WARNING("TTS unpaused to DISCARD.");
                  tts_playback_state = TTS_PLAYBACK_IDLE;
                  audioBuffer.clear();
                  LOG_WARNING("Emptying TTS queue.");
                  while (!tts_queue.empty()) {
                     tts_queue.pop();
                  }
                  pthread_mutex_unlock(&tts_mutex);

                  tts_stop_processing.store(true);
                  return;
               } else if (tts_playback_state == TTS_PLAYBACK_PLAY) {
                  //LOG_WARNING("TTS unpaused to PLAY.");
               } else if (tts_playback_state == TTS_PLAYBACK_IDLE) {
                  LOG_WARNING("TTS unpaused to IDLE.");
               } else {
                  LOG_ERROR("TTS unpaused to UNKNOWN.");
               }
               pthread_mutex_unlock(&tts_mutex);

               // Calculate bytes to write
               size_t bytes_to_write = std::min(chunk_bytes, total_bytes - i);

               // Write audio data
               rc = pa_simple_write(tts_handle.pa_handle, ((uint8_t*)audioBuffer.data()) + i,
                                    bytes_to_write, &error);
               if (rc < 0) {
                  LOG_ERROR("PulseAudio error from pa_simple_write: %s", pa_strerror(error));
                  //audioBuffer.clear();

                  // Close the current PulseAudio connection
                  if (tts_handle.pa_handle) {
                     pa_simple_free(tts_handle.pa_handle);
                     tts_handle.pa_handle = NULL;
                  }

                  // Reopen the PulseAudio playback device
                  tts_handle.pa_handle = openPulseaudioPlaybackDevice(tts_handle.pcm_capture_device);
                  if (tts_handle.pa_handle == NULL) {
                     LOG_ERROR("Error re-opening PulseAudio playback device.");
                  }
               }
            }
#endif
            // Clear the audio buffer for the next request
            audioBuffer.clear();

            pthread_mutex_lock(&tts_mutex);
            tts_playback_state = TTS_PLAYBACK_IDLE;
            pthread_mutex_unlock(&tts_mutex);
         });

         tts_stop_processing.store(false);
      }

      return NULL;
   }

   /**
    * @brief Initializes the text-to-speech system.
    *
    * This function loads the voice model, initializes the TTS engine, opens the
    * audio playback device, and starts the worker thread that processes TTS requests.
    *
    * @param pcm_device The name of the PCM device to use for audio playback.
    */
   void initialize_text_to_speech(char* pcm_device) {
      // Load the voice model
      std::optional<SpeakerId> speakerIdOpt = 0;
      loadVoice(tts_handle.config, "en_GB-alba-medium.onnx", "en_GB-alba-medium.onnx.json", tts_handle.voice, speakerIdOpt, false);

      // Initialize the TTS engine
      initialize(tts_handle.config);

      strcpy(tts_handle.pcm_capture_device, pcm_device);

      // Configure synthesis parameters
      tts_handle.voice.synthesisConfig.lengthScale = 0.85f;

      tts_handle.is_initialized = 1;

      // Initialize synchronization primitives
      pthread_mutex_init(&tts_queue_mutex, NULL);
      pthread_cond_init(&tts_queue_cond, NULL);

      // Open the audio playback device once
#ifdef ALSA_DEVICE
      int rc = openAlsaPcmPlaybackDevice(&(tts_handle.handle), tts_handle.pcm_capture_device, &(tts_handle.frames));
      if (rc) {
         LOG_ERROR("Error creating ALSA playback device.");
         // Handle error (e.g., set tts_handle.is_initialized = 0)
         tts_handle.is_initialized = 0;
         return;
      }
#else
      int error = 0;
      tts_handle.pa_handle = openPulseaudioPlaybackDevice(tts_handle.pcm_capture_device);
      if (tts_handle.pa_handle == NULL) {
         LOG_ERROR("Error creating Pulse playback device.");
         // Handle error (e.g., set tts_handle.is_initialized = 0)
         tts_handle.is_initialized = 0;
         return;
      }
#endif

      // Start the worker thread
      tts_thread_running = true;
      pthread_create(&tts_thread, NULL, tts_thread_function, NULL);
   }

   /**
    * @brief Enqueues a text string for conversion to speech.
    *
    * This function can be safely called from multiple threads. It adds the provided
    * text to a queue that is processed by a dedicated worker thread.
    *
    * @param text The text to be converted to speech.
    */
   void text_to_speech(char* text) {
      if (!tts_handle.is_initialized) {
         LOG_ERROR("Text-to-Speech system not initialized. Call initialize_text_to_speech() first.");
         return;
      }

      assert(text != nullptr && "Received a null pointer");
      std::string inputText(text);

      // Add text to the processing queue
      pthread_mutex_lock(&tts_queue_mutex);
      tts_queue.push(inputText);
      pthread_cond_signal(&tts_queue_cond);
      pthread_mutex_unlock(&tts_queue_mutex);
   }

   /**
    * @brief Cleans up the text-to-speech system.
    *
    * This function signals the worker thread to terminate, waits for it to finish,
    * closes the audio playback device, and then releases all resources used by the TTS engine.
    */
   void cleanup_text_to_speech() {
      if (!tts_handle.is_initialized) {
         LOG_ERROR("Text-to-Speech system not initialized. Call initialize_text_to_speech() first.");
         return;
      }

      // Signal the worker thread to exit
      pthread_mutex_lock(&tts_queue_mutex);
      tts_thread_running = false;
      pthread_cond_signal(&tts_queue_cond);
      pthread_mutex_unlock(&tts_queue_mutex);

      // Wait for the worker thread to finish
      pthread_join(tts_thread, NULL);

      // Close the audio playback device
#ifdef ALSA_DEVICE
      if (tts_handle.handle) {
         snd_pcm_close(tts_handle.handle);
         tts_handle.handle = NULL;
      }
#else
      if (tts_handle.pa_handle) {
         pa_simple_free(tts_handle.pa_handle);
         tts_handle.pa_handle = NULL;
      }
#endif

      // Destroy synchronization primitives
      pthread_mutex_destroy(&tts_queue_mutex);
      pthread_cond_destroy(&tts_queue_cond);

      // Clean up the TTS engine
      terminate(tts_handle.config);

      tts_handle.is_initialized = 0;
   }

   void remove_chars(char *str, const char *remove_chars) {
      char *src, *dst;
      bool should_remove;
      for (src = dst = str; *src != '\0'; src++) {
         should_remove = false;
         for (const char *rc = remove_chars; *rc != '\0'; rc++) {
            if (*src == *rc) {
               should_remove = true;
               break;
            }
         }
         if (!should_remove) {
            *dst++ = *src;
         }
      }
      *dst = '\0';
   }

   bool is_emoji(unsigned int codepoint) {
    // Basic emoji ranges (not exhaustive)
    return (codepoint >= 0x1F600 && codepoint <= 0x1F64F) || // Emoticons
           (codepoint >= 0x1F300 && codepoint <= 0x1F5FF) || // Miscellaneous Symbols and Pictographs
           (codepoint >= 0x1F680 && codepoint <= 0x1F6FF) || // Transport and Map Symbols
           (codepoint >= 0x2600 && codepoint <= 0x26FF) ||   // Miscellaneous Symbols
           (codepoint >= 0x2700 && codepoint <= 0x27BF) ||   // Dingbats
           (codepoint >= 0x1F900 && codepoint <= 0x1F9FF);   // Supplemental Symbols and Pictographs
   }

   void remove_emojis(char *str) {
      char *src, *dst;
      src = dst = str;

      while (*src) {
         unsigned char byte = *src;
         unsigned int codepoint = 0;
         int bytes_in_char = 1;

         if (byte < 0x80) {
            codepoint = byte; // 1-byte ASCII character
         } else if (byte < 0xE0) {
            codepoint = (byte & 0x1F) << 6;
            codepoint |= (*(src + 1) & 0x3F);
            bytes_in_char = 2;
         } else if (byte < 0xF0) {
            codepoint = (byte & 0x0F) << 12;
            codepoint |= (*(src + 1) & 0x3F) << 6;
            codepoint |= (*(src + 2) & 0x3F);
            bytes_in_char = 3;
         } else {
            codepoint = (byte & 0x07) << 18;
            codepoint |= (*(src + 1) & 0x3F) << 12;
            codepoint |= (*(src + 2) & 0x3F) << 6;
            codepoint |= (*(src + 3) & 0x3F);
            bytes_in_char = 4;
         }

         if (!is_emoji(codepoint)) {
            for (int i = 0; i < bytes_in_char; i++) {
               *dst++ = *src++;
            }
         } else {
            src += bytes_in_char; // Skip emoji
         }
      }
      *dst = '\0'; // Null-terminate the filtered string
   }
}
