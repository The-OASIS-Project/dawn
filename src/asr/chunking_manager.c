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

#include "asr/chunking_manager.h"

#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"
#include "utils/asr_transcript.h"

/**
 * @brief Chunking manager internal structure
 */
struct chunking_manager {
   asr_context_t *asr_ctx;  // ASR context (Whisper only)

   // Audio tracking (samples passed to ASR, not stored locally)
   size_t buffer_samples;   // Current samples in ASR buffer (for duration tracking)
   size_t buffer_capacity;  // Max capacity before auto-finalize (policy limit, not ASR limit)

   // Transcription accumulation
   char **chunk_texts;      // Array of chunk text strings
   size_t num_chunks;       // Number of chunks finalized
   size_t chunks_capacity;  // Allocated capacity for chunk array

   // State tracking (Issue #7: concurrent finalization protection)
   int finalization_in_progress;  // Flag to prevent re-entrance
};

// Default buffer capacity: 15 seconds at 16kHz = 240,000 samples
#define DEFAULT_CHUNK_BUFFER_CAPACITY (15 * 16000)

// Initial chunk array capacity (grows dynamically)
#define INITIAL_CHUNKS_CAPACITY 16

chunking_manager_t *chunking_manager_init(asr_context_t *asr_ctx) {
   if (!asr_ctx) {
      OLOG_ERROR("chunking_manager_init: NULL ASR context");
      return NULL;
   }

   // Defensive: Chunking only for Whisper (Issue #3: runtime assertion)
   asr_engine_type_t engine = asr_get_engine_type(asr_ctx);
   if (engine != ASR_ENGINE_WHISPER) {
      OLOG_ERROR("Chunking manager initialized for non-Whisper engine (%d), this is a bug", engine);
      return NULL;
   }

   chunking_manager_t *cm = (chunking_manager_t *)calloc(1, sizeof(chunking_manager_t));
   if (!cm) {
      OLOG_ERROR("chunking_manager_init: Failed to allocate manager struct");
      return NULL;
   }

   cm->asr_ctx = asr_ctx;
   cm->buffer_capacity = DEFAULT_CHUNK_BUFFER_CAPACITY;
   cm->buffer_samples = 0;
   cm->finalization_in_progress = 0;

   // Allocate chunk text array
   cm->chunks_capacity = INITIAL_CHUNKS_CAPACITY;
   cm->chunk_texts = (char **)calloc(cm->chunks_capacity, sizeof(char *));
   if (!cm->chunk_texts) {
      OLOG_ERROR("chunking_manager_init: Failed to allocate chunk array");
      free(cm);
      return NULL;
   }

   cm->num_chunks = 0;

   OLOG_INFO("Chunking manager initialized (capacity: %zu samples, %.1fs)", cm->buffer_capacity,
             cm->buffer_capacity / 16000.0f);

   return cm;
}

int chunking_manager_add_audio(chunking_manager_t *cm, const int16_t *audio, size_t samples) {
   if (!cm || !audio) {
      OLOG_ERROR("chunking_manager_add_audio: NULL parameter");
      return FAILURE;
   }

   // Check if adding would overflow buffer (Decision #4: auto-finalize)
   if (cm->buffer_samples + samples > cm->buffer_capacity) {
      OLOG_WARNING("Buffer near capacity (%zu/%zu samples), forcing chunk", cm->buffer_samples,
                   cm->buffer_capacity);

      char *chunk_text = NULL;
      int result = chunking_manager_finalize_chunk(cm, &chunk_text);

      if (result == FAILURE) {
         // CRITICAL: Circuit breaker to prevent infinite loop
         OLOG_ERROR("Chunk finalization failed, DISCARDING buffer to prevent hang");
         cm->buffer_samples = 0;  // Reset buffer even on failure
         return FAILURE;
      }

      free(chunk_text);

      // Check buffer pressure after finalization (Issue #4: buffer monitoring)
      if (cm->buffer_samples > cm->buffer_capacity * 0.8f) {
         OLOG_WARNING("Buffer pressure high after auto-finalize (%zu/%zu samples), "
                      "may indicate inference latency issue",
                      cm->buffer_samples, cm->buffer_capacity);
      }
   }

   // Feed audio to ASR context (Whisper accumulates internally)
   asr_result_t *partial_result = asr_process_partial(cm->asr_ctx, audio, samples);

   // Track sample count for duration/capacity monitoring
   cm->buffer_samples += samples;
   if (partial_result) {
      asr_result_free(partial_result);  // Whisper returns empty partials, discard
   } else {
      OLOG_ERROR("chunking_manager_add_audio: asr_process_partial() returned NULL");
   }

   return SUCCESS;
}

int chunking_manager_finalize_chunk(chunking_manager_t *cm, char **chunk_text_out) {
   if (!cm) {
      OLOG_ERROR("chunking_manager_finalize_chunk: NULL manager");
      return FAILURE;
   }

   if (!chunk_text_out) {
      OLOG_ERROR("chunking_manager_finalize_chunk: NULL output pointer");
      return FAILURE;
   }

   // Re-entrance protection (Issue #7: concurrent finalization)
   if (cm->finalization_in_progress) {
      OLOG_WARNING("Finalization already in progress, skipping");
      *chunk_text_out = NULL;
      return SUCCESS;  // Not a failure, just a no-op
   }

   // Nothing to finalize
   if (cm->buffer_samples == 0) {
      OLOG_INFO("No audio to finalize (buffer empty)");
      *chunk_text_out = NULL;
      return SUCCESS;
   }

   cm->finalization_in_progress = 1;

   OLOG_INFO("Finalizing chunk (%zu samples, %.2fs)", cm->buffer_samples,
             cm->buffer_samples / 16000.0f);

   // Process audio through ASR
   asr_result_t *result = asr_finalize(cm->asr_ctx);

   if (!result) {
      OLOG_ERROR("asr_finalize() returned NULL");
      cm->finalization_in_progress = 0;
      cm->buffer_samples = 0;  // Discard buffer on ASR failure
      *chunk_text_out = NULL;
      return FAILURE;
   }

   // Extract text from result
   char *chunk_text = NULL;
   if (result->text && strlen(result->text) > 0) {
      chunk_text = strdup(result->text);
      if (!chunk_text) {
         OLOG_ERROR("Failed to allocate chunk text");
         asr_result_free(result);
         cm->finalization_in_progress = 0;
         cm->buffer_samples = 0;
         *chunk_text_out = NULL;
         return FAILURE;
      }

      OLOG_INFO("Chunk %zu finalized: \"%s\"", cm->num_chunks, chunk_text);

      // Filter out blank/silence chunks ([BLANK_AUDIO], detected by Whisper).
      // Return to caller but don't store in chunk array to avoid contaminating real speech
      if (asr_transcript_is_blank(chunk_text)) {
         OLOG_INFO("Chunk contains [BLANK_AUDIO], skipping storage (not adding to concatenation)");
         cm->finalization_in_progress = 0;
         cm->buffer_samples = 0;
         *chunk_text_out = chunk_text;  // Still return to caller for logging
         asr_result_free(result);
         asr_reset(cm->asr_ctx);
         return SUCCESS;
      }

      // Grow chunk array if needed (dynamic growth)
      if (cm->num_chunks >= cm->chunks_capacity) {
         size_t new_capacity = cm->chunks_capacity * 2;
         char **new_array = (char **)realloc(cm->chunk_texts, new_capacity * sizeof(char *));
         if (!new_array) {
            OLOG_ERROR("Failed to grow chunk array (capacity %zu → %zu)", cm->chunks_capacity,
                       new_capacity);
            free(chunk_text);
            asr_result_free(result);
            cm->finalization_in_progress = 0;
            cm->buffer_samples = 0;
            *chunk_text_out = NULL;
            return FAILURE;
         }
         cm->chunk_texts = new_array;
         cm->chunks_capacity = new_capacity;
         OLOG_INFO("Chunk array grown to capacity %zu", new_capacity);
      }

      // Store chunk text internally
      cm->chunk_texts[cm->num_chunks] = strdup(chunk_text);
      if (!cm->chunk_texts[cm->num_chunks]) {
         OLOG_ERROR("Failed to store chunk text internally");
         free(chunk_text);
         asr_result_free(result);
         cm->finalization_in_progress = 0;
         cm->buffer_samples = 0;
         *chunk_text_out = NULL;
         return FAILURE;
      }

      cm->num_chunks++;
   } else {
      OLOG_INFO("Chunk finalized with empty text (silence or noise)");
      chunk_text = NULL;
   }

   asr_result_free(result);

   // Reset ASR for next chunk (safe for Whisper, per architecture review Issue #6)
   asr_reset(cm->asr_ctx);

   // Reset audio buffer
   cm->buffer_samples = 0;

   cm->finalization_in_progress = 0;

   *chunk_text_out = chunk_text;  // Caller must free
   return SUCCESS;
}

char *chunking_manager_get_full_text(chunking_manager_t *cm) {
   if (!cm) {
      OLOG_ERROR("chunking_manager_get_full_text: NULL manager");
      return NULL;
   }

   if (cm->num_chunks == 0) {
      OLOG_INFO("No chunks to concatenate");
      return NULL;
   }

   // Calculate total length (chunks + spaces + null terminator)
   size_t total_length = 0;
   for (size_t i = 0; i < cm->num_chunks; i++) {
      if (cm->chunk_texts[i]) {
         total_length += strlen(cm->chunk_texts[i]);
      }
   }
   total_length += (cm->num_chunks - 1);  // Spaces between chunks
   total_length += 1;                     // Null terminator

   // Allocate concatenated string
   char *full_text = (char *)calloc(total_length, sizeof(char));
   if (!full_text) {
      OLOG_ERROR("Failed to allocate full text buffer (%zu bytes)", total_length);
      return NULL;
   }

   // Concatenate chunks with spaces
   size_t offset = 0;
   for (size_t i = 0; i < cm->num_chunks; i++) {
      if (cm->chunk_texts[i]) {
         size_t chunk_len = strlen(cm->chunk_texts[i]);
         memcpy(full_text + offset, cm->chunk_texts[i], chunk_len);
         offset += chunk_len;

         // Add space separator (except after last chunk)
         if (i < cm->num_chunks - 1) {
            full_text[offset] = ' ';
            offset++;
         }
      }
   }

   full_text[offset] = '\0';

   OLOG_INFO("Concatenated %zu chunks: \"%s\"", cm->num_chunks, full_text);

   // Reset chunk accumulator for next utterance
   chunking_manager_reset(cm);

   return full_text;  // Caller must free
}

void chunking_manager_reset(chunking_manager_t *cm) {
   if (!cm) {
      return;
   }

   OLOG_INFO("Resetting chunking manager (%zu chunks accumulated)", cm->num_chunks);

   // Free accumulated chunk texts
   for (size_t i = 0; i < cm->num_chunks; i++) {
      if (cm->chunk_texts[i]) {
         free(cm->chunk_texts[i]);
         cm->chunk_texts[i] = NULL;
      }
   }

   // Reset counters
   cm->num_chunks = 0;
   cm->buffer_samples = 0;
   cm->finalization_in_progress = 0;

   // Note: Does NOT reset ASR context (caller's responsibility, per documentation)
}

void chunking_manager_cleanup(chunking_manager_t *cm) {
   if (!cm) {
      return;
   }

   OLOG_INFO("Cleaning up chunking manager");

   // Free chunk texts
   if (cm->chunk_texts) {
      for (size_t i = 0; i < cm->num_chunks; i++) {
         if (cm->chunk_texts[i]) {
            free(cm->chunk_texts[i]);
         }
      }
      free(cm->chunk_texts);
   }

   // Free manager struct
   free(cm);

   // Note: Does NOT cleanup ASR context (caller retains ownership, per documentation)
}

// Query functions (added per architecture review)

int chunking_manager_is_finalizing(chunking_manager_t *cm) {
   if (!cm) {
      return 0;
   }
   return cm->finalization_in_progress;
}

size_t chunking_manager_get_buffer_usage(chunking_manager_t *cm) {
   if (!cm) {
      return 0;
   }
   return cm->buffer_samples;
}

size_t chunking_manager_get_num_chunks(chunking_manager_t *cm) {
   if (!cm) {
      return 0;
   }
   return cm->num_chunks;
}
