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
 * Search Result Summarizer - LLM-based summarization for large search results
 *
 * This module intercepts large search results (over a configurable threshold)
 * and summarizes them using either a local LLM (llama-server) or cloud LLM
 * before passing them to the main conversation LLM.
 */

#ifndef SEARCH_SUMMARIZER_H
#define SEARCH_SUMMARIZER_H

#include <stddef.h>

// Return codes
#define SUMMARIZER_SUCCESS 0
#define SUMMARIZER_ERROR_NOT_INIT 1
#define SUMMARIZER_ERROR_BACKEND 2
#define SUMMARIZER_ERROR_ALLOC 3
#define SUMMARIZER_ERROR_DISABLED 4

// Default configuration values
#define SUMMARIZER_DEFAULT_THRESHOLD 3072
#define SUMMARIZER_DEFAULT_TARGET_WORDS 600
#define SUMMARIZER_LOCAL_ENDPOINT "http://127.0.0.1:8080/v1/chat/completions"
/* Timeout for local LLM summarization (2 minutes - local models are slow on large content) */
#define SUMMARIZER_LOCAL_TIMEOUT_SEC 120

// Maximum content size when passing through raw results on summarization failure.
// When content exceeds this limit, it's truncated at natural boundaries
// (paragraph > sentence > word) to preserve readability. ~4-5K tokens for GPT-4.
#define SUMMARIZER_MAX_PASSTHROUGH_BYTES (16 * 1024)

/**
 * @brief Backend type for summarization
 */
typedef enum {
   SUMMARIZER_BACKEND_DISABLED = 0,  // No summarization, pass-through
   SUMMARIZER_BACKEND_LOCAL,         // Use dedicated local llama-server (127.0.0.1:8080)
   SUMMARIZER_BACKEND_DEFAULT,       // Use main LLM interface (whatever is configured)
   SUMMARIZER_BACKEND_TFIDF          // Use local TF-IDF extractive summarization
} summarizer_backend_t;

/**
 * @brief Failure policy when summarization fails
 */
typedef enum {
   SUMMARIZER_ON_FAILURE_ERROR = 0,   // Return error (caller handles)
   SUMMARIZER_ON_FAILURE_PASSTHROUGH  // Fall back to raw results
} summarizer_failure_policy_t;

/**
 * @brief Summarizer configuration
 */
typedef struct {
   summarizer_backend_t backend;
   summarizer_failure_policy_t failure_policy;
   size_t threshold_bytes;       // Summarize if results exceed this size
   size_t target_summary_words;  // Target word count for summary (for LLM backends)
   float target_ratio;           // Target sentence ratio for TF-IDF (0.0-1.0)
} summarizer_config_t;

/**
 * @brief Initialize the summarizer module
 *
 * Must be called before any other summarizer functions.
 * Thread safety: Call once from main thread before spawning workers.
 *
 * @param config Configuration (copied internally). NULL uses defaults.
 * @return SUMMARIZER_SUCCESS or error code
 */
int search_summarizer_init(const summarizer_config_t *config);

/**
 * @brief Process search results, summarizing if over threshold
 *
 * Thread safety: Safe to call from multiple threads concurrently.
 *
 * Behavior:
 * - If backend is DISABLED: returns copy of input
 * - If input size <= threshold: returns copy of input
 * - If input size > threshold: summarizes using configured backend
 * - On failure with PASSTHROUGH policy: returns copy of input
 * - On failure with ERROR policy: returns error code, out_result may be NULL
 *
 * @param search_results Raw search results text
 * @param original_query The user's original query (for context in summary)
 * @param out_result Pointer to receive allocated result (caller frees)
 * @return SUMMARIZER_SUCCESS, or error code
 */
int search_summarizer_process(const char *search_results,
                              const char *original_query,
                              char **out_result);

/**
 * @brief Get current configuration (read-only)
 *
 * @return Pointer to current config, or NULL if not initialized
 */
const summarizer_config_t *search_summarizer_get_config(void);

/**
 * @brief Get human-readable backend name
 *
 * @param backend Backend type
 * @return String name (e.g., "local", "cloud", "disabled")
 */
const char *search_summarizer_backend_name(summarizer_backend_t backend);

/**
 * @brief Parse backend type from string
 *
 * @param str String to parse ("local", "cloud", "disabled")
 * @return Backend type, or SUMMARIZER_BACKEND_DISABLED if invalid
 */
summarizer_backend_t search_summarizer_parse_backend(const char *str);

/**
 * @brief Cleanup and free resources
 */
void search_summarizer_cleanup(void);

#endif  // SEARCH_SUMMARIZER_H
