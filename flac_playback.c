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

#include <pulse/simple.h>
#include <pulse/error.h>
#include <FLAC/stream_decoder.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mosquitto.h>

#include "flac_playback.h"
#include "logging.h"
#include "mosquitto_comms.h"

/**
 * @var float global_volume
 * @brief Global volume control for audio playback.
 *
 * This variable controls the volume level of audio playback throughout the application.
 * The volume level is represented as a floating-point number between 0.0 and 1.0,
 * where 0.0 means silence and 1.0 corresponds to the maximum volume level without amplification.
 * Values above 1.0 may result in amplification and potentially introduce distortion or clipping.
 *
 * Usage:
 * Assign a value to this variable to adjust the playback volume before starting or during audio playback.
 * For example, setting `global_volume = 0.75;` adjusts the volume to 75% of the maximum level.
 */
static float global_volume = 0.5f;

// Global variable to control music playback state.
// When set to 0, music playback is stopped.
// When set to 1, music playback is active.
static int music_play = 0;

/**
 * Sets the music playback state.
 *
 * @param play An integer indicating the desired playback state.
 *             Set to 1 to start playback, or 0 to stop playback.
 */
void setMusicPlay(int play) {
   music_play = play;
}

/**
 * Retrieves the current music playback state.
 *
 * @return An integer representing the playback state.
 *         Returns 1 if playback is active, or 0 if playback is stopped.
 */
int getMusicPlay(void) {
   return music_play;
}

/**
 * Callback function for processing FLAC metadata.
 * This function is called by the FLAC decoder when metadata is encountered in the FLAC stream.
 *
 * @param decoder The FLAC stream decoder instance.
 * @param metadata The metadata object encountered in the stream.
 * @param client_data Optional user data provided to the decoder.
 */
void metadata_callback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data) {
   if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
      FLAC__StreamMetadata_StreamInfo info = metadata->data.stream_info;
      LOG_INFO("Sample rate: %u Hz", info.sample_rate);
      LOG_INFO("Channels: %u", info.channels);
      LOG_INFO("Bits per sample: %u", info.bits_per_sample);
   }
   else if(metadata->type == FLAC__METADATA_TYPE_PICTURE) {
      LOG_INFO("*** Got FLAC__METADATA_TYPE_PICTURE.");
      FLAC__StreamMetadata_Picture picture = metadata->data.picture;

      if(picture.type == FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER) {
         LOG_INFO("Found front cover art. MIME type: %s", picture.mime_type);

         // Now, picture.data contains the image data of length picture.data_length
         // You can write this data to a file or use it as needed.
      }
   }
}

/**
 * Callback function for handling errors during FLAC decoding.
 * This function is called by the FLAC decoder when an error occurs.
 *
 * @param decoder The FLAC stream decoder instance.
 * @param status The error status code indicating the type of error.
 * @param client_data Optional user data provided to the decoder.
 */
void error_callback(
   const FLAC__StreamDecoder *decoder,
   FLAC__StreamDecoderErrorStatus status,
   void *client_data) {
   const char *status_str = FLAC__StreamDecoderErrorStatusString[status];
   LOG_ERROR("FLAC Error callback: %s", status_str);
}

/**
 * Callback function for handling decoded audio frames from the FLAC stream.
 * This function is called by the FLAC decoder each time an audio frame is successfully decoded.
 * It interleaves the decoded audio samples and writes them to the PulseAudio playback stream.
 *
 * @param decoder The FLAC stream decoder instance calling this callback.
 * @param frame The decoded audio frame containing audio samples to be processed.
 * @param buffer An array of pointers to the decoded audio samples for each channel.
 * @param client_data A pointer to user-defined data, in this case, used to pass the PulseAudio simple API playback stream.
 *
 * @return A FLAC__StreamDecoderWriteStatus value indicating whether the write operation was successful.
 *         Returns FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE to continue decoding,
 *         or FLAC__STREAM_DECODER_WRITE_STATUS_ABORT to stop decoding due to an error.
 */
FLAC__StreamDecoderWriteStatus write_callback(
   const FLAC__StreamDecoder *decoder,
   const FLAC__Frame *frame,
   const FLAC__int32 *const buffer[],
   void *client_data) {

   // Check if music playback has been stopped externally.
   if (!music_play) {
      LOG_WARNING("Stop playback requested.");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   // Cast client_data back to pa_simple pointer for audio playback.
   pa_simple *s = (pa_simple *)client_data;

   // Allocate memory for interleaved audio samples.
   int16_t *interleaved = malloc(frame->header.blocksize * frame->header.channels * sizeof(int16_t));
   if (!interleaved) {
       LOG_ERROR("Memory allocation failed for interleaved audio buffer.");
       return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   // Interleave audio samples from all channels into a single buffer.
   for (unsigned i = 0, j = 0; i < frame->header.blocksize; ++i) {
      for (unsigned ch = 0; ch < frame->header.channels; ++ch, ++j) {
         // Adjust the sample volume before interleaving.
         int32_t adjusted_sample = (int32_t)(buffer[ch][i] * global_volume);

         // Clipping protection (optional but recommended).
         if (adjusted_sample < INT16_MIN) {
            adjusted_sample = INT16_MIN;
         } else if (adjusted_sample > INT16_MAX) {
            adjusted_sample = INT16_MAX;
         }

         interleaved[j] = (int16_t)adjusted_sample;
      }
   }

   // Write the interleaved audio samples to the PulseAudio stream.
   if (pa_simple_write(s, interleaved, frame->header.blocksize * frame->header.channels * sizeof(int16_t), NULL) < 0) {
      free(interleaved);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   // Free the allocated memory for the interleaved buffer after writing to the audio stream.
   free(interleaved);

   // Signal the FLAC decoder to continue decoding the next frame.
   return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/**
 * Plays a FLAC audio file.
 * This function sets up a FLAC decoder and a PulseAudio playback stream to play the specified FLAC audio file.
 * It utilizes callbacks for writing decoded audio to the PulseAudio stream, handling metadata, and managing errors.
 *
 * @param arg A pointer to a PlaybackArgs structure containing playback parameters such as the file name and PulseAudio sink name.
 * @return NULL always. This function does not return a value and is intended to be used with threading.
 *
 * The function performs the following steps:
 * 1. Initializes a PulseAudio playback stream with a specified sample format, channel count, and sample rate.
 * 2. Creates a new FLAC stream decoder and configures it for MD5 checking to verify the integrity of the decoded audio.
 * 3. Initializes the FLAC decoder with the specified file and sets up callbacks for writing audio data, handling metadata, and errors.
 * 4. Starts the decoding process and continues until the end of the stream or an error occurs.
 * 5. Cleans up resources by finishing the decoding process, deleting the decoder, and freeing the PulseAudio stream.
 * 6. If an error occurs during decoding, a callback is triggered to handle the next action, such as playing the next track.
 */
void *playFlacAudio(void *arg) {
   PlaybackArgs *args = (PlaybackArgs *)arg;

   // Initialize PulseAudio for playback.
   pa_simple *pa_handle = NULL;
   pa_sample_spec ss = {
      .format = PA_SAMPLE_S16LE,
      .channels = 2,
      .rate = 44100
   };

   FLAC__StreamDecoderInitStatus init_status;

   int error = 0;

   // Open PulsAudio for playback.
   if (!(pa_handle = pa_simple_new(NULL, "FLAC Player", PA_STREAM_PLAYBACK, args->sink_name,
                                   "playback", &ss, NULL, NULL, &error))) {
      LOG_ERROR("Error opening PulseAudio for playback: %s", pa_strerror(error));
      return NULL;
   }

   // Initialize FLAC decoder.
   FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
   if (decoder == NULL) {
      LOG_ERROR("Error creating FLAC decorder.");
      pa_simple_free(pa_handle);
      return NULL;
   }

   // Enable MD5 checking for the decoder.
   if (!FLAC__stream_decoder_set_md5_checking(decoder, true)) {
      LOG_ERROR("Error setting FLAC md5 checking.");
      FLAC__stream_decoder_delete(decoder);
      pa_simple_free(pa_handle);
      return NULL;
   }

   music_play = 1; // Ensure playback is enabled.
   if ((init_status = FLAC__stream_decoder_init_file(decoder, args->file_name, write_callback,
                                                     metadata_callback, error_callback, pa_handle))
         != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      LOG_ERROR("ERROR: initializing decoder: %s", FLAC__StreamDecoderInitStatusString[init_status]);
      FLAC__stream_decoder_delete(decoder);
      pa_simple_free(pa_handle);
      return NULL;
   }

   // Process the FLAC stream until the end or an error occurs.
   if (!(error = FLAC__stream_decoder_process_until_end_of_stream(decoder))) {
      LOG_ERROR("Error during FLAC decoding process.");
   } else {
      LOG_INFO("Decoding completed successfully.");
   }

   // Cleanup and resource management.
   FLAC__stream_decoder_finish(decoder);
   FLAC__stream_decoder_delete(decoder);
   pa_simple_free(pa_handle);

   if (error) {
      musicCallback("next", NULL);
   }

   return NULL;
}

/**
 * @brief Sets the global music playback volume.
 *
 * This function adjusts the global volume level for music playback across the application.
 * It directly modifies the `global_volume` variable, which affects the volume at which audio is played.
 * The volume level should be specified as a float between 0.0 and 2.0, where 0.0 is complete silence,
 * 1.0 is the maximum volume level, greater than 1.0 is amplification.
 * Values outside this range may lead to undefined behavior.
 *
 * @param val The new volume level as a float. Valid values range from 0.0 to 2.0.
 * @note It's recommended to clamp the value of `val` within the 0.0 to 2.0 range before calling this function
 *       to avoid unexpected behavior.
 */
void setMusicVolume(float val) {
   global_volume = val;
}
