#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sstream>
#include <vector>

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

#ifdef ALSA_DEVICE
int openAlsaPcmPlaybackDevice(snd_pcm_t **handle, const char *pcm_device, snd_pcm_uframes_t *frames)
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
    void text_to_speech(char *pcm_device, char* text) {
        // Convert the input text to a std::string
        string inputText(text);

        // Piper
        PiperConfig config;
        Voice voice;
        SynthesisResult result;
        vector<int16_t> audioBuffer;

#ifdef ALSA_DEVICE
        // Define the ALSA parameters
        snd_pcm_t *handle = NULL;
        snd_pcm_uframes_t frames = 0;
#else
        // Define the Pulse parameters
        pa_simple *pa_handle = NULL;
        int error = 0;
#endif

        int rc = 0;

        // Initialize the piper
        initialize(config);
        
        // Load the voice
        std::optional<SpeakerId> speakerIdOpt = std::nullopt;
        loadVoice(config, "en_GB-alba-medium.onnx", "en_GB-alba-medium.onnx.json", voice, speakerIdOpt);
        voice.synthesisConfig.lengthScale = 0.85f;

#ifdef ALSA_DEVICE
        rc = openAlsaPcmPlaybackDevice(&handle, pcm_device, &frames);
        if (rc) {
           LOG_ERROR("Error creating ALSA playback device.");
           return;
        }
#else
        pa_handle = openPulseaudioPlaybackDevice(pcm_device);
        if (pa_handle == NULL) {
           LOG_ERROR("Error creating Pulse playback device.");
           return;
        }
#endif

        // Convert text to audio data
        textToAudio(config, voice, inputText, audioBuffer, result, [&]() {
#ifdef ALSA_DEVICE
            // In the callback, play the audio data using ALSA
            for (size_t i = 0; i < audioBuffer.size(); i += frames) {
               rc = snd_pcm_writei(handle, &audioBuffer[i], min(frames, audioBuffer.size() - i));
               if (rc == -EPIPE) {
                  /* EPIPE means underrun */
                  LOG_ERROR("ALSA underrun occurred");
                  snd_pcm_prepare(handle);
               } else if (rc < 0) {
                  LOG_ERROR("ALSA error from writei: %s", snd_strerror(rc));
               }
            }
#else
            rc = pa_simple_write(pa_handle, audioBuffer.data(), audioBuffer.size() * sizeof(int16_t), &error);
            if (rc < 0) {
               LOG_ERROR("PulseAudio error from pa_simple_write: %s", pa_strerror(rc));
            }
#endif
            // Clear the audio buffer for the next sentence
            audioBuffer.clear();
        });

        // Clean up
        terminate(config);

#ifdef ALSA_DEVICE
        snd_pcm_drain(handle);
        snd_pcm_close(handle);
#else
        pa_simple_drain(pa_handle, NULL);
        pa_simple_free(pa_handle);
#endif
    }
}

