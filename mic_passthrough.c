#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include "dawn.h"

#ifdef ALSA_DEVICE
#include <alsa/asoundlib.h>
#else
#include <pulse/simple.h>
#include <pulse/error.h>
#endif

#define BUFSIZE 256

#ifndef ALSA_DEVICE
static const pa_sample_spec ss = {
    .format = PA_SAMPLE_S16LE,
    .rate = 44100,
    .channels = 2
};
#endif

int running = 1; // Control variable

void setStopVA(void) {
   running = 0;
}

#ifdef ALSA_DEVICE
void* voiceAmplificationThread(void* arg) {
   char *pcmCaptureDevice = getPcmCaptureDevice();
   char *pcmPlaybackDevice = findAudioPlaybackDevice("speakers");
   snd_pcm_t *inputHandle = NULL;
   snd_pcm_t *outputHandle = NULL;
   int error = 0;

   if (!(pcmCaptureDevice && pcmPlaybackDevice)) {
      printf("Unable to find audio devices.\n");
      return NULL;
   }

   if ((error = snd_pcm_open(&inputHandle, pcmCaptureDevice, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
      fprintf(stderr, "Error opening input PCM device: %s\n", snd_strerror(error));
      return NULL;
   }

   if ((error = snd_pcm_open(&outputHandle, pcmPlaybackDevice, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
      fprintf(stderr, "Error opening output PCM device: %s\n", snd_strerror(error));
      return NULL;
   }

   running = 1;
   uint8_t buffer[BUFSIZE];

   while (running) {
      if ((error = snd_pcm_readi(inputHandle, buffer, sizeof(buffer))) < 0) {
         fprintf(stderr, "Error reading: %s\n", snd_strerror(error));
         break;
      }

      if ((error = snd_pcm_writei(outputHandle, buffer, sizeof(buffer))) < 0) {
         fprintf(stderr, "Error writing: %s\n", snd_strerror(error));
         break;
      }
   }

   if (outputHandle != NULL) {
      snd_pcm_close(outputHandle);
   }
   if (inputHandle != NULL) {
      snd_pcm_close(inputHandle);
   }

   return NULL;
}
#else
void* voiceAmplificationThread(void* arg) {
   char *pcmCaptureDevice = getPcmCaptureDevice();
   char *pcmPlaybackDevice = findAudioPlaybackDevice("speakers");
   pa_simple *input = NULL, *output = NULL;
   int error = 0;

   if (pcmPlaybackDevice == NULL) {
      printf("Unabled to find \"speakers\" device.\n");
      return NULL;
   }

   if (!(input = pa_simple_new(NULL, "Mic Amp (In)", PA_STREAM_RECORD, pcmCaptureDevice, "record", &ss, NULL, NULL, &error))) {
      fprintf(stderr, "Error initializing input: %s\n", pa_strerror(error));
      return NULL;
   }

   if (!(output = pa_simple_new(NULL, "Mic Amp (Out)", PA_STREAM_PLAYBACK, pcmPlaybackDevice, "playback", &ss, NULL, NULL, &error))) {
      fprintf(stderr, "Error initializing output: %s\n", pa_strerror(error));
      return NULL;
   }

   running = 1;
   uint8_t buffer[BUFSIZE];
   while (running) {
      if (pa_simple_read(input, buffer, sizeof(buffer), &error) < 0) {
         fprintf(stderr, "Error reading: %s\n", pa_strerror(error));
         break;
      }

      if (pa_simple_write(output, buffer, sizeof(buffer), &error) < 0) {
         fprintf(stderr, "Error writing: %s\n", pa_strerror(error));
         break;
      }
   }

   if (output != NULL) {
      pa_simple_free(output);
   }
   if (input != NULL) {
      pa_simple_free(input);
   }

   return NULL;
}
#endif
