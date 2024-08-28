#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sstream>
#include <vector>

#include "text_to_command_nuevo.h"
#include "dawn.h"
#include "logging.h"
#include "piper.hpp"

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

extern "C" {
   // Initialization function
   void initialize_text_to_speech(char *pcm_device) {
      // Load the voice
      //std::optional<SpeakerId> speakerIdOpt = std::nullopt;
      std::optional<SpeakerId> speakerIdOpt = 0;
      loadVoice(tts_handle.config, "en_GB-alba-medium.onnx", "en_GB-alba-medium.onnx.json", tts_handle.voice, speakerIdOpt, false);
      // Initialize the piper
      initialize(tts_handle.config);

      strcpy(tts_handle.pcm_capture_device, pcm_device);

      tts_handle.voice.synthesisConfig.lengthScale = 0.85f;

      tts_handle.is_initialized = 1;
   }

   void text_to_speech(char* text) {
      int error = 0;

      if (!tts_handle.is_initialized) {
         LOG_ERROR("Text-to-Speech system not initialized. Call initialize_text_to_speech() first.");
         return;
      }

      assert(text != nullptr && "Received a null pointer");
      std::string inputText(text);

      // Piper
      SynthesisResult result;
      vector<int16_t> audioBuffer;

      int rc = 0;

#ifdef ALSA_DEVICE
      int rc = openAlsaPcmPlaybackDevice(&(tts_handle.handle), tts_handle.pcm_capture_device, &(tts_handle.frames));
      if (rc) {
         LOG_ERROR("Error creating ALSA playback device.");
         return;
      }
#else
      tts_handle.pa_handle = openPulseaudioPlaybackDevice(tts_handle.pcm_capture_device);
      if (tts_handle.pa_handle == NULL) {
         LOG_ERROR("Error creating Pulse playback device.");
         return;
      }
#endif

      // Convert text to audio data
      textToAudio(tts_handle.config, tts_handle.voice, inputText, audioBuffer, result, [&]() {
#ifdef ALSA_DEVICE
         // In the callback, play the audio data using ALSA
         for (size_t i = 0; i < audioBuffer.size(); i += frames) {
            rc = snd_pcm_writei(tts_handle.handle, &audioBuffer[i], min(tts_handle.frames, audioBuffer.size() - i));
            if (rc == -EPIPE) {
               /* EPIPE means underrun */
               LOG_ERROR("ALSA underrun occurred");
               snd_pcm_prepare(tts_handle.handle);
            } else if (rc < 0) {
               LOG_ERROR("ALSA error from writei: %s", snd_strerror(rc));
            }
         }
#else
         rc = pa_simple_write(tts_handle.pa_handle, audioBuffer.data(), audioBuffer.size() * sizeof(int16_t), &error);
         if (rc < 0) {
            LOG_ERROR("PulseAudio error from pa_simple_write: %s", pa_strerror(rc));
            audioBuffer.clear();
            return;
         }

         pa_simple_drain(tts_handle.pa_handle, NULL);
         pa_simple_flush(tts_handle.pa_handle, NULL);
#endif
         // Clear the audio buffer for the next sentence
         audioBuffer.clear();
      });

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
   }

   // Cleanup function
   void cleanup_text_to_speech() {
      if (!tts_handle.is_initialized) {
         LOG_ERROR("Text-to-Speech system not initialized. Call initialize_text_to_speech() first.");
         return;
      }

      // Clean up Piper
      terminate(tts_handle.config);

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
} // extern "C"

