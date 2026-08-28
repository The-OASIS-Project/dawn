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
 *
 * WebUI Audio Processing - Opus codec and ASR integration for browser clients
 */

#include "webui/webui_audio.h"

#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "asr/asr_interface.h"
#include "audio/resampler.h"
#include "auth/auth_db.h"
#include "core/conv_event.h"
#include "core/session_manager.h"
#include "core/text_filter.h"
#include "core/turn_queue.h"
#include "core/utterance_dedup.h"
#include "core/worker_pool.h"
#include "llm/llm_context.h"
#include "logging.h"
#include "tts/text_to_speech.h"
#include "tts/tts_preprocessing.h"
#include "utils/asr_transcript.h"
#include "utils/sentence_buffer.h"
#include "webui/webui_always_on.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Module State
 * ============================================================================= */

static OpusDecoder *s_decoder = NULL;
static OpusEncoder *s_encoder = NULL;
static resampler_t *s_input_resampler = NULL; /* 48000Hz → 16000Hz for ASR */
static resampler_t *s_tts_resampler = NULL;   /* 22050Hz → 48000Hz for TTS output */

/* Split mutexes: decoder+input_resampler vs encoder+tts_resampler operate independently.
 * This allows concurrent encode (TTS output) and decode (ASR input) across sessions. */
static pthread_mutex_t s_decode_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_encode_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_audio_mutex = PTHREAD_MUTEX_INITIALIZER; /* lifecycle only */
static atomic_bool s_initialized = false;

/* Note: ASR contexts are borrowed from worker pool (no local ASR context)
 * This avoids loading Whisper model twice and saves GPU memory */

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int webui_audio_init(void) {
   pthread_mutex_lock(&s_audio_mutex);

   if (s_initialized) {
      OLOG_WARNING("WebUI audio already initialized");
      pthread_mutex_unlock(&s_audio_mutex);
      return WEBUI_AUDIO_SUCCESS;
   }

   int err;

   /* Create Opus decoder (for incoming audio from browser) */
   s_decoder = opus_decoder_create(WEBUI_OPUS_SAMPLE_RATE, WEBUI_OPUS_CHANNELS, &err);
   if (err != OPUS_OK || !s_decoder) {
      OLOG_ERROR("WebUI audio: Failed to create Opus decoder: %s", opus_strerror(err));
      pthread_mutex_unlock(&s_audio_mutex);
      return WEBUI_AUDIO_ERROR;
   }

   /* Create Opus encoder (for outgoing TTS audio to browser) */
   s_encoder = opus_encoder_create(WEBUI_OPUS_SAMPLE_RATE, WEBUI_OPUS_CHANNELS,
                                   OPUS_APPLICATION_VOIP, &err);
   if (err != OPUS_OK || !s_encoder) {
      OLOG_ERROR("WebUI audio: Failed to create Opus encoder: %s", opus_strerror(err));
      opus_decoder_destroy(s_decoder);
      s_decoder = NULL;
      pthread_mutex_unlock(&s_audio_mutex);
      return WEBUI_AUDIO_ERROR;
   }

   /* Configure encoder for voice */
   opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(WEBUI_OPUS_BITRATE));
   opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(5)); /* Balanced quality/CPU (0-10 scale) */
   opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_AUTO));

   /* Create resampler for input audio (48kHz → 16kHz for ASR) */
   s_input_resampler = resampler_create(WEBUI_OPUS_SAMPLE_RATE, WEBUI_ASR_SAMPLE_RATE, 1);
   if (!s_input_resampler) {
      OLOG_ERROR("WebUI audio: Failed to create input resampler");
      opus_encoder_destroy(s_encoder);
      opus_decoder_destroy(s_decoder);
      s_encoder = NULL;
      s_decoder = NULL;
      pthread_mutex_unlock(&s_audio_mutex);
      return WEBUI_AUDIO_ERROR;
   }

   /* Create resampler for TTS output (22050Hz → 48kHz for Opus output).
    * MEDIUM quality: inaudible for speech but ~4-5x cheaper than BEST, so a long
    * sentence's resample no longer spikes CPU and starves the music stream thread
    * (was a ~680ms synchronous burst per long sentence). */
   s_tts_resampler = resampler_create_ex(22050, WEBUI_OPUS_SAMPLE_RATE, 1,
                                         RESAMPLER_QUALITY_MEDIUM);
   if (!s_tts_resampler) {
      OLOG_ERROR("WebUI audio: Failed to create TTS resampler");
      resampler_destroy(s_input_resampler);
      opus_encoder_destroy(s_encoder);
      opus_decoder_destroy(s_decoder);
      s_input_resampler = NULL;
      s_encoder = NULL;
      s_decoder = NULL;
      pthread_mutex_unlock(&s_audio_mutex);
      return WEBUI_AUDIO_ERROR;
   }

   /* Verify worker pool is initialized (provides ASR contexts) */
   if (!worker_pool_is_initialized()) {
      OLOG_WARNING("WebUI audio: Worker pool not initialized - ASR will be unavailable");
   }

   s_initialized = true;
   OLOG_INFO("WebUI audio initialized (Opus %dHz, ASR %dHz, via worker pool)",
             WEBUI_OPUS_SAMPLE_RATE, WEBUI_ASR_SAMPLE_RATE);

   pthread_mutex_unlock(&s_audio_mutex);
   return WEBUI_AUDIO_SUCCESS;
}

void webui_audio_cleanup(void) {
   pthread_mutex_lock(&s_audio_mutex);

   if (!s_initialized) {
      pthread_mutex_unlock(&s_audio_mutex);
      return;
   }

   /* Note: ASR contexts are owned by worker pool, not cleaned up here */

   if (s_input_resampler) {
      resampler_destroy(s_input_resampler);
      s_input_resampler = NULL;
   }

   if (s_tts_resampler) {
      resampler_destroy(s_tts_resampler);
      s_tts_resampler = NULL;
   }

   if (s_encoder) {
      opus_encoder_destroy(s_encoder);
      s_encoder = NULL;
   }

   if (s_decoder) {
      opus_decoder_destroy(s_decoder);
      s_decoder = NULL;
   }

   s_initialized = false;
   OLOG_INFO("WebUI audio cleaned up");

   pthread_mutex_unlock(&s_audio_mutex);
}

bool webui_audio_is_initialized(void) {
   return atomic_load(&s_initialized);
}

/* =============================================================================
 * Opus Decoding Functions
 * ============================================================================= */

int webui_opus_decode_stream(const uint8_t *opus_data,
                             size_t opus_len,
                             int16_t **pcm_out,
                             size_t *pcm_samples) {
   if (!opus_data || opus_len == 0 || !pcm_out || !pcm_samples) {
      return WEBUI_AUDIO_ERROR;
   }

   if (!atomic_load(&s_initialized)) {
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Heuristic buffer sizing: typical Opus expansion ratio is ~120x (bytes → samples).
    * Clamp between a reasonable minimum and WEBUI_PCM_MAX_SAMPLES. */
   size_t est_samples = opus_len * 120;
   if (est_samples < 48000)
      est_samples = 48000; /* minimum 1 second */
   size_t max_output_samples = (est_samples < WEBUI_PCM_MAX_SAMPLES) ? est_samples
                                                                     : WEBUI_PCM_MAX_SAMPLES;
   int16_t *pcm_buffer = malloc(max_output_samples * sizeof(int16_t));
   if (!pcm_buffer) {
      return WEBUI_AUDIO_ERROR_ALLOC;
   }

   pthread_mutex_lock(&s_decode_mutex);

   if (!s_decoder) {
      pthread_mutex_unlock(&s_decode_mutex);
      free(pcm_buffer);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   size_t total_samples = 0;
   size_t offset = 0;

   /* Parse length-prefixed Opus frames: [2-byte length][opus frame]... */
   while (offset + 2 <= opus_len) {
      /* Read frame length (little-endian) */
      uint16_t frame_len = opus_data[offset] | (opus_data[offset + 1] << 8);
      offset += 2;

      if (frame_len == 0 || offset + frame_len > opus_len) {
         OLOG_WARNING("WebUI audio: Invalid frame length %u at offset %zu", frame_len, offset - 2);
         break;
      }

      /* Decode frame */
      int decoded = opus_decode(s_decoder, opus_data + offset, frame_len,
                                pcm_buffer + total_samples,
                                (int)(max_output_samples - total_samples), 0);

      if (decoded < 0) {
         OLOG_WARNING("WebUI audio: Opus decode error: %s", opus_strerror(decoded));
         /* Try packet loss concealment */
         decoded = opus_decode(s_decoder, NULL, 0, pcm_buffer + total_samples,
                               WEBUI_OPUS_FRAME_SAMPLES, 0);
         if (decoded < 0) {
            decoded = 0; /* Skip this frame */
         }
      }

      /* Verify decoded count doesn't exceed remaining buffer space */
      if (decoded > 0 && (size_t)decoded > max_output_samples - total_samples) {
         OLOG_ERROR("WebUI audio: Opus decode returned more samples than buffer space");
         break;
      }

      total_samples += decoded;
      offset += frame_len;

      /* Safety check - stop before buffer is completely full */
      if (total_samples >= max_output_samples - WEBUI_OPUS_FRAME_SAMPLES * 2) {
         OLOG_WARNING("WebUI audio: PCM buffer nearly full, stopping decode");
         break;
      }
   }

   pthread_mutex_unlock(&s_decode_mutex);

   if (total_samples == 0) {
      free(pcm_buffer);
      return WEBUI_AUDIO_ERROR_DECODE;
   }

   /* Buffer may be slightly oversized but is valid - skip realloc for shrinking
    * as it adds overhead for minimal benefit and may cause fragmentation */
   *pcm_out = pcm_buffer;
   *pcm_samples = total_samples;

   OLOG_INFO("WebUI audio: Decoded %zu samples from %zu bytes Opus", total_samples, opus_len);
   return WEBUI_AUDIO_SUCCESS;
}

int webui_opus_decode_frame(const uint8_t *opus_frame,
                            size_t opus_len,
                            int16_t *pcm_out,
                            int max_samples) {
   if (!opus_frame || opus_len == 0 || !pcm_out || max_samples <= 0) {
      return -1;
   }

   if (!atomic_load(&s_initialized)) {
      return -1;
   }

   pthread_mutex_lock(&s_decode_mutex);

   if (!s_decoder) {
      pthread_mutex_unlock(&s_decode_mutex);
      return -1;
   }

   int decoded = opus_decode(s_decoder, opus_frame, (int)opus_len, pcm_out, max_samples, 0);

   pthread_mutex_unlock(&s_decode_mutex);

   return decoded;
}

/* =============================================================================
 * Opus Encoding Functions
 * ============================================================================= */

int webui_opus_encode_stream(const int16_t *pcm_data,
                             size_t pcm_samples,
                             uint8_t **opus_out,
                             size_t *opus_len,
                             size_t *frame_count_out) {
   if (!pcm_data || pcm_samples == 0 || !opus_out || !opus_len) {
      return WEBUI_AUDIO_ERROR;
   }

   if (!atomic_load(&s_initialized)) {
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   pthread_mutex_lock(&s_encode_mutex);

   if (!s_encoder) {
      pthread_mutex_unlock(&s_encode_mutex);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Calculate number of frames (20ms each at 16kHz = 320 samples) */
   size_t frame_samples = WEBUI_OPUS_FRAME_SAMPLES;
   size_t num_frames = (pcm_samples + frame_samples - 1) / frame_samples;

   /* Allocate output buffer: 2 bytes length prefix + max frame size per frame */
   size_t max_output_size = num_frames * (2 + WEBUI_OPUS_MAX_FRAME_SIZE);
   uint8_t *output = malloc(max_output_size);
   if (!output) {
      pthread_mutex_unlock(&s_encode_mutex);
      return WEBUI_AUDIO_ERROR_ALLOC;
   }

   size_t output_offset = 0;
   size_t input_offset = 0;

   /* Temporary buffer for last partial frame (padded with zeros) */
   int16_t frame_buffer[WEBUI_OPUS_FRAME_SAMPLES];

   for (size_t i = 0; i < num_frames; i++) {
      const int16_t *frame_ptr;
      size_t samples_available = pcm_samples - input_offset;

      if (samples_available >= frame_samples) {
         frame_ptr = pcm_data + input_offset;
      } else {
         /* Pad last frame with zeros */
         memcpy(frame_buffer, pcm_data + input_offset, samples_available * sizeof(int16_t));
         memset(frame_buffer + samples_available, 0,
                (frame_samples - samples_available) * sizeof(int16_t));
         frame_ptr = frame_buffer;
      }

      /* Encode frame */
      uint8_t opus_frame[WEBUI_OPUS_MAX_FRAME_SIZE];
      int encoded_bytes = opus_encode(s_encoder, frame_ptr, (int)frame_samples, opus_frame,
                                      sizeof(opus_frame));

      if (encoded_bytes < 0) {
         OLOG_WARNING("WebUI audio: Opus encode error: %s", opus_strerror(encoded_bytes));
         input_offset += frame_samples;
         continue;
      }

      /* Write length prefix (little-endian) */
      output[output_offset++] = (uint8_t)(encoded_bytes & 0xFF);
      output[output_offset++] = (uint8_t)((encoded_bytes >> 8) & 0xFF);

      /* Write encoded frame */
      memcpy(output + output_offset, opus_frame, encoded_bytes);
      output_offset += encoded_bytes;

      input_offset += frame_samples;
   }

   pthread_mutex_unlock(&s_encode_mutex);

   if (output_offset == 0) {
      free(output);
      return WEBUI_AUDIO_ERROR_ENCODE;
   }

   /* Buffer may be slightly oversized but is valid - skip realloc for shrinking */
   *opus_out = output;
   *opus_len = output_offset;
   if (frame_count_out)
      *frame_count_out = num_frames;

   OLOG_INFO("WebUI audio: Encoded %zu samples to %zu bytes Opus (%zu frames)", pcm_samples,
             output_offset, num_frames);
   return WEBUI_AUDIO_SUCCESS;
}

/* =============================================================================
 * ASR Integration Functions
 * ============================================================================= */

int webui_audio_transcribe(const int16_t *pcm_data, size_t pcm_samples, char **text_out) {
   if (!pcm_data || pcm_samples == 0 || !text_out) {
      return WEBUI_AUDIO_ERROR;
   }

   if (!atomic_load(&s_initialized)) {
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Borrow an ASR context from the worker pool */
   asr_context_t *asr_ctx = worker_pool_borrow_asr();
   if (!asr_ctx) {
      OLOG_WARNING("WebUI audio: All workers busy, cannot transcribe");
      return WEBUI_AUDIO_ERROR_ASR;
   }

   /* Reset ASR for new utterance */
   asr_reset(asr_ctx);

   /* Feed audio to ASR */
   asr_process_partial(asr_ctx, pcm_data, pcm_samples);

   /* Get final transcription */
   asr_result_t *result = asr_finalize(asr_ctx);

   /* Return ASR context to pool immediately */
   worker_pool_return_asr(asr_ctx);

   if (!result || !result->text || strlen(result->text) == 0) {
      if (result) {
         asr_result_free(result);
      }
      return WEBUI_AUDIO_ERROR_ASR;
   }

   *text_out = strdup(result->text);
   OLOG_INFO("WebUI audio: ASR result: \"%s\" (%.1fms)", result->text, result->processing_time);

   asr_result_free(result);

   if (!*text_out) {
      return WEBUI_AUDIO_ERROR_ALLOC;
   }

   return WEBUI_AUDIO_SUCCESS;
}

/**
 * @brief Resample 48kHz PCM to 16kHz for ASR (shared helper for opus_to_text and pcm48k_to_text)
 */
static int resample_48k_to_16k(const int16_t *pcm_in,
                               size_t in_samples,
                               int16_t **pcm_out,
                               size_t *out_samples) {
   if (!atomic_load(&s_initialized)) {
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   pthread_mutex_lock(&s_decode_mutex);
   if (!s_input_resampler) {
      pthread_mutex_unlock(&s_decode_mutex);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Pre-allocate with 10% margin to avoid reallocs during chunked processing */
   size_t output_size = resampler_get_output_size(s_input_resampler, in_samples);
   output_size = (output_size * 11) / 10;
   int16_t *resampled = malloc(output_size * sizeof(int16_t));
   if (!resampled) {
      pthread_mutex_unlock(&s_decode_mutex);
      return WEBUI_AUDIO_ERROR_ALLOC;
   }

   size_t total_resampled = 0;
   size_t chunk_size = RESAMPLER_MAX_SAMPLES;
   size_t offset = 0;

   while (offset < in_samples) {
      size_t remaining = in_samples - offset;
      size_t to_process = (remaining < chunk_size) ? remaining : chunk_size;
      size_t available_output = output_size - total_resampled;

      size_t resampled_chunk = resampler_process(s_input_resampler, pcm_in + offset, to_process,
                                                 resampled + total_resampled, available_output);

      if (resampled_chunk == 0) {
         OLOG_ERROR("WebUI audio: 48k→16k resampling failed at offset %zu", offset);
         break;
      }

      total_resampled += resampled_chunk;
      offset += to_process;
   }
   pthread_mutex_unlock(&s_decode_mutex);

   if (total_resampled == 0) {
      free(resampled);
      return WEBUI_AUDIO_ERROR;
   }

   *pcm_out = resampled;
   *out_samples = total_resampled;
   return WEBUI_AUDIO_SUCCESS;
}

int webui_audio_opus_to_text(const uint8_t *opus_data, size_t opus_len, char **text_out) {
   int16_t *pcm = NULL;
   size_t pcm_samples = 0;

   /* Decode Opus to PCM (48kHz) */
   int ret = webui_opus_decode_stream(opus_data, opus_len, &pcm, &pcm_samples);
   if (ret != WEBUI_AUDIO_SUCCESS) {
      return ret;
   }

   /* Resample 48kHz → 16kHz for ASR */
   int16_t *resampled = NULL;
   size_t total_resampled = 0;
   int resample_ret = resample_48k_to_16k(pcm, pcm_samples, &resampled, &total_resampled);
   free(pcm);
   if (resample_ret != WEBUI_AUDIO_SUCCESS) {
      return resample_ret;
   }

   OLOG_INFO("WebUI audio: Resampled %zu → %zu samples (48kHz → 16kHz)", pcm_samples,
             total_resampled);

   /* Transcribe resampled PCM (16kHz) */
   ret = webui_audio_transcribe(resampled, total_resampled, text_out);
   free(resampled);

   return ret;
}

int webui_audio_pcm48k_to_text(const int16_t *pcm_data, size_t pcm_samples, char **text_out) {
   if (!pcm_data || pcm_samples == 0 || !text_out) {
      return WEBUI_AUDIO_ERROR;
   }

   /* Resample 48kHz → 16kHz for ASR */
   int16_t *resampled = NULL;
   size_t total_resampled = 0;
   int resample_ret = resample_48k_to_16k(pcm_data, pcm_samples, &resampled, &total_resampled);
   if (resample_ret != WEBUI_AUDIO_SUCCESS) {
      return resample_ret;
   }

   OLOG_INFO("WebUI audio: PCM48k resampled %zu → %zu samples (48kHz → 16kHz)", pcm_samples,
             total_resampled);

   /* Transcribe resampled PCM (16kHz) */
   int ret = webui_audio_transcribe(resampled, total_resampled, text_out);
   free(resampled);

   return ret;
}

/* =============================================================================
 * TTS Integration Functions
 * ============================================================================= */

/* Resample borrowed 22050Hz TTS PCM → 48000Hz (shared s_tts_resampler under s_encode_mutex).
 * §Phase-4 "from PCM" primitive: does NOT own @pcm22k (the multi-target fan reuses one synth
 * across formats).  Returns a freshly-malloc'd 48k buffer in *pcm48k_out / *samples48k_out. */
static int webui_audio_pcm22k_to_pcm48k(const int16_t *pcm22k,
                                        size_t samples22k,
                                        int16_t **pcm48k_out,
                                        size_t *samples48k_out) {
   if (!pcm22k || samples22k == 0 || !pcm48k_out || !samples48k_out) {
      return WEBUI_AUDIO_ERROR;
   }

   pthread_mutex_lock(&s_encode_mutex);

   if (!s_tts_resampler) {
      pthread_mutex_unlock(&s_encode_mutex);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Pre-allocate with 10% margin to avoid reallocs during chunked processing */
   size_t output_size = resampler_get_output_size(s_tts_resampler, samples22k);
   output_size = (output_size * 11) / 10;
   int16_t *resampled = malloc(output_size * sizeof(int16_t));
   if (!resampled) {
      pthread_mutex_unlock(&s_encode_mutex);
      return WEBUI_AUDIO_ERROR_ALLOC;
   }

   /* Process in chunks if needed (resampler has max chunk size) */
   size_t total_resampled = 0;
   size_t chunk_size = RESAMPLER_MAX_SAMPLES;
   size_t offset = 0;

   while (offset < samples22k) {
      size_t remaining = samples22k - offset;
      size_t to_process = (remaining < chunk_size) ? remaining : chunk_size;
      size_t available_output = output_size - total_resampled;

      size_t resampled_chunk = resampler_process(s_tts_resampler, pcm22k + offset, to_process,
                                                 resampled + total_resampled, available_output);

      if (resampled_chunk == 0) {
         OLOG_ERROR("WebUI audio: Resampling failed at offset %zu", offset);
         break;
      }

      total_resampled += resampled_chunk;
      offset += to_process;
   }

   pthread_mutex_unlock(&s_encode_mutex);

   if (total_resampled == 0) {
      free(resampled);
      return WEBUI_AUDIO_ERROR;
   }

   *pcm48k_out = resampled;
   *samples48k_out = total_resampled;
   return WEBUI_AUDIO_SUCCESS;
}

int webui_audio_text_to_opus(const char *text,
                             uint8_t **opus_out,
                             size_t *opus_len,
                             size_t *frame_count_out) {
   if (!text || strlen(text) == 0 || !opus_out || !opus_len) {
      return WEBUI_AUDIO_ERROR;
   }

   /* Generate TTS audio (22050Hz) */
   int16_t *tts_pcm = NULL;
   size_t tts_samples = 0;
   uint32_t tts_rate = 0;

   int ret = text_to_speech_to_pcm(text, &tts_pcm, &tts_samples, &tts_rate);
   if (ret != 0 || !tts_pcm || tts_samples == 0) {
      OLOG_ERROR("WebUI audio: TTS failed");
      return WEBUI_AUDIO_ERROR;
   }

   OLOG_INFO("WebUI audio: TTS generated %zu samples at %uHz", tts_samples, tts_rate);

   /* Resample 22k->48k then Opus-encode.  (The §Phase-4 fanout inlines these two steps itself so
    * it can share one 48k buffer across Opus + raw-PCM recipients, so this is the sole caller —
    * no separate pcm22k_to_opus wrapper.) */
   int16_t *resampled = NULL;
   size_t total_resampled = 0;
   ret = webui_audio_pcm22k_to_pcm48k(tts_pcm, tts_samples, &resampled, &total_resampled);
   free(tts_pcm);
   if (ret != WEBUI_AUDIO_SUCCESS) {
      return ret;
   }

   ret = webui_opus_encode_stream(resampled, total_resampled, opus_out, opus_len, frame_count_out);
   free(resampled);
   return ret;
}

int webui_audio_text_to_pcm(const char *text, int16_t **pcm_out, size_t *pcm_samples) {
   if (!text || strlen(text) == 0 || !pcm_out || !pcm_samples) {
      return WEBUI_AUDIO_ERROR;
   }

   /* Generate TTS audio (22050Hz) */
   int16_t *tts_pcm = NULL;
   size_t tts_samples = 0;
   uint32_t tts_rate = 0;

   int ret = text_to_speech_to_pcm(text, &tts_pcm, &tts_samples, &tts_rate);
   if (ret != 0 || !tts_pcm || tts_samples == 0) {
      OLOG_ERROR("WebUI audio: TTS failed");
      return WEBUI_AUDIO_ERROR;
   }

   OLOG_INFO("WebUI audio: TTS generated %zu samples at %uHz", tts_samples, tts_rate);

   ret = webui_audio_pcm22k_to_pcm48k(tts_pcm, tts_samples, pcm_out, pcm_samples);
   free(tts_pcm);
   return ret;
}

/* =============================================================================
 * WebSocket Audio Message Handlers
 *
 * Protocol:
 * - Client sends binary frames with 1-byte type prefix:
 *   - WS_BIN_AUDIO_IN (0x01): Opus audio chunk (length-prefixed frames)
 *   - WS_BIN_AUDIO_IN_END (0x02): End of utterance (triggers ASR + LLM + TTS)
 *
 * - Server responds with binary frames:
 *   - WS_BIN_AUDIO_OUT (0x11): PCM audio chunk for playback
 *   - WS_BIN_AUDIO_SEGMENT_END (0x12): Play accumulated audio segment now
 * ============================================================================= */

typedef struct {
   session_t *session;
   uint8_t *audio_data;
   size_t audio_len;
   bool use_opus;            /* True if audio_data is Opus-encoded, false if raw PCM */
   unsigned int request_gen; /* Captured request_generation to detect superseded requests */
   int64_t conv_id;          /* Conversation this turn was captured for at enqueue; applied to
                              * session->stream_conversation_id AT DEQUEUE (a queued turn must
                              * not disturb the running turn's binding). */
} audio_work_t;

/**
 * @brief Sleep-based TTS pacing to prevent burst delivery
 *
 * Called after each sentence's audio is queued. Tracks cumulative audio duration
 * sent versus wall-clock elapsed time. When cumulative audio exceeds
 * elapsed + lookahead window, sleeps the difference on the LLM worker thread.
 *
 * The first sentence is never paced (sent immediately to minimize time-to-first-playback).
 * Sleep is chunked to allow prompt exit on disconnect or new request cancellation.
 *
 * @note This function sleeps on the LLM worker thread, extending its occupancy from
 *       TTS-synthesis-time (~5s for a 30s response) to approximately real-time (~30s).
 *       With the default worker pool size of 4, this means 4 concurrent TTS-active
 *       sessions could fully saturate the pool. If concurrent TTS load increases,
 *       consider adding a cumulative pacing budget per response or a pool-utilization
 *       check that skips pacing when most workers are busy.
 *
 * @note Opus frame count slightly overstates real audio duration due to zero-padding
 *       of the last partial frame (max 19ms per sentence). The 1s lookahead absorbs this.
 */
static void webui_tts_pace_after_send(ws_connection_t *conn,
                                      session_t *session,
                                      uint64_t audio_duration_us) {
   /* Bail if client already disconnected (avoid writing to stale conn) */
   if (session->disconnected)
      return;

   conn->tts_audio_sent_us += audio_duration_us;

   /* First sentence: set start time, don't sleep (minimize time-to-first-playback) */
   if (conn->tts_pace_start_us == 0) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      conn->tts_pace_start_us = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
      return;
   }

   /* How far ahead of real-time are we? */
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
   uint64_t elapsed_us = now_us - conn->tts_pace_start_us;

   if (conn->tts_audio_sent_us <= elapsed_us + WEBUI_TTS_PACE_LOOKAHEAD_US)
      return; /* Not ahead enough to need pacing */

   uint64_t sleep_us = conn->tts_audio_sent_us - elapsed_us - WEBUI_TTS_PACE_LOOKAHEAD_US;

   if (sleep_us < WEBUI_TTS_PACE_SLEEP_MIN_US)
      return; /* Skip tiny sleeps */

   /* Clamp to safety cap (don't skip — still pace, just bounded) */
   if (sleep_us > WEBUI_TTS_PACE_SLEEP_MAX_US)
      sleep_us = WEBUI_TTS_PACE_SLEEP_MAX_US;

   OLOG_INFO("WebUI: TTS pacing sleep %llums (audio_sent=%llums, elapsed=%llums)",
             (unsigned long long)(sleep_us / 1000),
             (unsigned long long)(conn->tts_audio_sent_us / 1000),
             (unsigned long long)(elapsed_us / 1000));

   /* Sleep in chunks, checking for disconnect and cancellation between each */
   while (sleep_us > 0 && !session->disconnected &&
          atomic_load(&session->current_stream_id) == conn->tts_pace_stream_id) {
      uint64_t chunk = sleep_us < WEBUI_TTS_PACE_CHECK_US ? sleep_us : WEBUI_TTS_PACE_CHECK_US;
      usleep((useconds_t)chunk);
      sleep_us -= chunk;
   }
}

/* Clean one sentence for TTS synthesis: strip residual command/end-of-turn + memory-citation
 * tags, drop '*' and emojis, trim trailing whitespace.  Returns a malloc'd string (possibly
 * empty for an all-markup sentence — caller checks length), or NULL on alloc failure.  Shared
 * by the single-target and multi-target (§Phase-4 fanout) callbacks so the strip rules can't
 * drift between them. */
static char *webui_clean_sentence_for_tts(const char *sentence) {
   /* Fast path: skip 4 string scans if sentence has no special content */
   bool needs_cleaning = (strstr(sentence, "<command>") != NULL ||
                          strstr(sentence, "<end_of_turn>") != NULL ||
                          strstr(sentence, "<cited>") != NULL || strchr(sentence, '*') != NULL);

   char *cleaned = strdup(sentence);
   if (!cleaned)
      return NULL;

   if (needs_cleaning) {
      /* Defensively strip residual command/end-of-turn tags + the memory-citation tag from
       * spoken text — shared whole-string strips (the text stream path strips <cited>
       * separately via the streaming text_filter_cited_tags). */
      text_filter_command_strip(cleaned, true); /* per-sentence TTS: drop a mid-split tag */
      text_filter_cited_strip(cleaned);
      remove_chars(cleaned, "*");
   }
   remove_emojis(cleaned);

   /* Trim trailing whitespace */
   size_t len = strlen(cleaned);
   while (len > 0 && (cleaned[len - 1] == ' ' || cleaned[len - 1] == '\t' ||
                      cleaned[len - 1] == '\n' || cleaned[len - 1] == '\r')) {
      cleaned[--len] = '\0';
   }
   return cleaned;
}

/**
 * @brief Sentence callback for real-time TTS audio streaming
 *
 * Called for each complete sentence during LLM response streaming.
 * Generates TTS and sends audio immediately, enabling sentence-by-sentence
 * playback instead of waiting for the full response.
 *
 * Single-target: audio goes to this session's own connection.  Used by reinvoke (the §6d
 * picker retargets this callback's userdata) and Tier-2 satellites (self-target).  The
 * normal text/voice workers use webui_sentence_audio_fanout_callback (§Phase-4) instead.
 */
void webui_sentence_audio_callback(const char *sentence, void *userdata) {
   session_t *session = (session_t *)userdata;

   if (!sentence || strlen(sentence) == 0 || !session || session->disconnected) {
      return;
   }

   /* Snapshot client_data ONCE to avoid TOCTOU race with LWS disconnect handler
    * (which sets client_data = NULL from the LWS thread while we run on the worker thread) */
   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   if (!conn || !conn->tts_enabled) {
      return;
   }

   /* Reset pacing state when a new LLM response stream starts */
   uint32_t stream_id = atomic_load(&session->current_stream_id);
   if (stream_id != conn->tts_pace_stream_id) {
      conn->tts_pace_start_us = 0;
      conn->tts_audio_sent_us = 0;
      conn->tts_pace_stream_id = stream_id;
   }

   char *cleaned = webui_clean_sentence_for_tts(sentence);
   if (!cleaned)
      return;
   size_t len = strlen(cleaned);

   /* Re-check disconnected (client_data snapshot from above is still valid for field reads
    * since the ws_connection_t is owned by lws and only freed after the service loop exits) */
   if (session->disconnected) {
      free(cleaned);
      return;
   }

   /* Only generate TTS if there's actual content and TTS still enabled */
   if (len > 0 && conn->tts_enabled) {
      /* Switch to "speaking" state when first audio is ready */
      webui_send_state(session, "speaking");

      /* Check if client supports Opus codec (conn already validated above) */
      bool use_opus = conn->use_opus;

      OLOG_INFO("WebUI: TTS streaming sentence (%s): %.60s%s", use_opus ? "opus" : "pcm", cleaned,
                len > 60 ? "..." : "");

      if (use_opus) {
         /* Encode TTS output as Opus for bandwidth savings */
         uint8_t *opus = NULL;
         size_t opus_len = 0;
         size_t frame_count = 0;
         int ret = webui_audio_text_to_opus(cleaned, &opus, &opus_len, &frame_count);

         if (ret == WEBUI_AUDIO_SUCCESS && opus && opus_len > 0) {
            /* Re-check: session may have disconnected during TTS synthesis.
             * Use snapshotted conn (not session->client_data) to avoid TOCTOU race. */
            if (!session->disconnected && conn->tts_enabled) {
               webui_send_audio(session, opus, opus_len);
               webui_send_audio_end(session, true);
               webui_tts_pace_after_send(conn, session,
                                         (uint64_t)frame_count * WEBUI_OPUS_FRAME_MS * 1000ULL);
            }
            free(opus);
         }
      } else if (session->type == SESSION_TYPE_DAP2 && session->tier == DAP2_TIER_2) {
         /* Tier 2 satellite: send TTS at native rate (22050Hz) — skip 48kHz resample.
          * The ESP32 resamples locally, halving data over the wire and queue pressure. */
         int16_t *tts_pcm = NULL;
         size_t tts_samples = 0;
         uint32_t tts_rate = 0;
         int ret = text_to_speech_to_pcm(cleaned, &tts_pcm, &tts_samples, &tts_rate);

         if (ret == 0 && tts_pcm && tts_samples > 0) {
            OLOG_INFO("WebUI audio: Tier 2 TTS %zu samples at %uHz (native, no resample)",
                      tts_samples, tts_rate);
            /* Re-check: session may have disconnected during TTS synthesis.
             * Use snapshotted conn (not session->client_data) to avoid TOCTOU race. */
            if (!session->disconnected && conn->tts_enabled) {
               size_t bytes = tts_samples * sizeof(int16_t);
               webui_send_audio(session, (const uint8_t *)tts_pcm, bytes);
               webui_send_audio_end(session, false);
            }
            free(tts_pcm);
         }
      } else {
         /* WebUI browser (no Opus): send 48kHz PCM matching browser's playback rate */
         int16_t *pcm = NULL;
         size_t samples = 0;
         int ret = webui_audio_text_to_pcm(cleaned, &pcm, &samples);

         if (ret == WEBUI_AUDIO_SUCCESS && pcm && samples > 0) {
            /* Re-check: session may have disconnected during TTS synthesis.
             * Use snapshotted conn (not session->client_data) to avoid TOCTOU race. */
            if (!session->disconnected && conn->tts_enabled) {
               size_t bytes = samples * sizeof(int16_t);
               webui_send_audio(session, (const uint8_t *)pcm, bytes);
               webui_send_audio_end(session, false);
               webui_tts_pace_after_send(conn, session,
                                         (samples * 1000000ULL) / WEBUI_OPUS_SAMPLE_RATE);
            }
            free(pcm);
         }
      }
   }

   free(cleaned);
}

/* =============================================================================
 * Multi-target TTS fan-out (SERVER_AUTHORITATIVE_PERSISTENCE §Phase-4)
 *
 * A text- or voice-originated turn's synthesized speech is heard by every
 * speaker-capable viewer of the conversation, not only the origin connection.
 * Synthesis (text_to_speech_to_pcm, global tts_mutex) runs ONCE per sentence
 * regardless of recipient count; the 22k->48k resample runs once and the Opus
 * encode once, shared across all recipients of each format.  The origin is a
 * recipient iff its own TTS is on (opposite cell from the §Phase-3 tool-step
 * fan, which EXCLUDES the origin + is browsers-only).
 *
 * v1 SCOPE: browser-to-browser.  A non-origin Tier-2 (server-TTS) satellite is
 * NOT reached — webui_get_active_conversation_id() returns 0 for non-WEBUI
 * sessions, so a satellite has no per-conversation membership signal to match
 * against yet.  Cross-device fan to satellites is a deferred follow-up (needs a
 * satellite active-conversation binding).  A satellite that ORIGINATES a turn is
 * unaffected — it speaks via the single-target callback.
 * ============================================================================= */

/* Codec branch for one browser target. */
typedef enum {
   TTS_FMT_OPUS,  /* Opus browser */
   TTS_FMT_PCM48K /* Browser without Opus: 48kHz PCM */
} tts_target_fmt_t;

/* Shared membership predicate (arch HIGH-1: ONE source of truth for both the arm-time existence
 * scan and the per-sentence collect+send).  user_id filtering is owned by for_each_user_conn;
 * this checks only the audio-specific parts.  The origin is a target unconditionally when its TTS
 * is on — matching the pre-Phase-4 single-target behavior that the origin hears its own reply
 * regardless of which conversation it is currently viewing.  Non-origin targets must be a WEBUI
 * browser viewing THIS conversation (see the v1-scope note above re: satellites). */
static bool conn_is_audio_target(ws_connection_t *conn,
                                 int64_t conv_id,
                                 uint32_t origin_session_id) {
   if (!conn->wsi || !conn->tts_enabled)
      return false;
   session_t *s = conn->session;
   if (s->type != SESSION_TYPE_WEBUI)
      return false;
   if (s->session_id == origin_session_id)
      return true; /* origin: unconditional if TTS on */
   return webui_get_active_conversation_id(s) == conv_id;
}

static tts_target_fmt_t conn_target_fmt(ws_connection_t *conn) {
   return conn->use_opus ? TTS_FMT_OPUS : TTS_FMT_PCM48K;
}

/* Derive the fan target for a turn originating on @origin: the authenticated user and the turn's
 * conversation (stream id, else the live active-conv fallback).  The per-sentence send fan and the
 * turn-end idle fan MUST resolve @conv identically — they open and close the speaking/idle bracket
 * on the same recipient set — so this derivation lives in exactly ONE place (arch HIGH-1 sibling).
 * Returns false (no fan) when there is no authenticated user.  @ocon_out is optional (the send fan
 * needs it for pacing; the idle fan does not). */
static bool webui_origin_fan_target(session_t *origin,
                                    ws_connection_t **ocon_out,
                                    int *user_id_out,
                                    int64_t *conv_out) {
   ws_connection_t *ocon = (ws_connection_t *)origin->client_data;
   int user_id = ocon ? ocon->auth_user_id : (int)origin->metrics.user_id;
   if (user_id <= 0)
      return false; /* unauthenticated — never fan across all users (for_each_user_conn's <=0=all)
                     */
   int64_t conv = origin->stream_conversation_id;
   if (conv <= 0 && ocon && ocon->active_conversation_id > 0)
      conv = ocon->active_conversation_id;
   if (ocon_out)
      *ocon_out = ocon;
   *user_id_out = user_id;
   *conv_out = conv;
   return true;
}

/* Phase-1 scan: which formats does the live recipient set need (so we encode each at most once). */
typedef struct {
   int64_t conv_id;
   uint32_t origin_session_id;
   bool need_opus;
   bool need_pcm48;
   int count;
} tts_scan_ctx_t;

static bool visit_tts_scan(ws_connection_t *conn, void *vctx) {
   tts_scan_ctx_t *ctx = (tts_scan_ctx_t *)vctx;
   if (!conn_is_audio_target(conn, ctx->conv_id, ctx->origin_session_id))
      return true;
   ctx->count++;
   if (conn_target_fmt(conn) == TTS_FMT_OPUS)
      ctx->need_opus = true;
   else
      ctx->need_pcm48 = true;
   return true; /* visit all */
}

/* Phase-2 send: fan the pre-encoded buffers under s_conn_registry_mutex, re-validating each
 * recipient against the LIVE registry (a session that vanished during synth is simply not
 * visited).  Queuing under the registry lock is what makes this UAF-free — session destroy
 * purges the response queue under the same lock (master-plan R1). */
typedef struct {
   int64_t conv_id;
   uint32_t origin_session_id;
   const uint8_t *opus;
   size_t opus_len;
   const int16_t *pcm48;
   size_t pcm48_samples;
} tts_send_ctx_t;

static bool visit_tts_send(ws_connection_t *conn, void *vctx) {
   tts_send_ctx_t *ctx = (tts_send_ctx_t *)vctx;
   if (!conn_is_audio_target(conn, ctx->conv_id, ctx->origin_session_id))
      return true;
   session_t *s = conn->session;
   /* Light state frame only (webui_send_state additionally does session_get_llm_config +
    * llm_context_get_usage per call — per-session + module locks we must NOT nest under the
    * registry lock).  The context% is turn-static, so per-sentence metrics added nothing. */
   webui_send_state_with_detail(s, "speaking", NULL);
   if (conn_target_fmt(conn) == TTS_FMT_OPUS) {
      if (ctx->opus && ctx->opus_len > 0) {
         webui_send_audio(s, ctx->opus, ctx->opus_len);
         webui_send_audio_end(s, true);
      }
   } else {
      if (ctx->pcm48 && ctx->pcm48_samples > 0) {
         webui_send_audio(s, (const uint8_t *)ctx->pcm48, ctx->pcm48_samples * sizeof(int16_t));
         webui_send_audio_end(s, false);
      }
   }
   return true;
}

/* Arm-time existence check: is there a NON-origin speaker-capable viewer of this conversation?
 * The origin's own TTS flag is checked separately by the caller (arch HIGH-2: origin.tts_enabled
 * must stay a sufficient arm condition, independent of the mid-turn voice conv-bind timing). */
bool webui_audio_has_other_speaker(int user_id, int64_t conv_id, uint32_t origin_session_id) {
   if (user_id <= 0)
      return false;
   tts_scan_ctx_t scan = { .conv_id = conv_id, .origin_session_id = origin_session_id };
   for_each_user_conn(user_id, origin_session_id, visit_tts_scan, &scan);
   return scan.count > 0;
}

/* Turn-end "idle" fan: close the state bracket on every NON-origin recipient that got the
 * per-sentence "speaking" (WWFD: bracket-recipients == audio-recipients).  Aurora/www drive
 * their speaking presentation off state, not audio_end, so without this a bystander's reactor
 * stays "speaking" after the reply.  The origin gets its own "idle" via the worker's normal
 * webui_send_state(session,"idle"); origin is excluded here to avoid a double-idle. */
typedef struct {
   int64_t conv_id;
   uint32_t origin_session_id;
} tts_idle_ctx_t;

static bool visit_tts_idle(ws_connection_t *conn, void *vctx) {
   tts_idle_ctx_t *ctx = (tts_idle_ctx_t *)vctx;
   if (conn_is_audio_target(conn, ctx->conv_id, ctx->origin_session_id))
      webui_send_state_with_detail(conn->session, "idle", NULL);
   return true;
}

void webui_fanout_tts_idle(session_t *origin) {
   if (!origin)
      return;
   int user_id = 0;
   int64_t conv = 0;
   if (!webui_origin_fan_target(origin, NULL, &user_id, &conv))
      return;
   tts_idle_ctx_t ctx = { .conv_id = conv, .origin_session_id = origin->session_id };
   for_each_user_conn(user_id, origin->session_id, visit_tts_idle, &ctx);
}

/* Multi-target sentence callback: synth once, encode per format, fan to all speaker viewers.
 * userdata is the ORIGIN session (same shape as the single-target callback). */
void webui_sentence_audio_fanout_callback(const char *sentence, void *userdata) {
   session_t *origin = (session_t *)userdata;
   /* Do NOT abort on origin->disconnected: the turn survives a mid-turn origin drop (the worker
    * holds a turn ref, so `origin` stays valid), and other speaker-capable viewers should keep
    * hearing the rest of the reply. */
   if (!sentence || sentence[0] == '\0' || !origin)
      return;

   ws_connection_t *ocon = NULL;
   int user_id = 0;
   int64_t conv = 0;
   if (!webui_origin_fan_target(origin, &ocon, &user_id, &conv))
      return;
   uint32_t origin_sid = origin->session_id;

   /* Worker-local pacing reset when a new response stream starts (origin conn holds the pacer
    * fields, mirroring the single-target callback). */
   uint32_t stream_id = atomic_load(&origin->current_stream_id);
   if (ocon && stream_id != ocon->tts_pace_stream_id) {
      ocon->tts_pace_start_us = 0;
      ocon->tts_audio_sent_us = 0;
      ocon->tts_pace_stream_id = stream_id;
   }

   char *cleaned = webui_clean_sentence_for_tts(sentence);
   if (!cleaned)
      return;
   if (cleaned[0] == '\0') {
      free(cleaned);
      return;
   }

   /* Phase 1: which formats does the current recipient set need? */
   tts_scan_ctx_t scan = { .conv_id = conv, .origin_session_id = origin_sid };
   for_each_user_conn(user_id, 0, visit_tts_scan, &scan);
   if (scan.count == 0) {
      free(cleaned); /* no speaker-capable viewer this sentence — synthesize nothing */
      return;
   }

   /* Synthesize ONCE (the expensive, tts_mutex-serialized step) regardless of recipient count. */
   int16_t *pcm22k = NULL;
   size_t samples = 0;
   uint32_t rate = 0;
   if (text_to_speech_to_pcm(cleaned, &pcm22k, &samples, &rate) != 0 || !pcm22k || samples == 0) {
      OLOG_ERROR("WebUI audio: multi-target TTS synth failed");
      free(cleaned);
      return;
   }

   /* Resample 22k->48k ONCE — both browser formats share it: Opus encodes from the 48k buffer,
    * PCM recipients send it directly.  Encode Opus from that same buffer only if a recipient
    * needs it (so a mixed opus+pcm conversation pays one resample + one encode, not two). */
   int16_t *pcm48 = NULL;
   size_t pcm48_n = 0;
   uint8_t *opus = NULL;
   size_t opus_len = 0, opus_frames = 0;
   if (scan.need_opus || scan.need_pcm48) {
      if (webui_audio_pcm22k_to_pcm48k(pcm22k, samples, &pcm48, &pcm48_n) == WEBUI_AUDIO_SUCCESS &&
          scan.need_opus) {
         webui_opus_encode_stream(pcm48, pcm48_n, &opus, &opus_len, &opus_frames);
      }
   }

   /* Phase 2: fan under the registry lock (UAF-safe re-validation — master-plan R1). */
   tts_send_ctx_t send = { .conv_id = conv,
                           .origin_session_id = origin_sid,
                           .opus = opus,
                           .opus_len = opus_len,
                           .pcm48 = pcm48,
                           .pcm48_samples = pcm48_n };
   for_each_user_conn(user_id, 0, visit_tts_send, &send);

   /* Pace the worker ONCE per sentence (not per recipient), off the origin conn's clock. */
   if (ocon)
      webui_tts_pace_after_send(ocon, origin,
                                (uint64_t)samples * 1000000ULL / (rate ? rate : 22050));

   free(opus);
   free(pcm48);
   free(pcm22k);
   free(cleaned);
}

/* Per-sentence TTS dispatch for scheduler/briefing audio.  Mirrors the
 * inline-LLM-streaming callback (webui_sentence_audio_callback) but
 * deliberately bypasses the tts_enabled gate — scheduler notifications
 * are operator-scheduled actions whose audio routing was already
 * decided by briefing_should_speak() before we got here.  Pacing uses
 * the same conn->tts_pace_* fields the inline path uses; we reset
 * them at the top of scheduler_send_tts_to_session so a briefing fired
 * during a recent LLM stream doesn't share that stream's elapsed clock. */
typedef struct {
   session_t *session;
   ws_connection_t *conn;
} scheduler_tts_ctx_t;

static void scheduler_tts_sentence_callback(const char *sentence, void *userdata) {
   scheduler_tts_ctx_t *ctx = (scheduler_tts_ctx_t *)userdata;
   if (!ctx || !ctx->session || ctx->session->disconnected)
      return;
   if (!sentence || !sentence[0])
      return;

   session_t *session = ctx->session;
   ws_connection_t *conn = ctx->conn;

   if (conn->use_opus) {
      uint8_t *opus = NULL;
      size_t opus_len = 0;
      size_t frame_count = 0;
      int ret = webui_audio_text_to_opus(sentence, &opus, &opus_len, &frame_count);
      if (ret == WEBUI_AUDIO_SUCCESS && opus && opus_len > 0) {
         if (!session->disconnected) {
            webui_send_audio(session, opus, opus_len);
            webui_send_audio_end(session, true);
            webui_tts_pace_after_send(conn, session,
                                      (uint64_t)frame_count * WEBUI_OPUS_FRAME_MS * 1000ULL);
         }
         free(opus);
      }
   } else {
      int16_t *pcm = NULL;
      size_t samples = 0;
      int ret = webui_audio_text_to_pcm(sentence, &pcm, &samples);
      if (ret == WEBUI_AUDIO_SUCCESS && pcm && samples > 0) {
         if (!session->disconnected) {
            size_t bytes = samples * sizeof(int16_t);
            webui_send_audio(session, (const uint8_t *)pcm, bytes);
            webui_send_audio_end(session, false);
            webui_tts_pace_after_send(conn, session,
                                      (samples * 1000000ULL) / WEBUI_OPUS_SAMPLE_RATE);
         }
         free(pcm);
      }
   }
}

/**
 * Send TTS audio for a scheduler notification to a specific WebUI session.
 *
 * Streams sentence-by-sentence to match the inline-LLM-TTS path instead
 * of TTSing + Opus-encoding the entire briefing as one segment.  The
 * single-segment shape ran into a documented opus-worker desync at ~12s
 * into long-briefing (>60s) audio — the browser's frame-length validator
 * fired mid-stream, truncating the rest.  Per-sentence segments cap the
 * worst case at one sentence (~5-15s) AND let playback start ~1-2s after
 * fire instead of waiting ~20s for the full encode (Jetson at RTF 0.12
 * for a 77s briefing).  Pacing through conn->tts_pace_* throttles the
 * response queue on chatty multi-paragraph summaries.
 *
 * Bypasses the tts_enabled check (scheduler notifications are unsolicited)
 * and brackets the audio with speaking/idle state transitions.
 *
 * The opus-worker root cause is filed separately in docs/TODO.md
 * ("long-briefing TTS truncation"); this fix makes the pipeline robust
 * to it regardless of where the corruption originates — even if a single
 * sentence's segment hits the bug, the briefing degrades to "missing one
 * sentence" instead of "truncated mid-summary, rest silently lost."
 */
void scheduler_send_tts_to_session(session_t *session, const char *text) {
   if (!session || !text || !text[0] || session->disconnected)
      return;

   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   if (!conn)
      return;

   OLOG_INFO("WebUI: Scheduler TTS to session %u (%s, sentence-streaming): %.60s%s",
             session->session_id, conn->use_opus ? "opus" : "pcm", text,
             strlen(text) > 60 ? "..." : "");

   webui_send_state(session, "speaking");

   /* Reset pacing state for the briefing burst.  The inline-LLM-stream
    * pace_start/sent fields may carry leftover values from a recent
    * conversation; treat the briefing as a fresh stream so the first
    * sentence starts playing immediately and subsequent sentences pace
    * against the briefing's own audio clock. */
   conn->tts_pace_start_us = 0;
   conn->tts_audio_sent_us = 0;
   conn->tts_pace_stream_id = atomic_load(&session->current_stream_id);

   scheduler_tts_ctx_t ctx = { .session = session, .conn = conn };
   sentence_buffer_t *sb = sentence_buffer_create(scheduler_tts_sentence_callback, &ctx);
   if (sb) {
      sentence_buffer_feed(sb, text);
      sentence_buffer_flush(sb);
      sentence_buffer_free(sb);
   } else {
      /* sentence_buffer alloc failure — fall back to one-shot send so the
       * briefing still plays (degrades to the old single-segment behavior
       * which may truncate on >60s briefings, but at least the user gets
       * SOMETHING).  Same TTS+send shape as the per-sentence callback
       * above, but operates on the entire text in one pass. */
      OLOG_WARNING("WebUI: sentence_buffer alloc failed for briefing; "
                   "falling back to single-segment send (may truncate on >60s briefings)");
      scheduler_tts_sentence_callback(text, &ctx);
   }

   webui_send_state(session, "idle");
}

/* REQUEST_SUPERSEDED macro now defined in webui_internal.h */

/**
 * @brief Audio worker thread - process voice input and generate response
 *
 * Uses sentence-by-sentence TTS streaming: audio is generated and sent as each
 * sentence completes during the LLM response, rather than waiting for the full
 * response. This significantly reduces perceived latency.
 */
/* Balanced exit for the audio worker once it has passed its supersede check and
 * incremented turn_in_flight: clear the in-flight guard, THEN release the ref
 * (decrement-before-release, so a concurrent session_destroy that this release
 * unblocks already reads turn_in_flight == 0).  Mirror of text_worker_end. */
static void audio_worker_end(session_t *session) {
   if (session) {
      /* Close the multi-target TTS bracket on non-origin listeners (§Phase-4) BEFORE releasing
       * the turn ref — single funnel for every voice-worker exit; no-op when nothing was fanned. */
      webui_fanout_tts_idle(session);
      atomic_fetch_sub(&session->turn_in_flight, 1);
      session_release(session);
   }
}

static void *audio_worker_thread(void *arg) {
   audio_work_t *work = (audio_work_t *)arg;
   session_t *session = work->session;
   uint8_t *audio_data = work->audio_data;
   size_t audio_len = work->audio_len;
   unsigned int expected_gen = work->request_gen;

   if (!session || REQUEST_SUPERSEDED(session, expected_gen)) {
      OLOG_INFO("WebUI: Audio session disconnected or request superseded, aborting");
      if (session) {
         session_release(session); /* pre-increment exit: bare release, no turn_in_flight */
      }
      free(audio_data);
      free(work);
      return NULL;
   }

   /* Passed the supersede check — this turn is now in flight.  Guards
    * session_cleanup_expired from reaping a still-generating voice turn and makes
    * conv_has_turn_in_flight see it (parity with text_worker_thread).  Every exit
    * BELOW this point releases via audio_worker_end (decrement-then-release). */
   atomic_fetch_add(&session->turn_in_flight, 1);

   OLOG_INFO("WebUI: Processing audio for session %u (%zu bytes, %s)", session->session_id,
             audio_len, work->use_opus ? "opus" : "pcm");

   /* Send "listening" state (ASR in progress) */
   webui_send_state(session, "listening");

   /* Transcribe audio */
   char *transcript = NULL;
   int ret;

   if (work->use_opus) {
      /* Decode Opus (48kHz) and resample to 16kHz for ASR */
      ret = webui_audio_opus_to_text(audio_data, audio_len, &transcript);
      free(audio_data);
      work->audio_data = NULL;
   } else if (session->type == SESSION_TYPE_DAP2 && session->tier == DAP2_TIER_2) {
      /* Tier 2 satellite: raw PCM is already 16kHz — transcribe directly */

      /* Validate length is a multiple of 2 (16-bit samples) */
      if (audio_len % sizeof(int16_t) != 0) {
         OLOG_WARNING("WebUI: Tier 2 audio length %zu is not a multiple of 2", audio_len);
         webui_send_error(session, "INVALID_AUDIO", "Audio length must be a multiple of 2 bytes");
         webui_send_state(session, "idle");
         audio_worker_end(session);
         free(audio_data);
         free(work);
         return NULL;
      }

      size_t pcm_samples = audio_len / sizeof(int16_t);

      /* Enforce maximum duration to prevent DoS (30 seconds at 16kHz) */
      const size_t max_samples_16k = (size_t)WEBUI_MAX_RECORDING_SECONDS * 16000;
      if (pcm_samples > max_samples_16k) {
         OLOG_WARNING("WebUI: Tier 2 audio too long: %zu samples (max %zu)", pcm_samples,
                      max_samples_16k);
         webui_send_error(session, "AUDIO_TOO_LONG", "Recording exceeds 30 second limit");
         webui_send_state(session, "idle");
         audio_worker_end(session);
         free(audio_data);
         free(work);
         return NULL;
      }

      OLOG_INFO("WebUI: Tier 2 audio: %zu samples at 16kHz (no resample needed)", pcm_samples);
      ret = webui_audio_transcribe((const int16_t *)audio_data, pcm_samples, &transcript);
      free(audio_data);
      work->audio_data = NULL;
   } else {
      /* WebUI browser: raw PCM is 48kHz — resample to 16kHz for ASR */
      size_t pcm_samples = audio_len / sizeof(int16_t);
      ret = webui_audio_pcm48k_to_text((const int16_t *)audio_data, pcm_samples, &transcript);
      free(audio_data);
      work->audio_data = NULL;
   }

   /* asr_transcript_is_blank() subsumes NULL/empty/whitespace-only and also drops
    * a Whisper silence marker ("[BLANK_AUDIO]") — a *successful*, non-empty result
    * that must not reach the LLM (user pressed the button but nothing was heard). */
   if (ret != WEBUI_AUDIO_SUCCESS || asr_transcript_is_blank(transcript)) {
      OLOG_WARNING("WebUI: Audio transcription failed, empty, or blank/silence");
      webui_send_error(session, "ASR_FAILED", "Could not understand audio");
      webui_send_state(session, "idle");
      audio_worker_end(session);
      free(transcript);
      free(work);
      return NULL;
   }

   OLOG_INFO("WebUI: Transcribed: \"%s\"", transcript);

   /* Check if request was superseded */
   if (REQUEST_SUPERSEDED(session, expected_gen)) {
      audio_worker_end(session);
      free(transcript);
      free(work);
      return NULL;
   }

   /* Cross-device dedup: drop this utterance if the local mic or another device
    * already produced a command event within the window (same spoken command). */
   if (utterance_dedup_check(session->session_id)) {
      OLOG_INFO("WebUI: Dedup suppressed duplicate utterance for session %u: \"%s\"",
                session->session_id, transcript);
      webui_send_state(session, "idle");
      audio_worker_end(session);
      free(transcript);
      free(work);
      return NULL;
   }

   /* Add user message to session history immediately (before LLM call can be cancelled) */
   session_add_message(session, "user", transcript);

   /* Persist to conversation DB immediately (prevents race with client reload) */
   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   /* Voice turn: server-dispatched, so the browser never ran its conversation
    * pre-create.  Bind (lazily creating) a conversation so this transcript AND the
    * reply persist instead of evaporating on reload.  The assistant/tool rows
    * persist off session->stream_conversation_id (set from the enqueue-captured,
    * possibly 0, conv at dequeue with no active-conversation fallback), so point it
    * at the freshly-bound conversation too. */
   if (conn && conn->active_conversation_id <= 0 &&
       webui_voice_transcript_substantive(transcript)) {
      int64_t bound = webui_ensure_active_conversation(conn, transcript);
      if (bound > 0) {
         atomic_store(&session->stream_conversation_id, bound);
      }
   }
   bool saved_to_db = false;
   int64_t user_msg_id = 0;
   if (conn && conn->active_conversation_id > 0) {
      if (conv_db_add_message_ex(conn->active_conversation_id, conn->auth_user_id, "user",
                                 transcript, &user_msg_id) == AUTH_DB_SUCCESS) {
         saved_to_db = true;
         session_stamp_last_message_id(session, "user", user_msg_id);
      } else {
         user_msg_id = 0;
      }
   }

   /* Echo transcription as user message, carrying the DB row id so the origin
    * stamps data-message-id + dedups its own fan-out copy (server_saved also
    * prevents duplicate client save). */
   webui_send_transcript_ex(session, "user", transcript, saved_to_db, user_msg_id);
   /* Fan the user message out to every OTHER viewer of this conversation (§12c).
    * INVARIANT (mirrors webui_text_processing.c): guard on user_msg_id > 0 so a save
    * failure (id 0) emits ONLY the echo, never a second frame the client can't dedup —
    * a spoken turn has no optimistic bubble, so an id-0 fan-out would double it. */
   if (user_msg_id > 0 && conn && conn->auth_user_id > 0 && conn->active_conversation_id > 0) {
      conv_event_notify_message_appended(conn->active_conversation_id, conn->auth_user_id,
                                         user_msg_id, "user", transcript, NULL, 0);
   }

   /* This turn's input is ASR-transcribed (voice) — flag it before dispatch so
    * the prompt builder injects the ASR-disambiguation hint for this turn.
    * Correctness relies on per-session turn serialization (one dispatch in flight
    * per session); atomic_bool guards visibility, not logical interleave. */
   session->input_was_voice = true;

   /* Refresh events_observable for THIS turn.  The voice worker calls the LLM directly
    * (session_llm_call_with_tts_vision_no_add below) and never passes through
    * core_text_input_dispatch, which is the ONLY other writer of this flag (it "sets every
    * dispatch to reflect THIS turn", text_input_dispatch.c).  Without this the flag is stale
    * from a prior turn on this session: a preceding turn into a job conversation leaves it
    * true, so a normal voice turn's tool steps would wrongly take the durable conv_event
    * persist path (write-amp) instead of the ephemeral cross-viewer fan.  A push-to-talk voice
    * turn is never a job/background turn, so the correct value is always false here. */
   atomic_store(&session->events_observable, false);

   /* Phase 1e: per-turn focus injection.  Synchronous; runs on this
    * audio_worker_thread (spawned via pthread_create — NEVER on the
    * lws service thread).  Uses the post-ASR transcript as the turn
    * text so the focus block reflects what the user actually said. */
   session_dispatch_user_turn(session, transcript);

   /* Send "thinking" state while LLM processes - streaming callback will switch to "speaking" */
   webui_send_state_with_detail(session, "thinking", "Processing request...");

   /* Disconnect-safe captures (SERVER_AUTHORITATIVE Phase 2b-ii, correctness H1): a voice
    * turn survives a mid-turn client disconnect (turn_in_flight held), after which
    * libwebsockets frees `conn`.  The post-dispatch persist + audio_end MUST NOT deref
    * conn — capture everything now, use only the locals in the tail.  turn_conv reads
    * stream_conversation_id AFTER the lazy bind above. */
   int64_t turn_conv = atomic_load(&session->stream_conversation_id);
   int turn_user_id = conn ? conn->auth_user_id : (int)session->metrics.user_id;
   bool use_opus = conn ? atomic_load(&conn->use_opus) : false;

   /* Clear any stale visual stranded by a prior errored/cancelled turn (master-R5): once
    * the server APPENDS pending_visual, a leftover would attach to THIS turn's row.  The
    * text worker does the same at turn start; the voice worker never did.  Under tools_mutex
    * like every other pending_visual access — the render_visual callback writes it from a
    * pool thread, so the invariant must hold even though turn serialization makes a race
    * impossible at this exact point today. */
   pthread_mutex_lock(&session->tools_mutex);
   free(session->pending_visual);
   session->pending_visual = NULL;
   pthread_mutex_unlock(&session->tools_mutex);

   /* Call LLM with TTS streaming - audio is generated and sent per-sentence
    * No vision images for voice input (pass NULL for vision params).
    * Arm the Model A promise so the final stream_end stands the browser down from its
    * client-save; the server persists the reply in the tail (the path the voice worker
    * previously lacked entirely). */
   atomic_store(&session->will_persist_turn, true);
   /* Multi-target TTS (§Phase-4): fan the voice reply to every speaker-capable viewer of this
    * conversation, the origin voice device included.  Self-gates when no listener exists (scan
    * finds none → no synth), so the unconditional wiring keeps today's no-audio-when-TTS-off case.
    */
   char *response = session_llm_call_with_tts_vision_no_add(session, transcript, NULL, NULL, NULL,
                                                            0, webui_sentence_audio_fanout_callback,
                                                            session);
   atomic_store(&session->will_persist_turn, false);
   free(transcript);

   bool superseded = REQUEST_SUPERSEDED(session, expected_gen);

   /* Resolve the reply to persist (G4, SERVER_AUTHORITATIVE §9): the in-hand response, or the
    * cancel-at-buzzer stash if a cancel freed a completed reply inside dispatch and returned
    * NULL.  Every exit below persists it (or a genuine failure) before returning. */
   char *reply = response;
   if (reply == NULL) {
      reply = session->cancelled_final_response;
      session->cancelled_final_response = NULL;
   }

   /* Close the TTS audio stream — use_opus captured PRE-dispatch (H1: conn may be freed now). */
   webui_send_audio_end(session, use_opus);

   if (reply != NULL && reply[0] != '\0') {
      /* Server-authoritative persist of the voice assistant reply — the path the voice worker
       * previously lacked entirely (it just free'd the reply, relying on the browser
       * client-save).  Persist even when superseded-but-complete: the browser stood down on
       * will_persist.  turn_conv/turn_user_id are the pre-dispatch captures — NEVER conn (H1).
       * A voice turn with no bound conversation (turn_conv == 0) streams ephemerally, same as
       * before — no row, and NO error frame (nothing failed). */
      if (turn_conv > 0 && turn_user_id > 0) {
         if (webui_persist_final_answer(session, turn_conv, turn_user_id, reply, NULL) !=
             AUTH_DB_SUCCESS) {
            OLOG_ERROR("WebUI: failed to persist voice reply to conv %lld after retries",
                       (long long)turn_conv);
            webui_send_error(session, "PERSIST_ERROR",
                             "Your reply was played but could not be saved.");
         }
      }

      /* Send context usage update to WebUI (only meaningful when a reply was produced). */
      int current_tokens, max_tokens;
      float threshold;
      llm_context_get_last_usage(&current_tokens, &max_tokens, &threshold);
      if (max_tokens > 0) {
         webui_send_context(session, current_tokens, max_tokens, threshold);
      }
   } else if (!superseded) {
      /* Genuine failure: no reply and not superseded. */
      OLOG_WARNING("WebUI: LLM call failed");
      webui_send_error(session, "LLM_ERROR", "Failed to get response");
   }

   webui_send_state(session, "idle");

   /* Mark interaction complete for conversation idle timeout tracking */
   session_update_interaction_complete(session);

   /* Balanced exit: clear turn_in_flight, then release the ref acquired before
    * thread creation. */
   audio_worker_end(session);

   free(reply); /* frees response OR the taken stash (mutually exclusive) */
   free(work);
   return NULL;
}

/* =============================================================================
 * Turn-queue integration (P1): serialize the push-to-talk voice turn.  The PTT
 * path is the second WebUI turn producer (alongside text input); routing it
 * through the queue keeps voice input from racing an in-flight text/background
 * turn on the same session.
 * ============================================================================= */

/* free_work closure: drop a queued audio turn WITHOUT running it (bound-reject
 * or purge).  Mirrors the run-path cleanup (release the session retain + free). */
static void audio_turn_free(void *work) {
   audio_work_t *w = (audio_work_t *)work;
   if (w == NULL) {
      return;
   }
   if (w->session != NULL) {
      session_release(w->session);
   }
   free(w->audio_data);
   free(w);
}

/* Worker entry for a queued audio turn.  Binds this turn's conversation + clears
 * the turn flags AT DEQUEUE (never at enqueue — a queued turn must not disturb
 * the running turn), runs the existing ASR+LLM turn body, then chains the next
 * queued turn for this session. */
static void *audio_turn_thread_entry(void *arg) {
   audio_work_t *work = (audio_work_t *)arg;
   uint32_t sid = 0;
   if (work != NULL && work->session != NULL) {
      sid = work->session->session_id;
      if (atomic_load(&work->session->being_destroyed)) {
         /* Session began teardown after this turn was popped-to-spawn (C1): do
          * NOT clear its flags or run — teardown's cancel must stand.  Clean up
          * and let the queue drain. */
         audio_turn_free(work);
         turn_queue_turn_done(sid);
         return NULL;
      }
      atomic_store(&work->session->stream_conversation_id, work->conv_id);
      session_begin_turn_flags(work->session); /* fresh flags for THIS turn (G2) */
   }
   audio_worker_thread(work); /* existing turn body — frees work, releases session */
   turn_queue_turn_done(sid); /* chain the next queued turn for this session */
   return NULL;
}

/* spawn closure: start the turn worker for a dequeued audio turn. */
static void audio_turn_spawn(void *work) {
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int ret = pthread_create(&thread, &attr, audio_turn_thread_entry, work);
   pthread_attr_destroy(&attr);
   if (ret != 0) {
      /* Spawn failed: free this turn + advance the queue so it can't wedge. */
      audio_work_t *w = (audio_work_t *)work;
      uint32_t sid = (w != NULL && w->session != NULL) ? w->session->session_id : 0;
      OLOG_ERROR("WebUI: failed to spawn queued audio turn worker; dropping it");
      audio_turn_free(work);
      turn_queue_turn_done(sid);
   }
}

/**
 * @brief Handle binary WebSocket message (audio data)
 *
 * Binary message format:
 * - Byte 0: Message type (WS_BIN_AUDIO_IN or WS_BIN_AUDIO_IN_END)
 * - Bytes 1+: Opus audio data (for AUDIO_IN) or empty (for AUDIO_IN_END)
 */
void handle_binary_message(ws_connection_t *conn, const uint8_t *data, size_t len) {
   if (len < 1) {
      OLOG_WARNING("WebUI: Empty binary message");
      return;
   }

   if (!conn->session) {
      OLOG_WARNING("WebUI: Binary message but no session");
      return;
   }

   uint8_t msg_type = data[0];
   const uint8_t *payload = data + 1;
   size_t payload_len = len - 1;

   /* Always-on routing: AUDIO_IN accumulates normally (handles fragmentation).
    * The WebSocket callback routes complete frames to always_on_process_audio
    * after fragment reassembly. AUDIO_IN_END is a no-op in always-on mode. */
   if (conn->always_on && always_on_get_state(conn->always_on) != ALWAYS_ON_DISABLED) {
      if (msg_type == WS_BIN_AUDIO_IN_END) {
         return; /* Ignore end markers in always-on mode */
      }
      /* AUDIO_IN: fall through to PTT accumulation for fragment handling */
   }

   switch (msg_type) {
      case WS_BIN_AUDIO_IN: {
         /* Accumulate audio data in connection buffer */
         if (payload_len == 0) {
            break;
         }

         /* Initialize buffer if needed */
         if (!conn->audio_buffer) {
            conn->audio_buffer_capacity = WEBUI_AUDIO_BUFFER_SIZE;
            conn->audio_buffer = malloc(conn->audio_buffer_capacity);
            if (!conn->audio_buffer) {
               OLOG_ERROR("WebUI: Failed to allocate audio buffer");
               send_error_impl(conn->wsi, "BUFFER_ERROR", "Audio buffer allocation failed");
               return;
            }
            conn->audio_buffer_len = 0;
         }

         /* Check if we have room */
         if (conn->audio_buffer_len + payload_len > conn->audio_buffer_capacity) {
            /* Expand buffer */
            size_t new_capacity = conn->audio_buffer_capacity * 2;

            /* Enforce maximum capacity to prevent OOM from malicious/long recordings */
            if (new_capacity > WEBUI_AUDIO_MAX_CAPACITY) {
               OLOG_WARNING("WebUI: Audio buffer would exceed max capacity (%d bytes)",
                            WEBUI_AUDIO_MAX_CAPACITY);
               send_error_impl(conn->wsi, "BUFFER_FULL", "Recording too long");
               return;
            }

            uint8_t *new_buffer = realloc(conn->audio_buffer, new_capacity);
            if (!new_buffer) {
               OLOG_ERROR("WebUI: Failed to expand audio buffer");
               send_error_impl(conn->wsi, "BUFFER_ERROR", "Audio buffer allocation failed");
               return;
            }
            conn->audio_buffer = new_buffer;
            conn->audio_buffer_capacity = new_capacity;
         }

         /* Append audio data */
         memcpy(conn->audio_buffer + conn->audio_buffer_len, payload, payload_len);
         conn->audio_buffer_len += payload_len;

         OLOG_DEBUG("WebUI: Accumulated %zu bytes audio (total: %zu)", payload_len,
                    conn->audio_buffer_len);
         break;
      }

      case WS_BIN_AUDIO_IN_END: {
         /* End of utterance - process accumulated audio */
         if (!conn->audio_buffer || conn->audio_buffer_len == 0) {
            OLOG_WARNING("WebUI: AUDIO_IN_END but no audio accumulated");
            break;
         }

         OLOG_INFO("WebUI: Audio end, processing %zu bytes", conn->audio_buffer_len);

         /* Under the turn queue (P1) the queue serializes turn spawning per
          * session, so we do NOT bump request_generation here (that would
          * supersede a still-queued turn), nor clear the turn flags / bind
          * stream_conversation_id — both happen AT DEQUEUE (in
          * audio_turn_thread_entry), when this turn actually starts. The
          * conversation is captured now and applied at dequeue. */
         unsigned int new_gen = 0;
         int64_t turn_conv_id = 0;
         if (conn->session) {
            new_gen = atomic_load(&conn->session->request_generation);
            turn_conv_id = webui_get_active_conversation_id(conn->session);
         }

         /* Create work item for worker thread */
         audio_work_t *work = malloc(sizeof(audio_work_t));
         if (!work) {
            OLOG_ERROR("WebUI: Failed to allocate audio work");
            free(conn->audio_buffer);
            conn->audio_buffer = NULL;
            conn->audio_buffer_len = 0;
            send_error_impl(conn->wsi, "PROCESSING_ERROR", "Audio processing failed");
            return;
         }

         work->session = conn->session;
         work->audio_data = conn->audio_buffer;
         work->audio_len = conn->audio_buffer_len;
         work->use_opus = conn->use_opus;
         work->request_gen = new_gen; /* current gen; supersede is disabled under the queue */
         work->conv_id = turn_conv_id;

         /* Transfer ownership to worker */
         conn->audio_buffer = NULL;
         conn->audio_buffer_len = 0;

         /* Retain session for the queued turn (released via audio_turn_free on
          * reject/purge, or session_release inside audio_worker_thread on run). */
         session_retain(conn->session);

         /* Enqueue rather than spawn directly: the turn queue guarantees only one
          * turn runs per session at a time (serializes user + background turns),
          * spawning this one now iff nothing is in flight. */
         int qrc = turn_queue_enqueue(conn->session->session_id, TURN_SOURCE_USER, work,
                                      audio_turn_spawn, audio_turn_free);
         if (qrc != TURN_QUEUE_OK) {
            if (qrc == TURN_QUEUE_FULL) {
               send_error_impl(conn->wsi, "TURN_QUEUE_FULL",
                               "Too many turns queued; please wait for the current one to finish");
            } else {
               OLOG_ERROR("WebUI: failed to enqueue audio turn");
               send_error_impl(conn->wsi, "PROCESSING_ERROR", "Audio processing failed");
            }
            audio_turn_free(work); /* frees work + audio_data AND releases the session retain */
         }
         break;
      }

      default:
         OLOG_WARNING("WebUI: Unknown binary message type: 0x%02x", msg_type);
         break;
   }
}
