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
#include <stdlib.h>
#include <string.h>

#include "asr/asr_interface.h"
#include "audio/resampler.h"
#include "core/worker_pool.h"
#include "logging.h"
#include "tts/text_to_speech.h"

/* =============================================================================
 * Module State
 * ============================================================================= */

static OpusDecoder *s_decoder = NULL;
static OpusEncoder *s_encoder = NULL;
static resampler_t *s_tts_resampler = NULL; /* 22050Hz → 16000Hz for TTS output */
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_initialized = false;

/* Note: ASR contexts are borrowed from worker pool (no local ASR context)
 * This avoids loading Whisper model twice and saves GPU memory */

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int webui_audio_init(void) {
   pthread_mutex_lock(&s_mutex);

   if (s_initialized) {
      LOG_WARNING("WebUI audio already initialized");
      pthread_mutex_unlock(&s_mutex);
      return WEBUI_AUDIO_SUCCESS;
   }

   int err;

   /* Create Opus decoder (for incoming audio from browser) */
   s_decoder = opus_decoder_create(WEBUI_OPUS_SAMPLE_RATE, WEBUI_OPUS_CHANNELS, &err);
   if (err != OPUS_OK || !s_decoder) {
      LOG_ERROR("WebUI audio: Failed to create Opus decoder: %s", opus_strerror(err));
      pthread_mutex_unlock(&s_mutex);
      return WEBUI_AUDIO_ERROR;
   }

   /* Create Opus encoder (for outgoing TTS audio to browser) */
   s_encoder = opus_encoder_create(WEBUI_OPUS_SAMPLE_RATE, WEBUI_OPUS_CHANNELS,
                                   OPUS_APPLICATION_VOIP, &err);
   if (err != OPUS_OK || !s_encoder) {
      LOG_ERROR("WebUI audio: Failed to create Opus encoder: %s", opus_strerror(err));
      opus_decoder_destroy(s_decoder);
      s_decoder = NULL;
      pthread_mutex_unlock(&s_mutex);
      return WEBUI_AUDIO_ERROR;
   }

   /* Configure encoder for voice */
   opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(WEBUI_OPUS_BITRATE));
   opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(5)); /* Balance quality/CPU */
   opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

   /* Create resampler for TTS output (22050Hz → 16000Hz) */
   s_tts_resampler = resampler_create(22050, WEBUI_OPUS_SAMPLE_RATE, 1);
   if (!s_tts_resampler) {
      LOG_ERROR("WebUI audio: Failed to create TTS resampler");
      opus_encoder_destroy(s_encoder);
      opus_decoder_destroy(s_decoder);
      s_encoder = NULL;
      s_decoder = NULL;
      pthread_mutex_unlock(&s_mutex);
      return WEBUI_AUDIO_ERROR;
   }

   /* Verify worker pool is initialized (provides ASR contexts) */
   if (!worker_pool_is_initialized()) {
      LOG_WARNING("WebUI audio: Worker pool not initialized - ASR will be unavailable");
   }

   s_initialized = true;
   LOG_INFO("WebUI audio initialized (Opus %dHz, ASR via worker pool)", WEBUI_OPUS_SAMPLE_RATE);

   pthread_mutex_unlock(&s_mutex);
   return WEBUI_AUDIO_SUCCESS;
}

void webui_audio_cleanup(void) {
   pthread_mutex_lock(&s_mutex);

   if (!s_initialized) {
      pthread_mutex_unlock(&s_mutex);
      return;
   }

   /* Note: ASR contexts are owned by worker pool, not cleaned up here */

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

   pthread_mutex_unlock(&s_mutex);
}

bool webui_audio_is_initialized(void) {
   bool result;
   pthread_mutex_lock(&s_mutex);
   result = s_initialized;
   pthread_mutex_unlock(&s_mutex);
   return result;
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

   pthread_mutex_lock(&s_mutex);

   if (!s_initialized || !s_decoder) {
      pthread_mutex_unlock(&s_mutex);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Pre-allocate PCM buffer for worst case (assume all frames are max duration)
    * Each Opus frame can decode to at most 120ms of audio at 16kHz = 1920 samples */
   size_t max_output_samples = WEBUI_PCM_MAX_SAMPLES;
   int16_t *pcm_buffer = malloc(max_output_samples * sizeof(int16_t));
   if (!pcm_buffer) {
      pthread_mutex_unlock(&s_mutex);
      return WEBUI_AUDIO_ERROR_ALLOC;
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

      total_samples += decoded;
      offset += frame_len;

      /* Safety check */
      if (total_samples >= max_output_samples - WEBUI_OPUS_FRAME_SAMPLES * 2) {
         LOG_WARNING("WebUI audio: PCM buffer nearly full, stopping decode");
         break;
      }
   }

   pthread_mutex_unlock(&s_mutex);

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

   pthread_mutex_lock(&s_mutex);

   if (!s_initialized || !s_decoder) {
      pthread_mutex_unlock(&s_mutex);
      return -1;
   }

   int decoded = opus_decode(s_decoder, opus_frame, (int)opus_len, pcm_out, max_samples, 0);

   pthread_mutex_unlock(&s_mutex);

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

   pthread_mutex_lock(&s_mutex);

   if (!s_initialized || !s_encoder) {
      pthread_mutex_unlock(&s_mutex);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Calculate number of frames (20ms each at 16kHz = 320 samples) */
   size_t frame_samples = WEBUI_OPUS_FRAME_SAMPLES;
   size_t num_frames = (pcm_samples + frame_samples - 1) / frame_samples;

   /* Allocate output buffer: 2 bytes length prefix + max frame size per frame */
   size_t max_output_size = num_frames * (2 + WEBUI_OPUS_MAX_FRAME_SIZE);
   uint8_t *output = malloc(max_output_size);
   if (!output) {
      pthread_mutex_unlock(&s_mutex);
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

   pthread_mutex_unlock(&s_mutex);

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

   if (!s_initialized) {
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

int webui_audio_opus_to_text(const uint8_t *opus_data, size_t opus_len, char **text_out) {
   int16_t *pcm = NULL;
   size_t pcm_samples = 0;

   /* Decode Opus to PCM */
   int ret = webui_opus_decode_stream(opus_data, opus_len, &pcm, &pcm_samples);
   if (ret != WEBUI_AUDIO_SUCCESS) {
      return ret;
   }

   /* Transcribe PCM */
   ret = webui_audio_transcribe(pcm, pcm_samples, text_out);
   free(pcm);

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

   pthread_mutex_lock(&s_mutex);

   if (!s_initialized || !s_tts_resampler) {
      pthread_mutex_unlock(&s_mutex);
      free(tts_pcm);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Resample 22050Hz → 16000Hz */
   size_t output_size = resampler_get_output_size(s_tts_resampler, tts_samples);
   int16_t *resampled = malloc(output_size * sizeof(int16_t));
   if (!resampled) {
      pthread_mutex_unlock(&s_mutex);
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

      size_t resampled_chunk = resampler_process(s_tts_resampler, tts_pcm + offset, to_process,
                                                 resampled + total_resampled,
                                                 output_size - total_resampled);

      if (resampled_chunk == 0) {
         LOG_ERROR("WebUI audio: Resampling failed at offset %zu", offset);
         break;
      }

      total_resampled += resampled_chunk;
      offset += to_process;
   }

   pthread_mutex_unlock(&s_mutex);
   free(tts_pcm);

   if (total_resampled == 0) {
      free(resampled);
      return WEBUI_AUDIO_ERROR;
   }

   LOG_INFO("WebUI audio: Resampled %zu → %zu samples (22050 → 16000Hz)", tts_samples,
            total_resampled);

   /* Encode to Opus */
   ret = webui_opus_encode_stream(resampled, total_resampled, opus_out, opus_len);
   free(resampled);

   return ret;
}

int webui_audio_text_to_pcm16k(const char *text, int16_t **pcm_out, size_t *pcm_samples) {
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

   pthread_mutex_lock(&s_mutex);

   if (!s_initialized || !s_tts_resampler) {
      pthread_mutex_unlock(&s_mutex);
      free(tts_pcm);
      return WEBUI_AUDIO_ERROR_NOT_INITIALIZED;
   }

   /* Resample 22050Hz → 16000Hz */
   size_t output_size = resampler_get_output_size(s_tts_resampler, tts_samples);
   int16_t *resampled = malloc(output_size * sizeof(int16_t));
   if (!resampled) {
      pthread_mutex_unlock(&s_mutex);
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

      /* Calculate required output size for this specific chunk */
      size_t chunk_output_needed = resampler_get_output_size(s_tts_resampler, to_process);
      size_t available_output = output_size - total_resampled;

      /* Use the larger of chunk requirement or remaining space */
      size_t chunk_output_max = (chunk_output_needed > available_output) ? chunk_output_needed
                                                                         : available_output;

      /* Safety check: don't overflow the output buffer */
      if (total_resampled + chunk_output_max > output_size) {
         /* Need to reallocate - add extra margin */
         size_t new_size = total_resampled + chunk_output_needed + 256;
         int16_t *new_buf = realloc(resampled, new_size * sizeof(int16_t));
         if (!new_buf) {
            LOG_ERROR("WebUI audio: Failed to expand resample buffer");
            break;
         }
         resampled = new_buf;
         output_size = new_size;
         chunk_output_max = chunk_output_needed;
      }

      size_t resampled_chunk = resampler_process(s_tts_resampler, tts_pcm + offset, to_process,
                                                 resampled + total_resampled, chunk_output_max);

      if (resampled_chunk == 0) {
         LOG_ERROR("WebUI audio: Resampling failed at offset %zu", offset);
         break;
      }

      total_resampled += resampled_chunk;
      offset += to_process;
   }

   pthread_mutex_unlock(&s_mutex);
   free(tts_pcm);

   if (total_resampled == 0) {
      free(resampled);
      return WEBUI_AUDIO_ERROR;
   }

   LOG_INFO("WebUI audio: Resampled to %zu samples at 16000Hz", total_resampled);

   *pcm_out = resampled;
   *pcm_samples = total_resampled;

   return WEBUI_AUDIO_SUCCESS;
}
