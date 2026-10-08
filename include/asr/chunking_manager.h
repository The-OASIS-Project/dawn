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

#ifndef CHUNKING_MANAGER_H
#define CHUNKING_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "asr/asr_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file chunking_manager.h
 * @brief Intelligent audio chunking for Whisper long utterances
 *
 * The chunking manager handles VAD-driven pause detection to split long
 * utterances into manageable chunks for Whisper ASR. It accumulates audio,
 * detects natural sentence boundaries (pauses), and concatenates the
 * transcribed chunks for seamless command processing.
 *
 * **Design Rationale:**
 * - Whisper processes audio in batches (not streaming)
 * - Long utterances (>30s) exceed optimal processing time
 * - Natural pauses (>0.5s) indicate sentence boundaries
 * - Chunking reduces latency and improves responsiveness
 *
 * **Integration:**
 * - Used exclusively with Whisper (asr_engine == ASR_ENGINE_WHISPER)
 * - Integrates with VAD pause detection (0.5s silence)
 * - Mediates all ASR interactions when active
 *
 * **Thread Safety:**
 * - NOT thread-safe (matches asr_context_t constraints)
 * - Call from same thread as ASR processing
 *
 * **Lifecycle:**
 * - Create once in main() for persistent use
 * - Reset between utterances (chunking_manager_reset)
 * - Cleanup on application exit
 */

/**
 * @brief Opaque chunking manager context
 */
typedef struct chunking_manager chunking_manager_t;

/**
 * @brief Initialize chunking manager
 *
 * Creates a new chunking manager instance for managing Whisper audio chunks.
 * The manager takes ownership of coordinating ASR interactions but does NOT
 * take ownership of the asr_context_t (caller retains ownership).
 *
 * **CRITICAL:** This function performs defensive validation to ensure
 * chunking is only used with Whisper (not Vosk), as chunking breaks
 * Vosk's streaming architecture.
 *
 * @param asr_ctx ASR context (must be Whisper engine)
 * @return Chunking manager instance, or NULL on failure
 *
 * @note Buffer capacity defaults to 15 seconds (CHUNK_BUFFER_CAPACITY)
 * @note Returns NULL if asr_ctx is not Whisper engine (defensive check)
 */
chunking_manager_t *chunking_manager_init(asr_context_t *asr_ctx);

/**
 * @brief Add audio samples to chunking buffer
 *
 * Accumulates audio samples in the internal buffer for later finalization.
 * If buffer reaches capacity (15s), automatically finalizes the current
 * chunk to prevent overflow (Decision #4: auto-finalize strategy).
 *
 * **Circuit Breaker:** If auto-finalize fails (Whisper error), the buffer
 * is discarded to prevent infinite loops. This ensures forward progress
 * even in failure scenarios.
 *
 * @param cm Chunking manager instance
 * @param audio Audio samples (16-bit PCM, 16kHz mono)
 * @param samples Number of samples to add
 * @return SUCCESS (0) or FAILURE (1)
 *
 * @note Auto-finalizes if adding samples would exceed buffer capacity
 * @note Discards buffer on finalization failure (safe degradation)
 */
int chunking_manager_add_audio(chunking_manager_t *cm, const int16_t *audio, size_t samples);

/**
 * @brief Finalize current audio chunk and accumulate transcribed text
 *
 * Processes accumulated audio through Whisper ASR, resets the audio buffer,
 * and stores the transcribed text internally. The transcribed chunk is also
 * returned to the caller (must be freed).
 *
 * **ASR Interaction:**
 * - Calls asr_finalize() to process buffered audio
 * - Calls asr_reset() to clear buffer for next chunk
 * - Safe to call mid-utterance (Whisper is stateless per-chunk)
 *
 * **Re-entrance Protection:**
 * If finalization is already in progress (from previous call), this function
 * returns immediately without error. This prevents concurrent Whisper
 * inference which could corrupt internal state.
 *
 * @param cm Chunking manager instance
 * @param chunk_text_out Pointer to receive allocated chunk text (caller must free)
 * @return SUCCESS (0) or FAILURE (1)
 *
 * @note Sets *chunk_text_out to NULL and returns SUCCESS if already finalizing
 * @note Chunk text is accumulated internally even if *chunk_text_out is NULL
 * @note Caller must free the returned string
 */
int chunking_manager_finalize_chunk(chunking_manager_t *cm, char **chunk_text_out);

/**
 * @brief Get concatenated text from all chunks and reset
 *
 * Returns the full transcribed text by concatenating all finalized chunks
 * with space separators. After returning the text, resets the chunk
 * accumulator for the next utterance.
 *
 * **Use Case:**
 * Call this after speech ends (1.5s silence detected) to get the complete
 * command text from all chunks.
 *
 * @param cm Chunking manager instance
 * @return Allocated concatenated text, or NULL if no chunks (caller must free)
 *
 * @note Resets internal chunk array after returning (fresh start for next utterance)
 * @note Returns NULL if no chunks have been finalized
 * @note Caller must free the returned string
 */
char *chunking_manager_get_full_text(chunking_manager_t *cm);

/**
 * @brief Reset chunking manager for new utterance
 *
 * Clears audio buffer and chunk accumulator without deallocating memory.
 * Use this between utterances to prepare for the next command.
 *
 * **When to call:**
 * - After processing command text (before returning to WAKEWORD_LISTEN)
 * - After interruption/timeout (discarding incomplete utterance)
 *
 * @param cm Chunking manager instance
 *
 * @note Does NOT reset ASR context (caller must call asr_reset separately)
 * @note Frees accumulated chunk texts but retains buffer capacity
 */
void chunking_manager_reset(chunking_manager_t *cm);

/**
 * @brief Cleanup chunking manager and free all resources
 *
 * Destroys the chunking manager instance and frees all allocated memory
 * (buffers, chunk texts, struct). Call this on application shutdown.
 *
 * @param cm Chunking manager instance (can be NULL)
 *
 * @note Does NOT cleanup ASR context (caller retains ownership)
 * @note Safe to call with NULL pointer (no-op)
 */
void chunking_manager_cleanup(chunking_manager_t *cm);

/**
 * @brief Check if chunk finalization is currently in progress
 *
 * Returns whether a previous chunking_manager_finalize_chunk() call is
 * still processing. Used to prevent concurrent finalization calls which
 * could corrupt Whisper's internal state.
 *
 * **Use Case:**
 * Before calling finalize_chunk(), check if finalization is already running
 * to avoid re-entrance during long Whisper inference.
 *
 * @param cm Chunking manager instance
 * @return 1 if finalizing, 0 if idle
 *
 * @note Added per architecture review Issue #7 (concurrent finalization)
 */
int chunking_manager_is_finalizing(chunking_manager_t *cm);

/**
 * @brief Get current audio buffer usage in samples
 *
 * Returns the number of audio samples currently accumulated in the buffer.
 * Useful for monitoring buffer pressure and debugging.
 *
 * @param cm Chunking manager instance
 * @return Number of samples in buffer (0 to CHUNK_BUFFER_CAPACITY)
 *
 * @note Added per architecture review GAP #2 (buffer monitoring)
 */
size_t chunking_manager_get_buffer_usage(chunking_manager_t *cm);

/**
 * @brief Get number of chunks finalized so far
 *
 * Returns the count of chunks that have been finalized for the current
 * utterance. Useful for logging and debugging multi-chunk commands.
 *
 * @param cm Chunking manager instance
 * @return Number of finalized chunks
 *
 * @note Added per architecture review GAP #3 (chunk count query)
 */
size_t chunking_manager_get_num_chunks(chunking_manager_t *cm);

#ifdef __cplusplus
}
#endif

#endif  // CHUNKING_MANAGER_H
