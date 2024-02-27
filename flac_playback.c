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

static int music_play = 0;

void setMusicPlay(int play) {
   music_play = play;
}

int getMusicPlay(void) {
   return music_play;
}

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

void error_callback(
   const FLAC__StreamDecoder *decoder,
   FLAC__StreamDecoderErrorStatus status,
   void *client_data) {
   const char *status_str = FLAC__StreamDecoderErrorStatusString[status];
   fprintf(stderr, "FLAC Error callback: %s\n", status_str);
}

FLAC__StreamDecoderWriteStatus write_callback(
   const FLAC__StreamDecoder *decoder,
   const FLAC__Frame *frame,
   const FLAC__int32 *const buffer[],
   void *client_data) {

   if (!music_play) {
      printf("Stop playback requested.\n");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   pa_simple *s = (pa_simple *)client_data;
   int16_t *interleaved = malloc(frame->header.blocksize * frame->header.channels * sizeof(int16_t));
   for (unsigned i = 0, j = 0; i < frame->header.blocksize; ++i) {
      for (unsigned ch = 0; ch < frame->header.channels; ++ch, ++j) {
         interleaved[j] = buffer[ch][i];
      }
   }

   if (pa_simple_write(s, interleaved, frame->header.blocksize * frame->header.channels * sizeof(int16_t), NULL) < 0) {
      free(interleaved);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   free(interleaved);
   return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void *playFlacAudio(void *arg) {
   PlaybackArgs *args = (PlaybackArgs *)arg;
   pa_simple *pa_handle = NULL;

   // Initialize PulseAudio
   pa_sample_spec ss;
   ss.format = PA_SAMPLE_S16LE;
   ss.channels = 2;
   ss.rate = 44100;

   FLAC__StreamDecoderInitStatus init_status;

   int error = 0;

   if (!(pa_handle = pa_simple_new(NULL, "FLAC Player", PA_STREAM_PLAYBACK, args->sink_name,
                                   "playback", &ss, NULL, NULL, &error))) {
      fprintf(stderr, "Error opening PulseAudio record: %s\n", pa_strerror(error));
      return NULL;
   }

   // Initialize FLAC decoder
   FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
   if (decoder == NULL) {
      printf("Error creating FLAC decorder.\n");

      return NULL;
   }
   if (!FLAC__stream_decoder_set_md5_checking(decoder, true)) {
      printf("Error setting FLAC md5 checking.\n");
      return NULL;
   }

   music_play = 1;
   if ((init_status = FLAC__stream_decoder_init_file(decoder, args->file_name, write_callback,
                                                     metadata_callback, error_callback, pa_handle))
         != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      fprintf(stderr, "ERROR: initializing decoder: %s\n", FLAC__StreamDecoderInitStatusString[init_status]);

      return NULL;
   }

   if (!(error = FLAC__stream_decoder_process_until_end_of_stream(decoder))) {
      printf("Error in decoding...\n");
   } else {
      fprintf(stderr, "decoding: %s\n", "succeeded");
      fprintf(stderr, "   state: %s\n", FLAC__StreamDecoderStateString[FLAC__stream_decoder_get_state(decoder)]);
   }

   // Cleanup
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
