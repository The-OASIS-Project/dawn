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

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#include "dawn.h"
#include "logging.h"

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
/**
 * Continuously captures audio from a specified input device and plays it back through a specified output device,
 * effectively amplifying the captured sound. This function is intended to be run in a separate thread and will
 * continue running until `setStopVA` is called to set the global `running` flag to 0.
 *
 * This implementation is specific to systems with ALSA support. It uses ALSA's API to open PCM devices for
 * capture and playback, and to handle audio data transfer between these devices.
 *
 * Prerequisites:
 * - ALSA (Advanced Linux Sound Architecture) must be supported and configured on the system.
 * - Proper ALSA devices must be available and specified by the user or configuration.
 *
 * @param arg Unused parameter, included for compatibility with pthreads' start routine signature.
 * @return NULL Always returns NULL, indicating the thread has completed execution.
 *
 * Note:
 * - Error handling is incorporated to address issues with device initialization and audio data transfer.
 * - The global `running` variable controls the main loop. Use `setStopVA` to request thread termination.
 */
void* voiceAmplificationThread(void* arg) {
   char *pcmCaptureDevice = getPcmCaptureDevice();
   char *pcmPlaybackDevice = findAudioPlaybackDevice("speakers");
   snd_pcm_t *inputHandle = NULL;
   snd_pcm_t *outputHandle = NULL;
   int error = 0;

   if (!(pcmCaptureDevice && pcmPlaybackDevice)) {
      LOG_ERROR("Unable to find audio devices.");
      return NULL;
   }

   if ((error = snd_pcm_open(&inputHandle, pcmCaptureDevice, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
      LOG_ERROR("Error opening input PCM device: %s", snd_strerror(error));
      return NULL;
   }

   if ((error = snd_pcm_open(&outputHandle, pcmPlaybackDevice, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
      LOG_ERROR("Error opening output PCM device: %s", snd_strerror(error));
      return NULL;
   }

   running = 1;
   uint8_t buffer[BUFSIZE];

   while (running) {
      if ((error = snd_pcm_readi(inputHandle, buffer, sizeof(buffer))) < 0) {
         LOG_ERROR("Error reading: %s", snd_strerror(error));
         break;
      }

      if ((error = snd_pcm_writei(outputHandle, buffer, sizeof(buffer))) < 0) {
         LOG_ERROR("Error writing: %s", snd_strerror(error));
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
/**
 * Implements a real-time voice amplification loop using PulseAudio for both audio capture and playback.
 * It captures audio from a microphone and plays it back through speakers in real-time until told to stop.
 * This function is specifically designed to run in a separate thread and relies on the global `running` flag
 * to control the execution of its main loop.
 *
 * Prerequisites:
 * - PulseAudio must be supported and properly configured on the system.
 * - The specified audio capture (microphone) and playback (speakers) devices must be available.
 *
 * @param arg Unused parameter, included for compatibility with pthreads' start routine signature.
 * @return NULL Always returns NULL, indicating the thread has completed execution.
 *
 * Note:
 * - The function initializes PulseAudio streams for both input and output using the specified device names.
 * - The global `running` variable controls the loop execution, enabling external control to start or stop the voice amplification.
 * - Proper error handling is implemented to manage issues during audio capture and playback initialization and operation.
 * - Resource management ensures that PulseAudio streams are freed appropriately before thread termination.
 */
void* voiceAmplificationThread(void* arg) {
   // PulseAudio simple API objects for input and output.
   pa_simple *input = NULL, *output = NULL;
   int error = 0; // Variable to capture PulseAudio error codes.

   // Retrieve the PCM device names for capture and playback.
   const char *pcmCaptureDevice = getPcmCaptureDevice();
   const char *pcmPlaybackDevice = findAudioPlaybackDevice("speakers");

   // Validate playback device availability.
   if (pcmPlaybackDevice == NULL) {
      LOG_ERROR("Unabled to find \"speakers\" device.");
      return NULL;
   }

   // Initialize the PulseAudio input stream.
   if (!(input = pa_simple_new(NULL, "Mic Amp (In)", PA_STREAM_RECORD, pcmCaptureDevice, "record", &ss, NULL, NULL, &error))) {
      LOG_ERROR("Error initializing input: %s", pa_strerror(error));
      return NULL;
   }

   // Initialize the PulseAudio output stream.
   if (!(output = pa_simple_new(NULL, "Mic Amp (Out)", PA_STREAM_PLAYBACK, pcmPlaybackDevice, "playback", &ss, NULL, NULL, &error))) {
      LOG_ERROR("Error initializing output: %s", pa_strerror(error));
      pa_simple_free(input); // Ensure input is freed if output initialization fails.
      return NULL;
   }

   running = 1;
   // Main loop for capturing and playing back audio in real-time.
   uint8_t buffer[BUFSIZE]; // Buffer for audio data.
   while (running) {
      // Read audio data from input.
      if (pa_simple_read(input, buffer, sizeof(buffer), &error) < 0) {
         LOG_ERROR("Error reading: %s", pa_strerror(error));
         break;
      }

      // Write audio data to output.
      if (pa_simple_write(output, buffer, sizeof(buffer), &error) < 0) {
         LOG_ERROR("Error writing: %s", pa_strerror(error));
         break;
      }
   }

   pa_simple_free(output);
   pa_simple_free(input);

   return NULL;
}
#endif
