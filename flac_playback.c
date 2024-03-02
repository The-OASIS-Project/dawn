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

#include <pulse/simple.h>
#include <pulse/error.h>
#include <FLAC/stream_decoder.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mosquitto.h>

#include "flac_playback.h"
#include "mosquitto_comms.h"

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
      printf("Sample rate: %u Hz\n", info.sample_rate);
      printf("Channels: %u\n", info.channels);
      printf("Bits per sample: %u\n", info.bits_per_sample);
      printf("Min blocksize: %u\n", info.min_blocksize);
      printf("Max blocksize: %u\n", info.max_blocksize);
      printf("Min framesize: %u\n", info.min_framesize);
      printf("Max framesize: %u\n", info.max_framesize);
      printf("Total samples: %lu\n", info.total_samples);
      printf("MD5: ");
      for(int i = 0; i < 16; ++i) {
         printf("%02x", info.md5sum[i]);
      }
      printf("\n");
   }
   else if(metadata->type == FLAC__METADATA_TYPE_PICTURE) {
      printf("*** Got FLAC__METADATA_TYPE_PICTURE.\n");
      FLAC__StreamMetadata_Picture picture = metadata->data.picture;

      if(picture.type == FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER) {
         printf("Found front cover art. MIME type: %s\n", picture.mime_type);

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
   fprintf(stderr, "FLAC Error callback: %s\n", status_str);
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
      printf("Stop playback requested.\n");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   // Cast client_data back to pa_simple pointer for audio playback.
   pa_simple *s = (pa_simple *)client_data;

   // Allocate memory for interleaved audio samples.
   int16_t *interleaved = malloc(frame->header.blocksize * frame->header.channels * sizeof(int16_t));
   if (!interleaved) {
       fprintf(stderr, "Memory allocation failed for interleaved audio buffer.\n");
       return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   // Interleave audio samples from all channels into a single buffer.
   for (unsigned i = 0, j = 0; i < frame->header.blocksize; ++i) {
      for (unsigned ch = 0; ch < frame->header.channels; ++ch, ++j) {
         interleaved[j] = buffer[ch][i];
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
      fprintf(stderr, "Error opening PulseAudio for playback: %s\n", pa_strerror(error));
      return NULL;
   }

   // Initialize FLAC decoder.
   FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
   if (decoder == NULL) {
      fprintf(stderr, "Error creating FLAC decorder.\n");
      pa_simple_free(pa_handle);
      return NULL;
   }

   // Enable MD5 checking for the decoder.
   if (!FLAC__stream_decoder_set_md5_checking(decoder, true)) {
      fprintf(stderr, "Error setting FLAC md5 checking.\n");
      FLAC__stream_decoder_delete(decoder);
      pa_simple_free(pa_handle);
      return NULL;
   }

   music_play = 1; // Ensure playback is enabled.
   if ((init_status = FLAC__stream_decoder_init_file(decoder, args->file_name, write_callback,
                                                     metadata_callback, error_callback, pa_handle))
         != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      fprintf(stderr, "ERROR: initializing decoder: %s\n", FLAC__StreamDecoderInitStatusString[init_status]);
      FLAC__stream_decoder_delete(decoder);
      pa_simple_free(pa_handle);
      return NULL;
   }

   // Process the FLAC stream until the end or an error occurs.
   if (!(error = FLAC__stream_decoder_process_until_end_of_stream(decoder))) {
      fprintf(stderr, "Error during FLAC decoding process.\n");
   } else {
      fprintf(stderr, "Decoding completed successfully.\n");
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

#ifdef ENABLE_MAIN
int main(int argc, char *argv[]) {
   if (argc < 4) {
      fprintf(stderr, "Usage: %s <sink_name> <file_name> <start_time>\n", argv[0]);
      return 1;
   }

   PlaybackArgs args;
   args.sink_name = argv[1];
   args.file_name = argv[2];
   args.start_time = atoi(argv[3]);

   printf("Playing: %s %s %d\n", args.sink_name, args.file_name, args.start_time);

   pthread_t audio_thread;
    
   // Create the playback thread
   if (pthread_create(&audio_thread, NULL, play_flac_audio, &args)) {
      fprintf(stderr, "Error creating thread\n");
      return 1;
   }

   // Do other stuff or just wait
   // ...

   // Stop playback and exit
   //pthread_cancel(audio_thread);
   pthread_join(audio_thread, NULL);

   return 0;
}
#endif
