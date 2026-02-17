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

#include "asr/asr_interface.h"
#include "audio/resampler.h"
#include "core/session_manager.h"
#include "core/worker_pool.h"
#include "llm/llm_context.h"
#include "logging.h"
#include "tts/text_to_speech.h"
#include "tts/tts_preprocessing.h"
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
      LOG_WARNING("WebUI audio already initialized");
      pthread_mutex_unlock(&s_audio_mutex);
      return WEBUI_AUDIO_SUCCESS;
   }

   int err;

   /* Create Opus decoder (for incoming audio from browser) */
   s_decoder = opus_decoder_create(WEBUI_OPUS_SAMPLE_RATE, WEBUI_OPUS_CHANNELS, &err);
   if (err != OPUS_OK || !s_decoder) {
      LOG_ERROR("WebUI audio: Failed to create Opus decoder: %s", opus_strerror(err));
      pthread_mutex_unlock(&s_audio_mutex);
      return WEBUI_AUDIO_ERROR;
   }

   /* Create Opus encoder (for outgoing TTS audio to browser) */
   s_encoder = opus_encoder_create(WEBUI_OPUS_SAMPLE_RATE, WEBUI_OPUS_CHANNELS,
                                   OPUS_APPLICATION_VOIP, &err);
   if (err != OPUS_OK || !s_encoder) {
      LOG_ERROR("WebUI audio: Failed to create Opus encoder: %s", opus_strerror(err));
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
      LOG_ERROR("WebUI audio: Failed to create input resampler");
      opus_encoder_destroy(s_encoder);
      opus_decoder_destroy(s_decoder);
      s_encoder = NULL;
      s_decoder = NULL;
      pthread_mutex_unlock(&s_audio_mutex);
      return WEBUI_AUDIO_ERROR;
   }

   /* Create resampler for TTS output (22050Hz → 48kHz for Opus output) */
   s_tts_resampler = resampler_create(22050, WEBUI_OPUS_SAMPLE_RATE, 1);
   if (!s_tts_resampler) {
      LOG_ERROR("WebUI audio: Failed to create TTS resampler");
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
      LOG_WARNING("WebUI audio: Worker pool not initialized - ASR will be unavailable");
   }

   s_initialized = true;
   LOG_INFO("WebUI audio initialized (Opus %dHz, ASR %dHz, via worker pool)",
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
   LOG_INFO("WebUI audio cleaned up");

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
         LOG_WARNING("WebUI audio: Invalid frame length %u at offset %zu", frame_len, offset - 2);
         break;
      }

      /* Decode frame */
      int decoded = opus_decode(s_decoder, opus_data + offset, frame_len,
                                pcm_buffer + total_samples,
                                (int)(max_output_samples - total_samples), 0);

      if (decoded < 0) {
         LOG_WARNING("WebUI audio: Opus decode error: %s", opus_strerror(decoded));
         /* Try packet loss concealment */
         decoded = opus_decode(s_decoder, NULL, 0, pcm_buffer + total_samples,
                               WEBUI_OPUS_FRAME_SAMPLES, 0);
         if (decoded < 0) {
            decoded = 0; /* Skip this frame */
         }
      }

      /* Verify decoded count doesn't exceed remaining buffer space */
      if (decoded > 0 && (size_t)decoded > max_output_samples - total_samples) {
         LOG_ERROR("WebUI audio: Opus decode returned more samples than buffer space");
         break;
      }

      total_samples += decoded;
      offset += frame_len;

      /* Safety check - stop before buffer is completely full */
      if (total_samples >= max_output_samples - WEBUI_OPUS_FRAME_SAMPLES * 2) {
         LOG_WARNING("WebUI audio: PCM buffer nearly full, stopping decode");
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

   LOG_INFO("WebUI audio: Decoded %zu samples from %zu bytes Opus", total_samples, opus_len);
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
                             size_t *opus_len) {
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
         LOG_WARNING("WebUI audio: Opus encode error: %s", opus_strerror(encoded_bytes));
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

   LOG_INFO("WebUI audio: Encoded %zu samples to %zu bytes Opus (%zu frames)", pcm_samples,
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
      LOG_WARNING("WebUI audio: All workers busy, cannot transcribe");
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
   LOG_INFO("WebUI audio: ASR result: \"%s\" (%.1fms)", result->text, result->processing_time);

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
         LOG_ERROR("WebUI audio: 48k→16k resampling failed at offset %zu", offset);
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

   LOG_INFO("WebUI audio: Resampled %zu → %zu samples (48kHz → 16kHz)", pcm_samples,
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

   LOG_INFO("WebUI audio: PCM48k resampled %zu → %zu samples (48kHz → 16kHz)", pcm_samples,
            total_resampled);

   /* Transcribe resampled PCM (16kHz) */
   int ret = webui_audio_transcribe(resampled, total_resampled, text_out);
   free(resampled);

   return ret;
}

/* =============================================================================
 * TTS Integration Functions
 * ============================================================================= */

int webui_audio_text_to_opus(const char *text, uint8_t **opus_out, size_t *opus_len) {
   if (!text || strlen(text) == 0 || !opus_out || !opus_len) {
      return WEBUI_AUDIO_ERROR;
   }

   /* Generate TTS audio (22050Hz) */
   int16_t *tts_pcm = NULL;
   size_t tts_samples = 0;
   uint32_t tts_rate = 0;

   int ret = text_to_speech_to_pcm(text, &tts_pcm, &tts_samples, &tts_rate);
   if (ret != 0 || !tts_pcm || tts_samples == 0) {
      LOG_ERROR("WebUI audio: TTS failed");
      return WEBUI_AUDIO_ERROR;
   }

   LOG_INFO("WebUI audio: TTS generated %zu samples at %uHz", tts_samples, tts_rate);

   pthread_mutex_lock(&s_encode_mutex);

   if (!s_tts_resampler) {
      pthread_mutex_unlock(&s_encode_mutex);
      free(tts_pcm);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Resample 22050Hz → 48000Hz (Opus native rate)
    * Pre-allocate with 10% margin to avoid reallocs during chunked processing */
   size_t output_size = resampler_get_output_size(s_tts_resampler, tts_samples);
   output_size = (output_size * 11) / 10;
   int16_t *resampled = malloc(output_size * sizeof(int16_t));
   if (!resampled) {
      pthread_mutex_unlock(&s_encode_mutex);
      free(tts_pcm);
      return WEBUI_AUDIO_ERROR_ALLOC;
   }

   /* Process in chunks if needed (resampler has max chunk size) */
   size_t total_resampled = 0;
   size_t chunk_size = RESAMPLER_MAX_SAMPLES;
   size_t offset = 0;

   while (offset < tts_samples) {
      size_t remaining = tts_samples - offset;
      size_t to_process = (remaining < chunk_size) ? remaining : chunk_size;
      size_t available_output = output_size - total_resampled;

      size_t resampled_chunk = resampler_process(s_tts_resampler, tts_pcm + offset, to_process,
                                                 resampled + total_resampled, available_output);

      if (resampled_chunk == 0) {
         LOG_ERROR("WebUI audio: Resampling failed at offset %zu", offset);
         break;
      }

      total_resampled += resampled_chunk;
      offset += to_process;
   }

   pthread_mutex_unlock(&s_encode_mutex);
   free(tts_pcm);

   if (total_resampled == 0) {
      free(resampled);
      return WEBUI_AUDIO_ERROR;
   }

   LOG_INFO("WebUI audio: Resampled %zu → %zu samples (22050 → %dHz)", tts_samples, total_resampled,
            WEBUI_OPUS_SAMPLE_RATE);

   /* Encode to Opus */
   ret = webui_opus_encode_stream(resampled, total_resampled, opus_out, opus_len);
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
      LOG_ERROR("WebUI audio: TTS failed");
      return WEBUI_AUDIO_ERROR;
   }

   LOG_INFO("WebUI audio: TTS generated %zu samples at %uHz", tts_samples, tts_rate);

   pthread_mutex_lock(&s_encode_mutex);

   if (!s_tts_resampler) {
      pthread_mutex_unlock(&s_encode_mutex);
      free(tts_pcm);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Resample 22050Hz → 48000Hz (Opus native rate)
    * Pre-allocate with 10% margin to avoid reallocs during chunked processing */
   size_t output_size = resampler_get_output_size(s_tts_resampler, tts_samples);
   output_size = (output_size * 11) / 10;
   int16_t *resampled = malloc(output_size * sizeof(int16_t));
   if (!resampled) {
      pthread_mutex_unlock(&s_encode_mutex);
      free(tts_pcm);
      return WEBUI_AUDIO_ERROR_ALLOC;
   }

   /* Process in chunks if needed (resampler has max chunk size) */
   size_t total_resampled = 0;
   size_t chunk_size = RESAMPLER_MAX_SAMPLES;
   size_t offset = 0;

   while (offset < tts_samples) {
      size_t remaining = tts_samples - offset;
      size_t to_process = (remaining < chunk_size) ? remaining : chunk_size;
      size_t available_output = output_size - total_resampled;

      size_t resampled_chunk = resampler_process(s_tts_resampler, tts_pcm + offset, to_process,
                                                 resampled + total_resampled, available_output);

      if (resampled_chunk == 0) {
         LOG_ERROR("WebUI audio: Resampling failed at offset %zu", offset);
         break;
      }

      total_resampled += resampled_chunk;
      offset += to_process;
   }

   pthread_mutex_unlock(&s_encode_mutex);
   free(tts_pcm);

   if (total_resampled == 0) {
      free(resampled);
      return WEBUI_AUDIO_ERROR;
   }

   LOG_INFO("WebUI audio: Resampled to %zu samples at %dHz", total_resampled,
            WEBUI_OPUS_SAMPLE_RATE);

   *pcm_out = resampled;
   *pcm_samples = total_resampled;

   return WEBUI_AUDIO_SUCCESS;
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
} audio_work_t;

/**
 * @brief Sentence callback for real-time TTS audio streaming
 *
 * Called for each complete sentence during LLM response streaming.
 * Generates TTS and sends audio immediately, enabling sentence-by-sentence
 * playback instead of waiting for the full response.
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

   /* Fast path: skip strdup + 4 string scans if sentence has no special content */
   bool needs_cleaning = (strstr(sentence, "<command>") != NULL ||
                          strstr(sentence, "<end_of_turn>") != NULL ||
                          strchr(sentence, '*') != NULL);

   char *cleaned;
   if (needs_cleaning) {
      cleaned = strdup(sentence);
      if (!cleaned)
         return;

      /* Remove command tags (they'll be processed from the full response later) */
      strip_command_tags(cleaned);

      /* Remove special characters that cause TTS issues */
      remove_chars(cleaned, "*");
      remove_emojis(cleaned);
   } else {
      cleaned = strdup(sentence);
      if (!cleaned)
         return;
      remove_emojis(cleaned);
   }

   /* Trim whitespace */
   size_t len = strlen(cleaned);
   while (len > 0 && (cleaned[len - 1] == ' ' || cleaned[len - 1] == '\t' ||
                      cleaned[len - 1] == '\n' || cleaned[len - 1] == '\r')) {
      cleaned[--len] = '\0';
   }

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

      LOG_INFO("WebUI: TTS streaming sentence (%s): %.60s%s", use_opus ? "opus" : "pcm", cleaned,
               len > 60 ? "..." : "");

      if (use_opus) {
         /* Encode TTS output as Opus for bandwidth savings */
         uint8_t *opus = NULL;
         size_t opus_len = 0;
         int ret = webui_audio_text_to_opus(cleaned, &opus, &opus_len);

         if (ret == WEBUI_AUDIO_SUCCESS && opus && opus_len > 0) {
            /* Re-check: session may have disconnected during TTS synthesis.
             * Use snapshotted conn (not session->client_data) to avoid TOCTOU race. */
            if (!session->disconnected && conn->tts_enabled) {
               webui_send_audio(session, opus, opus_len);
               webui_send_audio_end(session);
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
            LOG_INFO("WebUI audio: Tier 2 TTS %zu samples at %uHz (native, no resample)",
                     tts_samples, tts_rate);
            /* Re-check: session may have disconnected during TTS synthesis.
             * Use snapshotted conn (not session->client_data) to avoid TOCTOU race. */
            if (!session->disconnected && conn->tts_enabled) {
               size_t bytes = tts_samples * sizeof(int16_t);
               webui_send_audio(session, (const uint8_t *)tts_pcm, bytes);
               webui_send_audio_end(session);
            }
            free(tts_pcm);
         }
      } else {
         /* WebUI browser: send resampled 48kHz PCM */
         int16_t *pcm = NULL;
         size_t samples = 0;
         int ret = webui_audio_text_to_pcm(cleaned, &pcm, &samples);

         if (ret == WEBUI_AUDIO_SUCCESS && pcm && samples > 0) {
            /* Re-check: session may have disconnected during TTS synthesis.
             * Use snapshotted conn (not session->client_data) to avoid TOCTOU race. */
            if (!session->disconnected && conn->tts_enabled) {
               size_t bytes = samples * sizeof(int16_t);
               webui_send_audio(session, (const uint8_t *)pcm, bytes);
               webui_send_audio_end(session);
            }
            free(pcm);
         }
      }
   }

   free(cleaned);
}

/* REQUEST_SUPERSEDED macro now defined in webui_internal.h */

/**
 * @brief Audio worker thread - process voice input and generate response
 *
 * Uses sentence-by-sentence TTS streaming: audio is generated and sent as each
 * sentence completes during the LLM response, rather than waiting for the full
 * response. This significantly reduces perceived latency.
 */
static void *audio_worker_thread(void *arg) {
   audio_work_t *work = (audio_work_t *)arg;
   session_t *session = work->session;
   uint8_t *audio_data = work->audio_data;
   size_t audio_len = work->audio_len;
   unsigned int expected_gen = work->request_gen;

   if (!session || REQUEST_SUPERSEDED(session, expected_gen)) {
      LOG_INFO("WebUI: Audio session disconnected or request superseded, aborting");
      if (session) {
         session_release(session);
      }
      free(audio_data);
      free(work);
      return NULL;
   }

   LOG_INFO("WebUI: Processing audio for session %u (%zu bytes, %s)", session->session_id,
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
         LOG_WARNING("WebUI: Tier 2 audio length %zu is not a multiple of 2", audio_len);
         webui_send_error(session, "INVALID_AUDIO", "Audio length must be a multiple of 2 bytes");
         webui_send_state(session, "idle");
         session_release(session);
         free(audio_data);
         free(work);
         return NULL;
      }

      size_t pcm_samples = audio_len / sizeof(int16_t);

      /* Enforce maximum duration to prevent DoS (30 seconds at 16kHz) */
      const size_t max_samples_16k = (size_t)WEBUI_MAX_RECORDING_SECONDS * 16000;
      if (pcm_samples > max_samples_16k) {
         LOG_WARNING("WebUI: Tier 2 audio too long: %zu samples (max %zu)", pcm_samples,
                     max_samples_16k);
         webui_send_error(session, "AUDIO_TOO_LONG", "Recording exceeds 30 second limit");
         webui_send_state(session, "idle");
         session_release(session);
         free(audio_data);
         free(work);
         return NULL;
      }

      LOG_INFO("WebUI: Tier 2 audio: %zu samples at 16kHz (no resample needed)", pcm_samples);
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

   if (ret != WEBUI_AUDIO_SUCCESS || !transcript || strlen(transcript) == 0) {
      LOG_WARNING("WebUI: Audio transcription failed or empty");
      webui_send_error(session, "ASR_FAILED", "Could not understand audio");
      webui_send_state(session, "idle");
      session_release(session);
      free(transcript);
      free(work);
      return NULL;
   }

   LOG_INFO("WebUI: Transcribed: \"%s\"", transcript);

   /* Check if request was superseded */
   if (REQUEST_SUPERSEDED(session, expected_gen)) {
      session_release(session);
      free(transcript);
      free(work);
      return NULL;
   }

   /* Echo transcription as user message */
   webui_send_transcript(session, "user", transcript);

   /* Add user message to history immediately (before LLM call can be cancelled) */
   session_add_message(session, "user", transcript);

   /* Send "thinking" state while LLM processes - streaming callback will switch to "speaking" */
   webui_send_state_with_detail(session, "thinking", "Processing request...");

   /* Call LLM with TTS streaming - audio is generated and sent per-sentence
    * No vision images for voice input (pass NULL for vision params) */
   char *response = session_llm_call_with_tts_vision_no_add(session, transcript, NULL, NULL, NULL,
                                                            0, webui_sentence_audio_callback,
                                                            session);
   free(transcript);

   if (!response || REQUEST_SUPERSEDED(session, expected_gen)) {
      LOG_WARNING("WebUI: LLM call failed or request superseded");
      if (!REQUEST_SUPERSEDED(session, expected_gen)) {
         webui_send_error(session, "LLM_ERROR", "Failed to get response");
      }
      webui_send_state(session, "idle");
      session_release(session);
      free(response);
      free(work);
      return NULL;
   }

   /* Process commands if present (audio already sent via streaming callback) */
   if (strstr(response, "<command>")) {
      LOG_INFO("WebUI: Audio response contains commands, processing...");
      webui_send_state(session, "processing");

      char *processed = webui_process_commands(response, session);
      if (processed && !REQUEST_SUPERSEDED(session, expected_gen)) {
         free(response);
         response = processed;

         /* Generate TTS for the follow-up response (command results) */
         LOG_INFO("WebUI: Generating TTS for command result: %.60s%s", processed,
                  strlen(processed) > 60 ? "..." : "");
         webui_sentence_audio_callback(processed, session);
      }
   }

   /* Send audio end marker (all audio chunks have been sent) */
   webui_send_audio_end(session);

   /* Free response */
   free(response);

   /* Send context usage update to WebUI */
   {
      int current_tokens, max_tokens;
      float threshold;
      llm_context_get_last_usage(&current_tokens, &max_tokens, &threshold);
      if (max_tokens > 0) {
         webui_send_context(session, current_tokens, max_tokens, threshold);
      }
   }

   webui_send_state(session, "idle");

   /* Mark interaction complete for conversation idle timeout tracking */
   session_update_interaction_complete(session);

   /* Release session reference (acquired before thread creation) */
   session_release(session);

   free(work);
   return NULL;
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
      LOG_WARNING("WebUI: Empty binary message");
      return;
   }

   if (!conn->session) {
      LOG_WARNING("WebUI: Binary message but no session");
      return;
   }

   uint8_t msg_type = data[0];
   const uint8_t *payload = data + 1;
   size_t payload_len = len - 1;

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
               LOG_ERROR("WebUI: Failed to allocate audio buffer");
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
               LOG_WARNING("WebUI: Audio buffer would exceed max capacity (%d bytes)",
                           WEBUI_AUDIO_MAX_CAPACITY);
               send_error_impl(conn->wsi, "BUFFER_FULL", "Recording too long");
               return;
            }

            uint8_t *new_buffer = realloc(conn->audio_buffer, new_capacity);
            if (!new_buffer) {
               LOG_ERROR("WebUI: Failed to expand audio buffer");
               send_error_impl(conn->wsi, "BUFFER_ERROR", "Audio buffer allocation failed");
               return;
            }
            conn->audio_buffer = new_buffer;
            conn->audio_buffer_capacity = new_capacity;
         }

         /* Append audio data */
         memcpy(conn->audio_buffer + conn->audio_buffer_len, payload, payload_len);
         conn->audio_buffer_len += payload_len;

         LOG_DEBUG("WebUI: Accumulated %zu bytes audio (total: %zu)", payload_len,
                   conn->audio_buffer_len);
         break;
      }

      case WS_BIN_AUDIO_IN_END: {
         /* End of utterance - process accumulated audio */
         if (!conn->audio_buffer || conn->audio_buffer_len == 0) {
            LOG_WARNING("WebUI: AUDIO_IN_END but no audio accumulated");
            break;
         }

         LOG_INFO("WebUI: Audio end, processing %zu bytes", conn->audio_buffer_len);

         /* Increment request generation FIRST to invalidate any pending requests,
          * then reset disconnected flag. This prevents race conditions where an
          * old worker sees disconnected=false after a new message is sent. */
         unsigned int new_gen = 0;
         if (conn->session) {
            new_gen = atomic_fetch_add(&conn->session->request_generation, 1) + 1;
            conn->session->disconnected = false;
         }

         /* Create work item for worker thread */
         audio_work_t *work = malloc(sizeof(audio_work_t));
         if (!work) {
            LOG_ERROR("WebUI: Failed to allocate audio work");
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
         work->request_gen = new_gen;

         /* Transfer ownership to worker */
         conn->audio_buffer = NULL;
         conn->audio_buffer_len = 0;

         /* Retain session for worker thread (worker will release when done) */
         session_retain(conn->session);

         /* Spawn worker thread */
         pthread_t thread;
         pthread_attr_t attr;
         pthread_attr_init(&attr);
         pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

         int ret = pthread_create(&thread, &attr, audio_worker_thread, work);
         pthread_attr_destroy(&attr);

         if (ret != 0) {
            LOG_ERROR("WebUI: Failed to create audio worker thread");
            session_release(conn->session);
            free(work->audio_data);
            free(work);
            send_error_impl(conn->wsi, "PROCESSING_ERROR", "Audio processing failed");
         }
         break;
      }

      default:
         LOG_WARNING("WebUI: Unknown binary message type: 0x%02x", msg_type);
         break;
   }
}
